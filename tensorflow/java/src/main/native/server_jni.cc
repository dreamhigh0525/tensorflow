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

#include "tensorflow/java/src/main/native/server_jni.h"
#include "tensorflow/c/c_api.h"
#include "tensorflow/java/src/main/native/exception_jni.h"
#include "tensorflow/java/src/main/native/utils_jni.h"

JNIEXPORT jlong JNICALL Java_org_tensorflow_Server_allocate(
    JNIEnv* env, jclass clazz, jbyteArray server_def) {
  TF_Status* status = TF_NewStatus();

  jbyte* server_def_ptr = env->GetByteArrayElements(server_def, nullptr);

  TF_Server* server = TF_NewServer(
      server_def_ptr, static_cast<size_t>(env->GetArrayLength(server_def)),
      status);

  env->ReleaseByteArrayElements(server_def, server_def_ptr, JNI_ABORT);
  throwExceptionIfNotOK(env, status);

  return reinterpret_cast<jlong>(server);
}

JNIEXPORT void JNICALL Java_org_tensorflow_Server_start(JNIEnv* env,
                                                        jclass clazz,
                                                        jlong handle) {
  TF_Status* status = TF_NewStatus();
  TF_Server* server = reinterpret_cast<TF_Server*>(handle);

  TF_StartServer(server, status);
  throwExceptionIfNotOK(env, status);
}

JNIEXPORT void JNICALL Java_org_tensorflow_Server_stop(JNIEnv* env,
                                                       jclass clazz,
                                                       jlong handle) {
  TF_Status* status = TF_NewStatus();
  TF_Server* server = reinterpret_cast<TF_Server*>(handle);

  TF_StopServer(server, status);
  throwExceptionIfNotOK(env, status);
}

JNIEXPORT void JNICALL Java_org_tensorflow_Server_join(JNIEnv* env,
                                                       jclass clazz,
                                                       jlong handle) {
  TF_Status* status = TF_NewStatus();
  TF_Server* server = reinterpret_cast<TF_Server*>(handle);

  TF_JoinServer(server, status);
  throwExceptionIfNotOK(env, status);
}

JNIEXPORT void JNICALL Java_org_tensorflow_Server_delete(JNIEnv* env,
                                                         jclass clazz,
                                                         jlong handle) {
  TF_Server* server = reinterpret_cast<TF_Server*>(handle);

  TF_DeleteServer(server);
}
