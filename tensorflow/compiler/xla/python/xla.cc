/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/casts.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "pybind11/attr.h"
#include "pybind11/cast.h"
#include "pybind11/numpy.h"
#include "pybind11/pybind11.h"
#include "pybind11/pytypes.h"
#include "pybind11/stl_bind.h"
#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/pjrt/cpu_device.h"
#include "tensorflow/compiler/xla/pjrt/distributed/client.h"
#include "tensorflow/compiler/xla/pjrt/distributed/distributed.h"
#include "tensorflow/compiler/xla/pjrt/distributed/service.h"
#include "tensorflow/compiler/xla/pjrt/gpu_device.h"
#include "tensorflow/compiler/xla/pjrt/interpreter_device.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"
#include "tensorflow/compiler/xla/pjrt/tpu_client.h"
#include "tensorflow/compiler/xla/python/dlpack.h"
#include "tensorflow/compiler/xla/python/jax_jit.h"
#include "tensorflow/compiler/xla/python/ops.h"
#include "tensorflow/compiler/xla/python/outfeed_receiver_py.h"
#include "tensorflow/compiler/xla/python/profiler.h"
#include "tensorflow/compiler/xla/python/py_buffer.h"
#include "tensorflow/compiler/xla/python/py_executable.h"
#include "tensorflow/compiler/xla/python/py_traceback.h"
#include "tensorflow/compiler/xla/python/python_ref_manager.h"
#include "tensorflow/compiler/xla/python/pytree.h"
#include "tensorflow/compiler/xla/python/types.h"
#include "tensorflow/compiler/xla/python/xla_compiler.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/python/lib/core/bfloat16.h"
#include "tensorflow/stream_executor/platform.h"

namespace xla {
namespace {

namespace py = pybind11;

bool IsOptimizedBuild() {
#if NDEBUG
  return true;
#else
  return false;
#endif  // NDEBUG
}

StatusOr<py::object> BufferToPython(PyBuffer* buffer, py::handle& buffer_obj) {
  GlobalPyRefManager()->CollectGarbage();
  if (buffer->buffer()->IsOnCpu() &&
      buffer->buffer()->on_device_shape().IsArray() &&
      buffer->buffer()->on_device_shape().element_type() != BF16) {
    py::object out =
        py::reinterpret_steal<py::object>(PyArray_FROM_O(buffer_obj.ptr()));
    CHECK(out.ptr() != nullptr) << buffer->buffer()->on_host_shape().ToString(
        /*print_layout=*/true);
    return out;
  }
  std::shared_ptr<Literal> literal;
  {
    py::gil_scoped_release gil_release;
    TF_ASSIGN_OR_RETURN(literal, buffer->buffer()->ToLiteral());
  }
  return LiteralToPython(std::move(literal));
}

}  // namespace

PYBIND11_MODULE(xla_extension, m) {
  // Caution: import_array1 works by initializing a static variable
  // (PyArray_API) which is *defined* in a NumPy header. import_array1() must
  // therefore be called from the *same translation unit* as any users of
  // NumPy C APIs.
  auto init_numpy = []() -> bool {
    // import_array1 might look like a function. It's not. It's a macro that
    // calls `return`, which is why we wrap it in this strange-looking lambda.
    import_array1(false);
    return true;
  };
  if (!init_numpy()) {
    throw std::runtime_error("Unable to initialize Numpy API");
  }

  CHECK(tensorflow::RegisterNumpyBfloat16());

  // Types
  py::enum_<PrimitiveType>(m, "PrimitiveType")
      .value("PRIMITIVE_TYPE_INVALID", PRIMITIVE_TYPE_INVALID)
      .value("PRED", PRED)
      .value("S8", S8)
      .value("S16", S16)
      .value("S32", S32)
      .value("S64", S64)
      .value("U8", U8)
      .value("U16", U16)
      .value("U32", U32)
      .value("U64", U64)
      .value("F16", F16)
      .value("BF16", BF16)
      .value("F32", F32)
      .value("F64", F64)
      .value("C64", C64)
      .value("C128", C128)
      .value("TUPLE", TUPLE)
      .value("OPAQUE_TYPE", OPAQUE_TYPE)
      .value("TOKEN", TOKEN);

  m.def("bfloat16_dtype",
        []() { return py::handle(tensorflow::Bfloat16Dtype()); });

  // Must be before PyClient.compile.
  BuildXlaCompilerSubmodule(m);

  py::class_<PjRtDevice, ClientAndPtr<PjRtDevice>>(
      m, "Device",
      "A descriptor of an available device.\n\nSubclasses are used to "
      "represent specific types of devices, e.g. CPUs, GPUs. Subclasses may "
      "have additional properties specific to that device type.")
      .def_property_readonly(
          "id", &PjRtDevice::id,
          "Integer ID of this device.\n\nUnique across all available devices "
          "of this type, including remote devices on multi-host platforms.")
      .def_property_readonly("host_id", &PjRtDevice::host_id,
                             "Integer ID of this device's host.\n\n"
                             "This is always 0 except on multi-host platforms.")
      .def_property_readonly("platform",
                             [](const PjRtDevice& device) {
                               return device.client()->platform_name();
                             })
      .def_property_readonly("device_kind", &PjRtDevice::device_kind)
      .def_property_readonly(
          "client",
          [](const ClientAndPtr<PjRtDevice>& device) { return device.client; })
      .def("__str__", &PjRtDevice::DebugString)
      .def("transfer_to_infeed",
           [](const PjRtDevice& device, const LiteralSlice& literal) {
             GlobalPyRefManager()->CollectGarbage();
             py::gil_scoped_release gil_release;
             return device.TransferToInfeed(literal);
           })
      .def("transfer_from_outfeed",
           [](const PjRtDevice& device,
              const Shape& shape) -> StatusOr<py::object> {
             GlobalPyRefManager()->CollectGarbage();
             std::shared_ptr<Literal> literal_shared;
             {
               py::gil_scoped_release gil_release;
               Shape shape_with_layout = shape;
               ShapeUtil::ForEachMutableSubshape(
                   &shape_with_layout, [](Shape* subshape, const ShapeIndex&) {
                     if (!subshape->has_layout()) {
                       LayoutUtil::SetToDefaultLayout(subshape);
                     }
                   });
               TF_ASSIGN_OR_RETURN(Literal literal, device.TransferFromOutfeed(
                                                        shape_with_layout));

               literal_shared = std::make_shared<Literal>(std::move(literal));
             }
             return LiteralToPython(std::move(literal_shared));
           });

  py::class_<CpuDevice, PjRtDevice, ClientAndPtr<CpuDevice>>(m, "CpuDevice")
      .def("__repr__", [](const CpuDevice& device) {
        return absl::StrFormat("CpuDevice(id=%i)", device.id());
      });

  py::class_<GpuDevice, PjRtDevice, ClientAndPtr<GpuDevice>>(m, "GpuDevice")
      .def("__repr__", [](const GpuDevice& device) {
        return absl::StrFormat("GpuDevice(id=%i)", device.id());
      });

  py::class_<PjRtTpuDevice, PjRtDevice, ClientAndPtr<PjRtTpuDevice>>(
      m, "TpuDevice")
      .def_property_readonly("host_id", &PjRtTpuDevice::host_id)
      .def_property_readonly(
          "coords",
          [](const PjRtTpuDevice& device) -> pybind11::tuple {
            return IntSpanToTuple(device.coords());
          },
          "The coordinates of this TpuDevice's chip in the TPU mesh network.")
      .def_property_readonly(
          "core_on_chip", &PjRtTpuDevice::core_on_chip,
          "The index of this TpuDevice's core on the TPU chip.")
      .def("__repr__", [](const PjRtTpuDevice& device) {
        return absl::StrFormat(
            "TpuDevice(id=%i, host=%i, coords=(%s), core_on_chip=%i)",
            device.id(), device.host_id(), absl::StrJoin(device.coords(), ","),
            device.core_on_chip());
      });

  // Local XLA client methods.

  py::class_<GpuAllocatorConfig> alloc_config(m, "GpuAllocatorConfig");
  alloc_config.def(py::init<>())
      .def_readwrite("kind", &GpuAllocatorConfig::kind)
      .def_readwrite("memory_fraction", &GpuAllocatorConfig::memory_fraction)
      .def_readwrite("preallocate", &GpuAllocatorConfig::preallocate);
  py::enum_<GpuAllocatorConfig::Kind>(alloc_config, "Kind")
      .value("DEFAULT", GpuAllocatorConfig::Kind::kDefault)
      .value("PLATFORM", GpuAllocatorConfig::Kind::kPlatform)
      .value("BFC", GpuAllocatorConfig::Kind::kBFC);

  py::enum_<PjRtClient::HostBufferSemantics>(m, "HostBufferSemantics")
      .value("IMMUTABLE_ONLY_DURING_CALL",
             PjRtClient::HostBufferSemantics::kImmutableOnlyDuringCall)
      .value("IMMUTABLE_UNTIL_TRANSFER_COMPLETES",
             PjRtClient::HostBufferSemantics::kImmutableUntilTransferCompletes)
      .value("ZERO_COPY", PjRtClient::HostBufferSemantics::kZeroCopy);

  py::class_<PyClient, std::shared_ptr<PyClient>> py_local_client(m, "Client");
  py_local_client.def_property_readonly("platform", &PyClient::platform_name)
      .def("device_count", &PyClient::device_count)
      .def("local_device_count", &PyClient::addressable_device_count)
      .def("devices", &PyClient::Devices)
      .def("local_devices", &PyClient::LocalDevices)
      .def("host_id", &PyClient::host_id)
      .def("get_default_device_assignment",
           &PyClient::GetDefaultDeviceAssignment)
      // TODO(skye): delete after all callers can handle 2D output
      .def("get_default_device_assignment",
           &PyClient::GetDefaultDeviceAssignment1D)
      .def("create_channel_handle", &PyClient::CreateChannelHandle)
      .def("create_device_to_host_channel_handle",
           &PyClient::CreateDeviceToHostChannelHandle)
      .def("create_host_to_device_channel_handle",
           &PyClient::CreateHostToDeviceChannelHandle)
      .def("buffer_from_pyval", &PyClient::BufferFromPyval, py::arg("argument"),
           py::arg("device") = nullptr, py::arg("force_copy") = false,
           py::arg("host_buffer_semantics") =
               PjRtClient::HostBufferSemantics::kZeroCopy)
      .def("compile", &PyClient::Compile, py::arg("computation"),
           py::arg("compile_options") = CompileOptions())
      .def("heap_profile", &PyClient::HeapProfile);

  m.def(
      "get_cpu_client",
      [](bool asynchronous) -> StatusOr<std::shared_ptr<PyClient>> {
        TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtClient> client,
                            GetCpuClient(asynchronous));
        return std::make_shared<PyClient>(std::move(client));
      },
      py::arg("asynchronous") = true);
  m.def("get_interpreter_client", []() -> StatusOr<std::shared_ptr<PyClient>> {
    TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtClient> client,
                        GetInterpreterClient());
    return std::make_shared<PyClient>(std::move(client));
  });
  m.def(
      "get_gpu_client",
      [](bool asynchronous, const GpuAllocatorConfig& allocator_config,
         std::shared_ptr<DistributedRuntimeClient> distributed_client,
         int node_id) -> StatusOr<std::shared_ptr<PyClient>> {
        TF_ASSIGN_OR_RETURN(
            std::unique_ptr<PjRtClient> client,
            GetGpuClient(asynchronous, allocator_config,
                         std::move(distributed_client), node_id));
        return std::make_shared<PyClient>(std::move(client));
      },
      py::arg("asynchronous") = true,
      py::arg("allocator_config") = GpuAllocatorConfig(),
      py::arg("distributed_client") = nullptr, py::arg("node_id") = 0);
  m.def(
      "get_tpu_client",
      [](bool asynchronous) -> StatusOr<std::shared_ptr<PyClient>> {
        TF_ASSIGN_OR_RETURN(std::shared_ptr<PjRtClient> client,
                            GetTpuClient(asynchronous));
        return std::make_shared<PyClient>(std::move(client));
      },
      py::arg("asynchronous") = true);

  py::class_<DeviceArrayBase> device_array_base(m, "DeviceArrayBase");
  device_array_base.def(py::init<>());

  py::class_<PyBuffer, DeviceArrayBase, std::unique_ptr<PyBuffer>> buffer(
      m, "Buffer");
  // TODO(phawkins): alias for backward compatibility. Remove after JAX no
  // longer uses this name.
  m.add_object("PyLocalBuffer", buffer);
  buffer
      .def_property_readonly("__array_priority__",
                             [](py::object) { return 100; })
      .def_property("_device", &PyBuffer::GetStickyDevice,
                    &PyBuffer::SetStickyDevice)
      .def_property("aval", &PyBuffer::GetAval, &PyBuffer::SetAval)
      .def_property_readonly("_lazy_expr",
                             [](py::object buffer) { return py::none(); })
      .def_property_readonly("device_buffer",
                             [](py::object buffer) { return buffer; })
      .def_property_readonly(
          "shape",
          [](const PyBuffer& pybuffer) -> pybind11::tuple {
            return IntSpanToTuple(
                pybuffer.buffer()->on_host_shape().dimensions());
          })
      .def_property_readonly(
          "dtype",
          [](const PyBuffer& buffer) {
            PrimitiveType primitive =
                buffer.buffer()->on_host_shape().element_type();
            return PrimitiveTypeToDtype(primitive).ValueOrDie();
          })
      .def_property_readonly("size", &PyBuffer::size)
      .def_property_readonly("ndim", &PyBuffer::ndim)
      .def_property_readonly(
          "_value",
          [](py::handle buffer_obj) -> pybind11::object {
            PyBuffer* buffer = buffer_obj.cast<PyBuffer*>();
            if (buffer->is_deleted()) {
              throw std::runtime_error("DeviceArray has been deleted.");
            }
            py::object npy_value_ = buffer->GetNpyValue();
            if (npy_value_.is_none()) {
              npy_value_ = BufferToPython(buffer, buffer_obj).ValueOrDie();
              // TODO(jblspiau): Change `LiteralToPython` to return a
              // `py::array`, so we can set more easily the attribute.
              npy_value_.attr("flags").attr("writeable") = Py_False;
              buffer->SetNpyValue(npy_value_);
            }
            return npy_value_;
          })
      .def("copy_to_device", &PyBuffer::CopyToDevice)
      .def("on_device_size_in_bytes", &PyBuffer::OnDeviceSizeInBytes)
      .def("delete", &PyBuffer::Delete)
      // The GIL is released within BlockHostUntilReady.
      .def("block_until_ready",
           [](py::object buffer_obj) -> xla::StatusOr<py::object> {
             PyBuffer* buffer = buffer_obj.cast<PyBuffer*>();
             TF_RETURN_IF_ERROR(buffer->BlockHostUntilReady());
             return buffer_obj;
           })
      .def("block_host_until_ready", &PyBuffer::BlockHostUntilReady)
      .def("copy_to_host_async", &PyBuffer::CopyToHostAsync,
           py::call_guard<py::gil_scoped_release>())
      .def("to_py",
           [](py::handle buffer_obj) {
             PyBuffer* buffer = buffer_obj.cast<PyBuffer*>();
             return BufferToPython(buffer, buffer_obj);
           })
      .def("xla_shape", &PyBuffer::shape)
      .def_property_readonly("client", &PyBuffer::client)
      .def("device", &PyBuffer::device)
      .def("platform", &PyBuffer::platform_name)
      .def("is_deleted", &PyBuffer::is_deleted)
      .def("unsafe_buffer_pointer", &PyBuffer::UnsafeBufferPointer)
      .def_property_readonly("__cuda_array_interface__",
                             &PyBuffer::CudaArrayInterface)
      .def_property_readonly("traceback", &PyBuffer::traceback);

  // pybind11's implementation of the buffer protocol doesn't allow for correct
  // error handling. We bypass it and implement the buffer protocol ourselves.
  PyTypeObject* buffer_type = reinterpret_cast<PyTypeObject*>(buffer.ptr());
  buffer_type->tp_as_buffer = PyBuffer::BufferProtocol();

  py::class_<PyExecutable, std::shared_ptr<PyExecutable>> executable(
      m, "Executable");
  executable.def_property_readonly("client", &PyExecutable::client)
      .def("local_logical_device_ids",
           [](PyExecutable* exec) {
             auto span = exec->addressable_device_logical_ids();
             // Not on dispatch critical path, so ok to have heap allocation.
             std::vector<std::pair<int, int>> addressable_device_logic_ids;
             addressable_device_logic_ids.reserve(span.size());
             for (const auto& logical_device_id : span) {
               addressable_device_logic_ids.push_back(std::make_pair(
                   logical_device_id.replica, logical_device_id.partition));
             }
           })
      .def("local_devices", &PyExecutable::AddressableDevices)
      .def("size_of_generated_code_in_bytes",
           &PyExecutable::SizeOfGeneratedCodeInBytes)
      .def("delete", &PyExecutable::Delete)
      .def("execute", &PyExecutable::Execute, py::arg("arguments"))
      .def("execute_on_local_devices", &PyExecutable::ExecuteOnLocalDevices,
           py::arg("arguments"))
      .def("hlo_modules", &PyExecutable::HloModules)
      .def_property_readonly("traceback", &PyExecutable::traceback);

  m.def("buffer_to_dlpack_managed_tensor", BufferToDLPackManagedTensor,
        py::arg("buffer"), py::arg("take_ownership") = true);
  m.def("dlpack_managed_tensor_to_buffer", DLPackManagedTensorToBuffer);

  BuildProfilerSubmodule(&m);
  BuildOpsSubmodule(&m);
  BuildOutfeedReceiverSubmodule(&m);
  BuildPytreeSubmodule(m);
  BuildJaxjitSubmodule(m);
  BuildTracebackSubmodule(m);

  py::class_<DistributedRuntimeService,
             std::unique_ptr<DistributedRuntimeService>>
      distributed_runtime_service(m, "DistributedRuntimeService");
  py::class_<DistributedRuntimeClient,
             std::shared_ptr<DistributedRuntimeClient>>
      distributed_runtime_client(m, "DistributedRuntimeClient");
  distributed_runtime_client.def("connect", &DistributedRuntimeClient::Connect)
      .def("shutdown", &DistributedRuntimeClient::Shutdown);

  m.def("get_distributed_runtime_service", &GetDistributedRuntimeService);
  m.def("get_distributed_runtime_client", &GetDistributedRuntimeClient);

  m.def("collect_garbage", []() { GlobalPyRefManager()->CollectGarbage(); });

  m.def("is_optimized_build", &IsOptimizedBuild);
}  // NOLINT(readability/fn_size)

}  // namespace xla
