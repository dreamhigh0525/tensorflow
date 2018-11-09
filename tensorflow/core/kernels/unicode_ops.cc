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

#include <memory>
#include <string>

#include "unicode/ucnv.h"  // TF:icu
#include "unicode/ucnv_err.h"  // TF:icu
#include "unicode/umachine.h"  // TF:icu
#include "unicode/uniset.h"  // TF:icu
#include "unicode/unistr.h"  // TF:icu
#include "unicode/uset.h"  // TF:icu
#include "unicode/utypes.h"  // TF:icu
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/kernels/string_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/util/bcast.h"
#include "tensorflow/core/util/ptr_util.h"

namespace tensorflow {

// This error callback is only useful for finding illegal encoding errors when
// we want to be strict -- otherwise illegal encodings are replaced on read
// with 0xFFFD and signaled to the callback.
void unicode_error_callback(const void* context, UConverterToUnicodeArgs* args,
                            const char* codeUnits, int32_t length,
                            UConverterCallbackReason reason,
                            UErrorCode* pErrorCode) {
  // Careful: this depends on setting up the context settings when the
  // callback is registered.
  bool* format_error = const_cast<bool*>(static_cast<const bool*>(context));

  if (reason == UCNV_UNASSIGNED || reason == UCNV_ILLEGAL ||
      reason == UCNV_IRREGULAR) {
    *format_error = true;
  }

  // Side note: the default behavior in this case is that without a substitution
  // made by the callback, the UConverter will signal an error to the iterator
  // making the string iteration bail out. Instead, forward to the built-in
  // substitution handler.
  UCNV_TO_U_CALLBACK_SUBSTITUTE(nullptr, args, codeUnits, length, reason,
                                pErrorCode);
}

// Iterates through a source string given the provided input UConverter specific
// to the encoding for that string. Calls a provided callback for each codepoint
// consumed. Provides the callback with the codepoint and the number of bytes
// consumed from the input string to produce it. If there are invalid encoding
// loci in the source string, they will be provided as a 0xFFFD codepoint to
// the callback, unless the "fail_on_formatting_error" arg is set, in which
// case the callback will be passed the signal that there is such an invalid
// encoding position.
// callback: function(UChar32 codepoint, int num_bytes_consumed_from_source_str,
//                    bool fatal_format_error)
void IterateUnicodeString(const string& str, UConverter* converter,
                          std::function<void(UChar32, int, bool)> callback) {
  const char* source = str.data();
  const char* limit = str.data() + str.length();
  UErrorCode status = U_ZERO_ERROR;

  UConverterToUCallback oldAction = nullptr;
  const void* oldContext = nullptr;
  bool format_error = false;

  // Subtle. You can't make a function pointer from a std::function. :-(
  // Instead, we pass the boolean pointer as the "context" object.
  ucnv_setToUCallBack(converter, unicode_error_callback, &format_error,
                      &oldAction, &oldContext, &status);
  if (U_FAILURE(status)) {
    LOG(ERROR) << "Could not set unicode error callback on converter";
    return;
  }

  while (source < limit) {
    const char* source_pre_fetch = source;
    // Note: ucnv_getNextUChar returns 0xFFFD on an encoding error.
    UChar32 next_char = ucnv_getNextUChar(converter, &source, limit, &status);
    if (U_FAILURE(status)) {
      source = limit;
    }
    int bytes_consumed = source - source_pre_fetch;
    callback(next_char, bytes_consumed, format_error);
    format_error = false;
  }

  ucnv_setToUCallBack(converter, oldAction, oldContext, nullptr, nullptr,
                      &status);
}

// Lifecycle wrapper for UConverter making it easier to use with thread_local.
// TODO(gbillock): Consider whether to use the higher-level convert API and
// create a specialized fast code path for UTF8.
class WrappedConverter {
 public:
  WrappedConverter() {}

  ~WrappedConverter() {
    if (converter_) {
      ucnv_close(converter_);
    }
  }

  void init(const string& name) {
    if (converter_ && name == name_) {
      // Note: this reset is not typically needed, but if not done, then in some
      // cases the cached converter will maintain state of input endianness
      // which isn't valid from input to input in every batched case.
      ucnv_reset(converter_);
      return;
    }

    if (converter_) {
      ucnv_close(converter_);
      converter_ = nullptr;
      name_ = "";
    }

    UErrorCode status = U_ZERO_ERROR;
    converter_ = ucnv_open(name.c_str(), &status);
    if (U_FAILURE(status)) {
      if (converter_) {
        ucnv_close(converter_);
        converter_ = nullptr;
      }
    } else {
      name_ = name;
    }
  }

  UConverter* converter_ = nullptr;
  string name_;
};

class UnicodeTranscodeOp : public OpKernel {
 public:
  explicit UnicodeTranscodeOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    string error_policy;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("errors", &error_policy));
    if (error_policy == "replace") {
      elide_replacement_ = false;
    } else if (error_policy == "ignore") {
      elide_replacement_ = true;
    } else if (error_policy == "strict") {
      error_on_malformatting_ = true;
    } else {
      ctx->CtxFailure(errors::InvalidArgument(
          "errors policy must be one of 'strict', 'replace', or 'ignore'"));
    }

    int32 replacement_char;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("replacement_char", &replacement_char));
    if (replacement_char >= UCHAR_MIN_VALUE &&
        replacement_char <= UCHAR_MAX_VALUE) {
      subst_ = replacement_char;
    } else {
      ctx->CtxFailure(errors::InvalidArgument(
          "replacement_char out of unicode codepoint range"));
    }

    string output_encoding;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("output_encoding", &output_encoding));
    OP_REQUIRES_OK(ctx,
                   ParseUnicodeEncoding(output_encoding, &output_encoding_));

    OP_REQUIRES_OK(ctx, ctx->GetAttr("replace_control_characters",
                                     &replace_control_chars_));

    OP_REQUIRES_OK(ctx, ctx->GetAttr("input_encoding", &input_encoding_));
    // Make a temporary UConverter to ensure it will create without error
    // at execution time (and to warm any data caches the converter needs).
    // This instance is not used.
    std::unique_ptr<WrappedConverter> input_encoder =
        absl::make_unique<WrappedConverter>();
    input_encoder->init(input_encoding_);
    OP_REQUIRES(ctx, input_encoder->converter_,
                errors::InvalidArgument(
                    "Could not create converter for input encoding: " +
                    input_encoding_));
  }

  void Compute(OpKernelContext* ctx) override {
    const Tensor* input_tensor;
    OP_REQUIRES_OK(ctx, ctx->input("input", &input_tensor));

    static thread_local std::unique_ptr<WrappedConverter> input_encoder;
    if (!input_encoder) {
      input_encoder.reset(new WrappedConverter());
    }
    input_encoder->init(input_encoding_);
    OP_REQUIRES(ctx, input_encoder->converter_,
                errors::InvalidArgument(
                    "Could not create converter for input encoding: " +
                    input_encoding_));

    // Output may be forwardable from input, in which case work in-place.
    Tensor* output_tensor;
    std::unique_ptr<Tensor> maybe_forwarded =
        ctx->forward_input(0 /*input_index*/, 0 /*output_index*/,
                           tensorflow::DT_STRING, input_tensor->shape(),
                           ctx->input_memory_type(0), ctx->input_alloc_attr(0));
    if (maybe_forwarded) {
      output_tensor = maybe_forwarded.get();
      OP_REQUIRES_OK(ctx, ctx->set_output("output", *output_tensor));
    } else {
      OP_REQUIRES_OK(ctx, ctx->allocate_output("output", input_tensor->shape(),
                                               &output_tensor));
      output_tensor->flat<string>() = input_tensor->flat<string>();
    }

    auto output_flat = output_tensor->flat<string>();
    bool found_any_format_error = false;
    for (size_t i = 0; i < output_flat.size(); ++i) {
      Transcode(&(output_flat(i)), input_encoder->converter_,
                &found_any_format_error);
    }
    if (error_on_malformatting_ && found_any_format_error) {
      ctx->CtxFailure(
          errors::InvalidArgument("Invalid formatting on input string"));
    }
  }

 private:
  // Consume a codepoint from the input string and add it to the buffer.
  // This function takes care of any replacement configuration on invalid or
  // out-of-range inputs.
  void TranslateCodepoints(icu::UnicodeString* s, bool* found_any_format_error,
                           UChar32 ch, int src_bytes, bool format_error) {
    if ((replace_control_chars_ && ch <= 0x1F) || format_error) {
      *found_any_format_error = true;
      if (elide_replacement_) {
        return;
      } else {
        ch = subst_;
      }
    }
    s->append(ch);
  }

  // Transcode the string from input encoding to the output_encoding_. If
  // non-valid characters are encountered, use the subst_/elide_replacement_
  // config to handle them.
  void Transcode(string* s, UConverter* input_encoder,
                 bool* found_any_format_error) {
    icu::UnicodeString source;
    IterateUnicodeString(
        *s, input_encoder,
        std::bind(&UnicodeTranscodeOp::TranslateCodepoints, this, &source,
                  found_any_format_error, std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));

    if (output_encoding_ == UnicodeEncoding::UTF8) {
      s->clear();
      source.toUTF8String(*s);
    } else if (output_encoding_ == UnicodeEncoding::UTF16BE) {
      // TODO(gbillock): consider using the
      // extract(char *dest, int32_t destCapacity, UConverter *cnv)
      // for UTF16/32
      s->clear();  // subtle: must come before reserve()
      s->reserve(2 * source.length() + 1);
      const char16_t* buf = source.getBuffer();
      for (int i = 0; i < source.length(); ++i) {
        // Emit big-endian encoding for UTF-16 always.
        s->push_back((buf[i] & 0xFF00) >> 8);
        s->push_back(buf[i] & 0x00FF);
      }
    } else if (output_encoding_ == UnicodeEncoding::UTF32BE) {
      s->clear();  // subtle: must come before reserve()
      s->reserve(4 * source.countChar32() + 1);
      for (int i = 0; i < source.countChar32(); ++i) {
        // Emit big-endian encoding for UTF-32 always.
        UChar32 ch = source.char32At(i);
        s->push_back((ch & 0xFF000000) >> 24);
        s->push_back((ch & 0x00FF0000) >> 16);
        s->push_back((ch & 0x0000FF00) >> 8);
        s->push_back((ch & 0x000000FF));
      }
    }
  }

  UChar32 subst_ = 0xFFFD;
  bool elide_replacement_ = false;
  bool replace_control_chars_ = false;
  bool error_on_malformatting_ = false;

  string input_encoding_;
  UnicodeEncoding output_encoding_ = UnicodeEncoding::UTF8;
};

REGISTER_KERNEL_BUILDER(Name("UnicodeTranscode").Device(DEVICE_CPU),
                        UnicodeTranscodeOp);

}  // namespace tensorflow
