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

#ifndef TENSORFLOW_CORE_UTIL_DEBUG_EVENTS_WRITER_H_
#define TENSORFLOW_CORE_UTIL_DEBUG_EVENTS_WRITER_H_

#include <deque>

#include "absl/container/flat_hash_map.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/io/record_writer.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/protobuf/debug_event.pb.h"

namespace tensorflow {
namespace tfdbg {

// The set of files generated by a debugged TensorFlow program.
enum DebugEventFileType {
  METADATA,
  SOURCE_FILES,
  STACK_FRAMES,
  GRAPHS,
  EXECUTION,
  GRAPH_EXECUTION_TRACES,
};

// Helper class for DebugEventsWriter.
// This class manages the writing of data to a single TFRecord file.
// Each object of the DebugEventsWriter class below involves multiple
// TFRecord files, and hence utilizes multiple objects of this helper class.
class SingleDebugEventFileWriter {
 public:
  explicit SingleDebugEventFileWriter(const string& file_path);

  Status Init();

  void WriteSerializedDebugEvent(tensorflow::StringPiece debug_event_str);

  Status Flush();
  Status Close();

  const string FileName();

 private:
  Env* env_;
  const string file_path_;
  std::atomic_int_fast32_t num_outstanding_events_;

  std::unique_ptr<WritableFile> writable_file_;
  std::unique_ptr<io::RecordWriter> record_writer_ PT_GUARDED_BY(writer_mu_);
  mutex writer_mu_;
};

// The DebugEvents writer class.
class DebugEventsWriter {
 public:
#ifndef SWIG
  // Prefix of version string present in the first entry of every event file.
  // Default size of each circular buffer (unit: number of DebugEvent protos).
  static constexpr const int64 kDefaultCyclicBufferSize = 1000;

  static constexpr const char* kFileNamePrefix = "tfdbg_events";
  static constexpr const char* kMetadataSuffix = "metadata";
  static constexpr const char* kSourceFilesSuffix = "source_files";
  static constexpr const char* kStackFramesSuffix = "stack_frames";
  static constexpr const char* kGraphsSuffix = "graphs";
  static constexpr const char* kExecutionSuffix = "execution";
  static constexpr const char* kGraphExecutionTracesSuffix =
      "graph_execution_traces";

  static constexpr const char* kVersionPrefix = "debug.Event:";
  static constexpr const int kCurrentFormatVersion = 1;
#endif

  // Get the DebugEventsWriter for the given dump_root.
  // For a given dump_root value, it is a singleton. tfdbg event files come in
  // sets of six. The singleton pattern avoids storing multiple sets in a single
  // folder, which might cause confusion.
  //
  // Args:
  //   dump_root: Dump root directory. If it doesn't exist, will be created.
  //   circular_buffer_size: Circular buffer size (in number of DebugEvent
  //     protos). If set to a value <=0, will abolish the circular-buffer
  //     behavior.
  // Returns:
  //   A pointer to a DebugEventsWriter object: a per-dump_root singleton.
  static DebugEventsWriter* GetDebugEventsWriter(const string& dump_root,
                                                 int64 circular_buffer_size);
  // Same as the 2-arg factory method above, but uses the default circular
  // buffer size.
  static DebugEventsWriter* GetDebugEventsWriter(const string& dump_root);
  ~DebugEventsWriter();

  // Sets the debug event filenames and opens file for writing.
  // All files (see the DebugEventFileType enum) share the same prefix and
  // differ only in their suffixes. If not called by user, will be invoked
  // automatically by a call to FileName() or any of the Write*() methods().
  // Idempotent: if the metadata file exists and is open, this is a no-op.
  // If on the other hand the file was opened, but has since disappeared (e.g.
  // deleted by another process), this will open a new file.
  Status Init();

  // The four DebugEvent fields below are written _without_ the circular buffer.
  // Source file contents are written to the *.source_files file.
  // Takes ownership of source_file.
  void WriteSourceFile(SourceFile* source_file);
  // Stack frames are written to the *.code_locations file.
  // Takes ownership of stack_frame_with_id.
  void WriteStackFrameWithId(StackFrameWithId* stack_frame_with_id);
  // Graph op creation events are written to the *.graphs file.
  // Takes ownership of graph_op_creation.
  void WriteGraphOpCreation(GraphOpCreation* graph_op_creation);
  // Debugged graphs are written to the *.graphs file.
  // Takes ownership of debugged_graph.
  void WriteDebuggedGraph(DebuggedGraph* debugged_graph);

  // The two DebugEvent fields below are written to the circular buffer
  // and saved to disk only at the FlushExecutionFiles() call.
  // Execution events (eager execution of an op or a tf.function) are written to
  // the *.execution file.
  // Takes ownership of execution.
  void WriteExecution(Execution* execution);
  // Graph execution traces (graph-internal tensor values or their summaries)
  // are written to the *.graph_execution_traces file.
  // Takes ownership of graph_execution_trace.
  void WriteGraphExecutionTrace(GraphExecutionTrace* graph_execution_trace);

  // Write a graph execution trace without using a protocol buffer.
  // Instead, pass the raw values related to the graph execution trace.
  // Args:
  //   tfdbg_context_id: A unique ID for the context of interest, e.g., a
  //   concreted compiled tf.function that the op of interest belongs to.
  //   op_name: Name of the op that this graph execution trace is concerned
  //     with. Applicable only to the single-tensor trace case. For cases in
  //     which the trace concerns multiple tensors, this is an empty string.
  //   output_slot: Output slot index of the op that this trace is concerned
  //     with.
  //   tensor_debug_mode: An integer that represents the tensor-debug mode enum.
  //   tensor_value: The value of the tensor that describes the tensor(s)
  //     that this trace is concerned with. The semantics of this tensor value
  //     depends on the value of `tensor_debug_mode`.
  void WriteGraphExecutionTrace(const string& tfdbg_context_id,
                                const string& device_name,
                                const string& op_name, int32 output_slot,
                                int32 tensor_debug_mode,
                                const Tensor& tensor_value);

  // Writes a serialized DebugEvent to one of the debug-events files
  // concerned with the non-execution events: the SOURCE_FILES, STACK_FRAMES
  // and GRAPHS files.
  // NOTE: Actually used in the Python binding, to avoid overhead of
  // serializing and parsing protos at the language interface.
  void WriteSerializedNonExecutionDebugEvent(const string& debug_event_str,
                                             DebugEventFileType type);

  // Writes a serialized DebugEvent to one of the debug-events files
  // concerned with the execution-related events: the EXECUTION and
  // GRAPH_EXECUTION_TRACES files. This involves the cyclic-buffer behavior if
  // circular_buffer_size is configured to be >0.
  // NOTE: Actually used in the Python binding, to avoid overhead of
  // serializing and parsing protos at the language interface.
  void WriteSerializedExecutionDebugEvent(const string& debug_event_str,
                                          DebugEventFileType type);

  // Given name of the device, retrieve a unique integer ID. As a side effect,
  // if this is the first time this object encounters the device name,
  // writes a DebuggedDevice proto to the .graphs file in the file set.
  int RegisterDeviceAndGetId(const string& device_name);

  // EventWriter automatically flushes and closes on destruction, but
  // this method is provided for users who want to write to disk sooner
  // and/or check for success.
  // FlushNonExecutionFiles() pushes outstanding DebugEvents not written
  // events to the circular buffer to their respective files.
  Status FlushNonExecutionFiles();

  // Writes current contents of the circular buffers to their respective
  // debug event files and clears the circular buffers.
  Status FlushExecutionFiles();

  // Close() calls FlushNonExecutionFiles() and FlushExecutionFiles()
  // and then closes the current debug events files.
  Status Close();

 private:
  static std::unordered_map<string, std::unique_ptr<DebugEventsWriter>>*

  // Get a static map from dump-root path to DebugEventsWriter objects.
  // This helps the per-dump-root singletone pattern.
  GetDebugEventsWriterMap();

  // Guards calls to the GetDebugEventsWriter() method.
  static mutex factory_mu_;

  DebugEventsWriter(const string& dump_root, int64 circular_buffer_size);

  // Get the path prefix. The same for all files, which differ only in the
  // suffix.
  string FileName(DebugEventFileType type);

  // Initialize the TFRecord writer for non-metadata file type.
  Status InitNonMetadataFile(DebugEventFileType type);

  void SerializeAndWriteDebugEvent(DebugEvent* debug_event,
                                   DebugEventFileType type);

  void SelectWriter(DebugEventFileType type,
                    std::unique_ptr<SingleDebugEventFileWriter>** writer);
  const string GetSuffix(DebugEventFileType type);
  string GetFileNameInternal(DebugEventFileType type);

  Env* env_;
  const string dump_root_;

  string file_prefix_;
  bool is_initialized_ GUARDED_BY(initialization_mu_);
  mutex initialization_mu_;

  const int64 circular_buffer_size_;
  std::deque<string> execution_buffer_ GUARDED_BY(execution_buffer_mu_);
  mutex execution_buffer_mu_;
  std::deque<string> graph_execution_trace_buffer_
      GUARDED_BY(graph_execution_trace_buffer_mu_);
  mutex graph_execution_trace_buffer_mu_;

  absl::flat_hash_map<string, int> device_name_to_id_ GUARDED_BY(device_mu_);
  mutex device_mu_;

  std::unique_ptr<SingleDebugEventFileWriter> metadata_writer_;
  std::unique_ptr<SingleDebugEventFileWriter> source_files_writer_;
  std::unique_ptr<SingleDebugEventFileWriter> stack_frames_writer_;
  std::unique_ptr<SingleDebugEventFileWriter> graphs_writer_;
  std::unique_ptr<SingleDebugEventFileWriter> execution_writer_;
  std::unique_ptr<SingleDebugEventFileWriter> graph_execution_traces_writer_;

  TF_DISALLOW_COPY_AND_ASSIGN(DebugEventsWriter);

  friend class DebugEventsWriterTest;
};

}  // namespace tfdbg
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_UTIL_DEBUG_EVENTS_WRITER_H_
