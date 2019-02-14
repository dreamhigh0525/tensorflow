/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

/* Wrap trt_conversion */
%{
#define SWIG_FILE_WITH_INIT
%}
%include "std_string.i"
%include "tensorflow/python/platform/base.i"

%{
struct version_struct{
  int vmajor;
  int vminor;
  int vpatch;
};

PyObject* version_helper(version_struct* in) {
  PyObject *tuple(nullptr);
  tuple = Py_BuildValue("(iii)", in->vmajor, in->vminor, in->vpatch);
  if (!tuple) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError,
                      "Tuple creation from version structure failed!");
    }
    return NULL;
  }
  return tuple;
}

/* Define converters for vector<int> */
template<>
bool _PyObjAs(PyObject *pyobj, int* dest) {
  *dest = PyLong_AsLong(pyobj);
  return true;
}

template<>
PyObject *_PyObjFrom(const int& src) {
  return PyLong_FromLong(src);
}

%}

_LIST_OUTPUT_TYPEMAP(int, PyLong_FromLong);

%typemap(out) version_struct {
  PyObject *tuple = version_helper(&$1);
  if (!tuple) SWIG_fail;
  $result = tuple;
}

%{
#include "tensorflow/compiler/tf2tensorrt/convert/convert_graph.h"
#include "tensorflow/compiler/tf2tensorrt/convert/utils.h"
#include "tensorflow/compiler/tf2tensorrt/utils/test_utils.h"
%}

%ignoreall
%unignore tensorflow;
%unignore get_linked_tensorrt_version;
%unignore get_loaded_tensorrt_version;
%unignore is_tensorrt_enabled;
%unignore enable_test_value;
%unignore clear_test_values;
%unignore add_test_value;
%unignore get_test_value;

%{

version_struct get_linked_tensorrt_version() {
  // Return the version at the link time.
  version_struct s;
#if GOOGLE_CUDA && GOOGLE_TENSORRT
  const auto &lv = tensorflow::tensorrt::convert::GetLinkedTensorRTVersion();
  s.vmajor = lv[0];
  s.vminor = lv[1];
  s.vpatch = lv[2];
#endif  // GOOGLE_CUDA && GOOGLE_TENSORRT
  return s;
}

version_struct get_loaded_tensorrt_version() {
  // Return the version from the loaded library.
  version_struct s;
#if GOOGLE_CUDA && GOOGLE_TENSORRT
  const auto &lv = tensorflow::tensorrt::convert::GetLoadedTensorRTVersion();
  s.vmajor = lv[0];
  s.vminor = lv[1];
  s.vpatch = lv[2];
#endif  // GOOGLE_CUDA && GOOGLE_TENSORRT
  return s;
}

bool is_tensorrt_enabled() {
  return tensorflow::tensorrt::IsGoogleTensorRTEnabled();
}

void enable_test_value() {
  tensorflow::tensorrt::test::EnableTestValue();
}

#if PY_MAJOR_VERSION < 3
#define TRT_PY_TO_CPP_STRING PyString_AsString
#define TRT_CPP_TO_PY_STRING PyString_FromString
#else
#define TRT_PY_TO_CPP_STRING PyUnicode_AsUTF8
#define TRT_CPP_TO_PY_STRING PyUnicode_FromString
#endif

void clear_test_values(PyObject* pattern) {
  tensorflow::tensorrt::test::ClearTestValues(
      string(TRT_PY_TO_CPP_STRING(pattern)));
}

void add_test_value(PyObject* label, PyObject* value) {
  tensorflow::tensorrt::test::AddTestValue(
      string(TRT_PY_TO_CPP_STRING(label)), string(TRT_PY_TO_CPP_STRING(value)));
}

PyObject* get_test_value(PyObject* label) {
  string value = tensorflow::tensorrt::test::GetTestValue(
      string(TRT_PY_TO_CPP_STRING(label)));
  return TRT_CPP_TO_PY_STRING(value.c_str());
}

%}

version_struct get_linked_tensorrt_version();
version_struct get_loaded_tensorrt_version();
bool is_tensorrt_enabled();
void enable_test_value();
void clear_test_values(PyObject* pattern);
void add_test_value(PyObject* label, PyObject* value);
PyObject* get_test_value(PyObject* label);

%unignoreall
