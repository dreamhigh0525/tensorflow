/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/tools/parser/hlo_lexer.h"

#include <unordered_map>

#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/gtl/optional.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/platform/regexp.h"

namespace xla {
namespace tools {

using tensorflow::StringPiece;

namespace {

constexpr int kEOF = -1;
constexpr int kError = -2;

// [a-zA-Z0-9_.-]
bool IsIdentifierChar(char c) {
  return isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ||
         c == '_';
}

}  // namespace

int HloLexer::GetNextChar() {
  int current_char = PeekCurrentChar();
  if (current_char != kEOF && current_char != kError) {
    current_ptr_++;
  }
  return current_char;
}

int HloLexer::PeekCurrentChar() const {
  if (current_ptr_ == buf_.end()) {
    return kEOF;
  }
  char current_char = *current_ptr_;
  if (current_char == 0) {
    // '\0' should not appear in the middle of the string.
    return kError;
  }
  return static_cast<unsigned char>(current_char);
}

bool HloLexer::CanDereference(const char* ptr) const {
  return ptr < buf_.end() && ptr >= buf_.begin();
}

StringPiece HloLexer::StringPieceFromPointers(const char* begin,
                                              const char* end) const {
  CHECK(begin <= end);
  CHECK(begin == buf_.end() || CanDereference(begin));
  CHECK(end == buf_.end() || CanDereference(end));
  return StringPiece(begin, end - begin);
}

tensorflow::RegexpStringPiece HloLexer::RegexpStringPieceFromPointers(
    const char* begin, const char* end) const {
  CHECK(begin <= end);
  CHECK(begin == buf_.end() || CanDereference(begin));
  CHECK(end == buf_.end() || CanDereference(end));
  return tensorflow::RegexpStringPiece(begin, end - begin);
}

TokKind HloLexer::LexToken() {
  while (true) {
    token_start_ = current_ptr_;

    int current_char = GetNextChar();
    switch (current_char) {
      default:
        // [a-zA-Z_]
        if (isalpha(static_cast<unsigned char>(current_char)) ||
            current_char == '_') {
          return LexIdentifier();
        }
        return TokKind::kError;
      case kEOF:
        // Hit the end of the input buffer.
        return TokKind::kEof;
      case kError:
        // Hit an invalid character in the input buffer.
        return TokKind::kError;
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        // Ignore whitespace.
        continue;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '-':
        if (current_char == '-' && PeekCurrentChar() == '>') {
          current_ptr_++;
          return TokKind::kArrow;
        }
        return LexDigitOrNegative();
      case '=':
        return TokKind::kEqual;
      case ',':
        return TokKind::kComma;
      case '%':
        return LexPercent();
      case ':':
        return TokKind::kColon;
      case '[':
        return TokKind::kLsquare;
      case ']':
        return TokKind::kRsquare;
      case '{':
        return TokKind::kLbrace;
      case '}':
        return TokKind::kRbrace;
      case '(':
        return TokKind::kLparen;
      case ')':
        return TokKind::kRparen;
    }
  }
}

// Lex a shape, name, keyword, or opcode.
// shape    ::= ([a-zA-Z0-9_]*[0-9]*)\[([0-9,]*)\](?:\s*{([0-9,]*)})?
// name     ::= [a-zA-Z_][a-zA-Z0-9_.-]*:
// keyword  ::= HloModule, ENTRY, ...
// opcode   ::= add, greater-than, ...
TokKind HloLexer::LexIdentifier() {
  {
    auto consumable = RegexpStringPieceFromPointers(token_start_, buf_.end());
    // 'consumable' will be advanced iff its prefix matches the pattern.
    static LazyRE2 shape_pattern = {
        R"(^(\w*\d*)\[([\d,]*)\](?:\s*{([\d,]*)})?)"};
    if (RE2::Consume(&consumable, *shape_pattern)) {
      auto status_or_shape = ShapeUtil::ParseShapeString(
          StringPieceFromPointers(token_start_, consumable.begin()));
      if (status_or_shape.ok()) {
        // This is a shape string.
        shape_val_ = status_or_shape.ValueOrDie();
        current_ptr_ = consumable.begin();
        return TokKind::kShape;
      }
    }
  }

  while (IsIdentifierChar(PeekCurrentChar())) {
    current_ptr_++;
  }

  // If followed by ':', it's a name.
  if (PeekCurrentChar() == ':') {
    str_val_.assign(token_start_, current_ptr_);
    current_ptr_++;  // skip ':'
    return TokKind::kName;
  }

  StringPiece identifier = StringPieceFromPointers(token_start_, current_ptr_);

  // See if this is a keyword.
#define KEYWORD(STR)            \
  do {                          \
    if (identifier == #STR) {   \
      return TokKind::kw_##STR; \
    }                           \
  } while (false)

  KEYWORD(true);
  KEYWORD(false);
  KEYWORD(HloModule);
  KEYWORD(ENTRY);

#undef KEYWORD

  // See if this is an opcode.
  auto opcode = StringToHloOpcode(identifier.ToString());
  if (opcode.ok()) {
    opcode_val_ = opcode.ValueOrDie();
    return TokKind::kOpcode;
  }

  current_ptr_ = token_start_ + 1;
  return TokKind::kError;
}

// Lex names after a % character.
// name ::= [a-zA-Z_][a-zA-Z0-9_.-]*
TokKind HloLexer::LexPercent() {
  const char* name_start = current_ptr_;
  if (isalpha(static_cast<unsigned char>(PeekCurrentChar())) ||
      PeekCurrentChar() == '_') {
    current_ptr_++;
    while (IsIdentifierChar(PeekCurrentChar())) {
      current_ptr_++;
    }
    str_val_.assign(name_start, current_ptr_);
    return TokKind::kName;
  }
  return TokKind::kError;
}

// Lex integer and floating-point values.
// int             [-]?[0-9]+
// fp with exp     [-]?([0-9]+|[0-9]+[.][0-9]*|[0-9]*[.][0-9]+)([eE][+-]?[0-9]+)
// fp without exp  [-]?([0-9]+[.][0-9]*|[0-9]*[.][0-9]+)
TokKind HloLexer::LexDigitOrNegative() {
  auto consumable = RegexpStringPieceFromPointers(token_start_, buf_.end());
  static LazyRE2 float_pattern = {
      R"([-]?((\d+|\d+[.]\d*|\d*[.]\d+)([eE][+-]?\d+))|(\d+[.]\d*|\d*[.]\d+))"};
  if (RE2::Consume(&consumable, *float_pattern)) {
    current_ptr_ = consumable.begin();
    tensorflow::strings::safe_strtod(string(token_start_, current_ptr_).c_str(),
                                     &decimal_val_);
    return TokKind::kDecimal;
  }

  static LazyRE2 int_pattern = {R"([-]?\d+)"};
  if (RE2::Consume(&consumable, *int_pattern)) {
    current_ptr_ = consumable.begin();
    tensorflow::strings::safe_strto64(
        StringPieceFromPointers(token_start_, current_ptr_), &int64_val_);
    return TokKind::kInt;
  }

  return TokKind::kError;
}

StringPiece HloLexer::GetCurrentLine() const {
  const char* start = token_start_;
  const char* end = current_ptr_;
  if (!CanDereference(start) || !CanDereference(end)) {
    return "LINE OUT OF RANGE";
  }
  while (start > buf_.begin() && *start != '\n') {
    start--;
  }
  while (end < buf_.end() && *end != '\n') {
    end++;
  }
  return StringPieceFromPointers(start, end);
}

}  // namespace tools
}  // namespace xla
