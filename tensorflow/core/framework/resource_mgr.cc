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

#include "tensorflow/core/framework/resource_mgr.h"

#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/platform/regexp.h"

namespace tensorflow {

ResourceMgr::ResourceMgr() : default_container_("localhost") {}

ResourceMgr::ResourceMgr(const string& default_container)
    : default_container_(default_container) {}

ResourceMgr::~ResourceMgr() { Clear(); }

void ResourceMgr::Clear() {
  mutex_lock l(mu_);
  for (const auto& p : containers_) {
    for (const auto& q : *p.second) {
      q.second->Unref();
    }
    delete p.second;
  }
  containers_.clear();
}

Status ResourceMgr::DoCreate(const string& container, ResourceMgrTypeIndex type,
                             const string& name, ResourceBase* resource) {
  {
    mutex_lock l(mu_);
    Container** b = &containers_[container];
    if (*b == nullptr) {
      *b = new Container;
    }
    if ((*b)->insert({{type, name}, resource}).second) {
      return Status::OK();
    }
  }
  resource->Unref();
  return errors::AlreadyExists("Resource ", container, "/", name, "/",
                               type.name());
}

Status ResourceMgr::DoLookup(const string& container, ResourceMgrTypeIndex type,
                             const string& name,
                             ResourceBase** resource) const {
  mutex_lock l(mu_);
  const Container* b = gtl::FindPtrOrNull(containers_, container);
  if (b == nullptr) {
    return errors::NotFound("Container ", container, " does not exist.");
  }
  auto r = gtl::FindPtrOrNull(*b, {type, name});
  if (r == nullptr) {
    return errors::NotFound("Resource ", container, "/", name, "/", type.name(),
                            " does not exist.");
  }
  *resource = const_cast<ResourceBase*>(r);
  (*resource)->Ref();
  return Status::OK();
}

Status ResourceMgr::DoDelete(const string& container, ResourceMgrTypeIndex type,
                             const string& name) {
  ResourceBase* base = nullptr;
  {
    mutex_lock l(mu_);
    Container* b = gtl::FindPtrOrNull(containers_, container);
    if (b == nullptr) {
      return errors::NotFound("Container ", container, " does not exist.");
    }
    auto iter = b->find({type, name});
    if (iter == b->end()) {
      return errors::NotFound("Resource ", container, "/", name, "/",
                              type.name(), " does not exist.");
    }
    base = iter->second;
    b->erase(iter);
  }
  CHECK(base != nullptr);
  base->Unref();
  return Status::OK();
}

Status ResourceMgr::Cleanup(const string& container) {
  Container* b = nullptr;
  {
    mutex_lock l(mu_);
    auto iter = containers_.find(container);
    if (iter == containers_.end()) {
      return errors::NotFound("Container ", container, " does not exist.");
    }
    b = iter->second;
    containers_.erase(iter);
  }
  CHECK(b != nullptr);
  for (const auto& p : *b) {
    p.second->Unref();
  }
  delete b;
  return Status::OK();
}

Status ContainerInfo::Init(ResourceMgr* rmgr, const NodeDef& ndef,
                           bool use_node_name_as_default) {
  CHECK(rmgr);
  rmgr_ = rmgr;
  string attr_container;
  TF_RETURN_IF_ERROR(GetNodeAttr(ndef, "container", &attr_container));
  static RE2 container_re("[A-Za-z0-9.][A-Za-z0-9_.\\-/]*");
  if (!attr_container.empty() &&
      !RE2::FullMatch(attr_container, container_re)) {
    return errors::InvalidArgument("container contains invalid characters: ",
                                   attr_container);
  }
  string attr_shared_name;
  TF_RETURN_IF_ERROR(GetNodeAttr(ndef, "shared_name", &attr_shared_name));
  if (!attr_shared_name.empty() && (attr_shared_name[0] == '_')) {
    return errors::InvalidArgument("shared_name cannot start with '_':",
                                   attr_shared_name);
  }
  if (!attr_container.empty()) {
    container_ = attr_container;
  } else {
    container_ = rmgr_->default_container();
  }
  if (!attr_shared_name.empty()) {
    name_ = attr_shared_name;
  } else if (use_node_name_as_default) {
    name_ = ndef.name();
  } else {
    resource_is_private_to_kernel_ = true;
    static std::atomic<int64> counter(0);
    name_ = strings::StrCat("_", counter.fetch_add(1), "_", ndef.name());
  }
  return Status::OK();
}

string ContainerInfo::DebugString() const {
  return strings::StrCat("[", container(), ",", name(), ",",
                         resource_is_private_to_kernel() ? "private" : "public",
                         "]");
}

}  //  end namespace tensorflow
