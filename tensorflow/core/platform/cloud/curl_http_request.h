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

#ifndef TENSORFLOW_CORE_PLATFORM_CLOUD_CURL_HTTP_REQUEST_H_
#define TENSORFLOW_CORE_PLATFORM_CLOUD_CURL_HTTP_REQUEST_H_

#include <string>
#include <unordered_map>
#include <vector>
#include <curl/curl.h>
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/platform/cloud/http_request.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

class LibCurl;  // libcurl interface as a class, for dependency injection.

/// \brief A basic HTTP client based on the libcurl library.
///
/// The usage pattern for the class reflects the one of the libcurl library:
/// create a request object, set request parameters and call Send().
///
/// For example:
///   std::unique_ptr<HttpRequest> request(http_request_factory->Create());
///   request->SetUri("http://www.google.com");
///   request->SetResultsBuffer(out_buffer);
///   request->Send();
class CurlHttpRequest : public HttpRequest {
 public:
  class Factory : public HttpRequest::Factory {
   public:
    virtual ~Factory() {}
    virtual HttpRequest* Create() { return new CurlHttpRequest(); }
  };

  CurlHttpRequest();
  explicit CurlHttpRequest(LibCurl* libcurl)
      : CurlHttpRequest(libcurl, Env::Default()) {}
  CurlHttpRequest(LibCurl* libcurl, Env* env);
  ~CurlHttpRequest() override;

  Status Init() override;

  /// Sets the request URI.
  Status SetUri(const string& uri) override;

  /// \brief Sets the Range header.
  ///
  /// Used for random seeks, for example "0-999" returns the first 1000 bytes
  /// (note that the right border is included).
  Status SetRange(uint64 start, uint64 end) override;

  /// Sets a request header.
  Status AddHeader(const string& name, const string& value) override;

  Status AddResolveOverride(const string& hostname, int64 port,
                            const string& ip_addr) override;

  /// Sets the 'Authorization' header to the value of 'Bearer ' + auth_token.
  Status AddAuthBearerHeader(const string& auth_token) override;

  /// Makes the request a DELETE request.
  Status SetDeleteRequest() override;

  /// \brief Makes the request a PUT request.
  ///
  /// The request body will be taken from the specified file starting from
  /// the given offset.
  Status SetPutFromFile(const string& body_filepath, size_t offset) override;

  /// Makes the request a PUT request with an empty body.
  Status SetPutEmptyBody() override;

  /// \brief Makes the request a POST request.
  ///
  /// The request body will be taken from the specified buffer.
  Status SetPostFromBuffer(const char* buffer, size_t size) override;

  /// Makes the request a POST request with an empty body.
  Status SetPostEmptyBody() override;

  /// \brief Specifies the buffer for receiving the response body.
  ///
  /// Size of out_buffer after an access will be exactly the number of bytes
  /// read. Existing content of the vector will be cleared.
  Status SetResultBuffer(std::vector<char>* out_buffer) override;

  /// \brief Returns the response headers of a completed request.
  ///
  /// If the header is not found, returns an empty string.
  string GetResponseHeader(const string& name) const override;

  /// Returns the response code of a completed request.
  uint64 GetResponseCode() const override;

  /// \brief Sends the formed request.
  ///
  /// If the result buffer was defined, the response will be written there.
  /// The object is not designed to be re-used after Send() is executed.
  Status Send() override;

  // Url encodes str and returns a new string.
  string EscapeString(const string& str) override;

  Status SetTimeouts(uint32 connection, uint32 inactivity,
                     uint32 total) override;

 private:
  /// A write callback in the form which can be accepted by libcurl.
  static size_t WriteCallback(const void* ptr, size_t size, size_t nmemb,
                              void* userdata);
  /// A read callback in the form which can be accepted by libcurl.
  static size_t ReadCallback(void* ptr, size_t size, size_t nmemb,
                             FILE* userdata);
  /// A header callback in the form which can be accepted by libcurl.
  static size_t HeaderCallback(const void* ptr, size_t size, size_t nmemb,
                               void* this_object);
  /// A progress meter callback in the form which can be accepted by libcurl.
  static int ProgressCallback(void* this_object, curl_off_t dltotal,
                              curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow);
  Status CheckInitialized() const;
  Status CheckMethodNotSet() const;
  Status CheckNotSent() const;

  LibCurl* libcurl_;
  Env* env_;

  FILE* put_body_ = nullptr;

  StringPiece post_body_buffer_;
  size_t post_body_read_ = 0;

  std::vector<char>* response_buffer_ = nullptr;
  CURL* curl_ = nullptr;
  curl_slist* curl_headers_ = nullptr;
  curl_slist* resolve_list_ = nullptr;

  std::vector<char> default_response_buffer_;

  std::unordered_map<string, string> response_headers_;
  uint64 response_code_ = 0;

  // The timestamp of the last activity related to the request execution, in
  // seconds since epoch.
  uint64 last_progress_timestamp_ = 0;
  // The last progress in terms of bytes transmitted.
  curl_off_t last_progress_bytes_ = 0;

  // The maximum period of request inactivity.
  uint32 inactivity_timeout_secs_ = 60;  // 1 minute

  // Timeout for the connection phase.
  uint32 connect_timeout_secs_ = 120;  // 2 minutes

  // Tiemout for the whole request. Set only to prevent hanging indefinitely.
  uint32 request_timeout_secs_ = 3600;  // 1 hour

  // Members to enforce the usage flow.
  bool is_initialized_ = false;
  bool is_uri_set_ = false;
  bool is_method_set_ = false;
  bool is_sent_ = false;

  // Store the URI to help disambiguate requests when errors occur.
  string uri_;

  TF_DISALLOW_COPY_AND_ASSIGN(CurlHttpRequest);
};

/// \brief A proxy to the libcurl C interface as a dependency injection measure.
///
/// This class is meant as a very thin wrapper for the libcurl C library.
class LibCurl {
 public:
  virtual ~LibCurl() {}

  virtual CURL* curl_easy_init() = 0;
  virtual CURLcode curl_easy_setopt(CURL* curl, CURLoption option,
                                    uint64 param) = 0;
  virtual CURLcode curl_easy_setopt(CURL* curl, CURLoption option,
                                    const char* param) = 0;
  virtual CURLcode curl_easy_setopt(CURL* curl, CURLoption option,
                                    void* param) = 0;
  virtual CURLcode curl_easy_setopt(CURL* curl, CURLoption option,
                                    size_t (*param)(void*, size_t, size_t,
                                                    FILE*)) = 0;
  virtual CURLcode curl_easy_setopt(CURL* curl, CURLoption option,
                                    size_t (*param)(const void*, size_t, size_t,
                                                    void*)) = 0;
  virtual CURLcode curl_easy_setopt(
      CURL* curl, CURLoption option,
      int (*param)(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t ultotal, curl_off_t ulnow)) = 0;
  virtual CURLcode curl_easy_perform(CURL* curl) = 0;
  virtual CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info,
                                     uint64* value) = 0;
  virtual CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info,
                                     double* value) = 0;
  virtual void curl_easy_cleanup(CURL* curl) = 0;
  virtual curl_slist* curl_slist_append(curl_slist* list, const char* str) = 0;
  virtual void curl_slist_free_all(curl_slist* list) = 0;
  virtual char* curl_easy_escape(CURL* curl, const char* str, int length) = 0;
  virtual void curl_free(void* p) = 0;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_PLATFORM_CLOUD_CURL_HTTP_REQUEST_H_
