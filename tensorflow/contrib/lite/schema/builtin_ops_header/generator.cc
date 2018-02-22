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
#include "tensorflow/contrib/lite/schema/builtin_ops_header/generator.h"
#include "tensorflow/contrib/lite/schema/schema_generated.h"

namespace tflite {
namespace builtin_ops_header {

namespace {
const char* kFileHeader =
    R"(/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CONTRIB_LITE_BUILTIN_OPS_H_
#define TENSORFLOW_CONTRIB_LITE_BUILTIN_OPS_H_

// DO NOT EDIT MANUALLY: This file is automatically generated by
// `schema_builtin_ops_header_generator.py`.

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// The enum for builtin operators.
// Note: CUSTOM and DELEGATE are 2 special ops which are not real biultin
// ops.
typedef enum {
)";

const char* kFileFooter =
    R"(} TfLiteBuiltinOperator;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus
#endif  // TENSORFLOW_CONTRIB_LITE_BUILTIN_OPS_H_
}
)";
}  // anonymous namespace

bool IsValidInputEnumName(const std::string& name) {
  const char* begin = name.c_str();
  const char* ch = begin;
  while (*ch != '\0') {
    // If it's not the first character, expect an underscore.
    if (ch != begin) {
      if (*ch != '_') {
        return false;
      }
      ++ch;
    }

    // Expecting a word with upper case letters or digits, like "CONV",
    // "CONV2D", "2D"...etc.
    bool empty = true;
    while (isupper(*ch) || isdigit(*ch)) {
      // It's not empty if at least one character is consumed.
      empty = false;
      ++ch;
    }
    if (empty) {
      return false;
    }
  }
  return true;
}

std::string ConstantizeVariableName(const std::string& name) {
  std::string result = "kTfLiteBuiltin";
  bool uppercase = true;
  for (char input_char : name) {
    if (input_char == '_') {
      uppercase = true;
    } else if (uppercase) {
      result += toupper(input_char);
      uppercase = false;
    } else {
      result += tolower(input_char);
    }
  }

  return result;
}

bool GenerateHeader(std::ostream& os) {
  auto enum_names = tflite::EnumNamesBuiltinOperator();

  // Check if all the input enum names are valid.
  for (auto enum_value : EnumValuesBuiltinOperator()) {
    auto enum_name = enum_names[enum_value];
    if (!IsValidInputEnumName(enum_name)) {
      std::cerr << "Invalid input enum name: " << enum_name << std::endl;
      return false;
    }
  }

  os << kFileHeader;
  for (auto enum_value : EnumValuesBuiltinOperator()) {
    auto enum_name = enum_names[enum_value];
    os << "  ";
    os << ConstantizeVariableName(enum_name);
    os << " = ";
    os << enum_value;
    os << ",\n";
  }
  os << kFileFooter;
  return true;
}

}  // namespace builtin_ops_header
}  // namespace tflite
