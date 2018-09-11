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

#include "ignite_plain_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>

#include <iostream>

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {

PlainClient::PlainClient(std::string host, int port)
    : host_(host), port_(port), sock_(-1) {}

PlainClient::~PlainClient() {
  if (IsConnected()) {
    Status status = Disconnect();
    if (!status.ok()) LOG(WARNING) << status.ToString();
  }
}

Status PlainClient::Connect() {
  if (sock_ == -1) {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == -1) return errors::Internal("Failed to create socket");
  }

  sockaddr_in server;

  server.sin_addr.s_addr = inet_addr(host_.c_str());
  if (server.sin_addr.s_addr == -1) {
    hostent* he;
    in_addr** addr_list;

    if ((he = gethostbyname(host_.c_str())) == NULL)
      return errors::Internal("Failed to resolve hostname \"", host_, "\"");

    addr_list = (in_addr**)he->h_addr_list;
    if (addr_list[0] != NULL) server.sin_addr = *addr_list[0];
  }

  server.sin_family = AF_INET;
  server.sin_port = htons(port_);

  if (connect(sock_, (sockaddr*)&server, sizeof(server)) < 0)
    return errors::Internal("Failed to connect to \"", host_, ":", port_, "\"");

  LOG(INFO) << "Connection to \"" << host_ << ":" << port_ << "\" established";

  return Status::OK();
}

Status PlainClient::Disconnect() {
  int close_res = close(sock_);
  sock_ = -1;

  LOG(INFO) << "Connection to \"" << host_ << ":" << port_ << "\" is closed";

  return close_res == 0
             ? Status::OK()
             : errors::Internal("Failed to correctly close connection");
}

bool PlainClient::IsConnected() { return sock_ != -1; }

int PlainClient::GetSocketDescriptor() { return sock_; }

Status PlainClient::ReadData(uint8_t* buf, int32_t length) {
  int recieved = 0;

  while (recieved < length) {
    int res = recv(sock_, buf, length - recieved, 0);

    if (res < 0)
      return errors::Internal("Error occured while reading from socket: ", res,
                              ", ", std::string(strerror(errno)));

    if (res == 0) return errors::Internal("Server closed connection");

    recieved += res;
    buf += res;
  }

  return Status::OK();
}

Status PlainClient::WriteData(uint8_t* buf, int32_t length) {
  int sent = 0;

  while (sent < length) {
    int res = send(sock_, buf, length - sent, 0);

    if (res < 0)
      return errors::Internal("Error occured while writing into socket: ", res,
                              ", ", std::string(strerror(errno)));

    sent += res;
    buf += res;
  }

  return Status::OK();
}

}  // namespace tensorflow
