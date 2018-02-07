//
// Created by skama on 1/23/18.
//

#include "tensorflow/contrib/tensorrt/resources/TRTResourceManager.h"
#include "tensorflow/core/platform/default/logging.h"


std::shared_ptr<tensorflow::ResourceMgr> tensorflow::trt::TRTResourceManager::getManager(const std::string &mgr_name) {
  // mutex is held for lookup only. Most instantiations where mutex will be held longer
  // will be during op creation and should be ok.
  tensorflow::mutex_lock lock(map_mutex_);
  auto s=managers_.find(mgr_name);
  if(s==managers_.end()){
    auto it=managers_.emplace(mgr_name,std::make_shared<tensorflow::ResourceMgr>(mgr_name));
    VLOG(0)<<"Returning a new manager "<<mgr_name;
    return it.first->second;
  }
  VLOG(1)<<"Returning old manager "<<mgr_name;
  return s->second;
}
