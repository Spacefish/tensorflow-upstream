/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/printer.h"

#include <string>
#include <utility>

#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"

namespace xla {

void StringPrinter::Append(absl::string_view s) {
  absl::StrAppend(&result_, s);
}

std::string StringPrinter::ToString() && { return std::move(result_); }

void CordPrinter::Append(absl::string_view s) { result_.Append(s); }

absl::Cord CordPrinter::ToCord() && { return std::move(result_); }

}  // namespace xla
