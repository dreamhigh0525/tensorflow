/* Copyright 2015 Google Inc. All Rights Reserved.

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

// Printf variants that place their output in a C++ string.
//
// Usage:
//      string result = strings::Printf("%d %s\n", 10, "hello");
//      strings::SPrintf(&result, "%d %s\n", 10, "hello");
//      strings::Appendf(&result, "%d %s\n", 20, "there");

#ifndef TENSORFLOW_LIB_STRINGS_STRINGPRINTF_H_
#define TENSORFLOW_LIB_STRINGS_STRINGPRINTF_H_

#include <stdarg.h>
#include <string>
#include <vector>

#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/port.h"

namespace tensorflow {
namespace strings {

// Return a C++ string
extern string Printf(const char* format, ...)
    // Tell the compiler to do printf format string checking.
    TF_PRINTF_ATTRIBUTE(1, 2);

// Append result to a supplied string
extern void Appendf(string* dst, const char* format, ...)
    // Tell the compiler to do printf format string checking.
    TF_PRINTF_ATTRIBUTE(2, 3);

// Lower-level routine that takes a va_list and appends to a specified
// string.  All other routines are just convenience wrappers around it.
extern void Appendv(string* dst, const char* format, va_list ap);

}  // namespace strings
}  // namespace tensorflow

#endif  // TENSORFLOW_LIB_STRINGS_STRINGPRINTF_H_
