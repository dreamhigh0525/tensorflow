/* Copyright 2018 Google Inc. All Rights Reserved.

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

#ifndef TENSORFLOW_TSL_PLATFORM_WINDOWS_WIDE_CHAR_H_
#define TENSORFLOW_TSL_PLATFORM_WINDOWS_WIDE_CHAR_H_

#include <string>

namespace tsl {

std::wstring Utf8ToWideChar(const std::string& utf8str);

std::string WideCharToUtf8(const std::wstring& wstr);

}  // namespace tsl

#endif  // TENSORFLOW_TSL_PLATFORM_WINDOWS_WIDE_CHAR_H_
