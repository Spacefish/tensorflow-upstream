/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/validator_runner_impl.h"

#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <thread>  // NOLINT: code only used on Android, where std::thread is allowed
#include <utility>
#include <vector>

#include "flatbuffers/flatbuffer_builder.h"  // from @flatbuffers
#include "tensorflow/lite/experimental/acceleration/configuration/configuration_generated.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/fb_storage.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/file_lock.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/model_loader.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/runner.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/status_codes.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/validator.h"
#include "tensorflow/lite/minimal_logging.h"

namespace tflite {
namespace acceleration {

using ::flatbuffers::FlatBufferBuilder;

MinibenchmarkStatus ValidatorRunnerImpl::Init() {
  std::unique_ptr<ModelLoader> model_loader =
      CreateModelLoaderFromPath(fd_or_model_path_);
  if (!model_loader) {
    TF_LITE_REPORT_ERROR(error_reporter_, "Failed to parse model path.");
    return kMinibenchmarkPreconditionNotMet;
  }

  // Check that the model can be loaded from disk.
  MinibenchmarkStatus status = model_loader->Init();
  if (status != kMinibenchmarkSuccess) {
    TF_LITE_REPORT_ERROR(error_reporter_, "Could not load model: %d",
                         static_cast<int>(status));
    return status;
  }

  status = nnapi_helper_.Load();
  if (status != kMinibenchmarkSuccess) {
    TF_LITE_REPORT_ERROR(error_reporter_, "Failed to load NNAPI SL: %d",
                         static_cast<int>(status));
    return status;
  }

  status = validation_entrypoint_helper_.Validate();
  if (status != kMinibenchmarkSuccess) {
    return status;
  }

  ProcessRunner check_runner(data_directory_path_,
                             validation_entrypoint_helper_.name().c_str(),
                             validation_entrypoint_helper_.LoadEntrypoint());
  status = check_runner.Init();
  if (status != kMinibenchmarkSuccess) {
    TF_LITE_REPORT_ERROR(error_reporter_, "Runner::Init returned %d",
                         static_cast<int>(status));
    return status;
  }
  return kMinibenchmarkSuccess;
}

void ValidatorRunnerImpl::TriggerValidationAsync(
    std::unique_ptr<std::vector<FlatBufferBuilder>> tflite_settings) {
  if (!tflite_settings || tflite_settings->empty()) {
    return;
  }

  // We purposefully detach the thread and have it own all the data. The
  // runner may potentially hang, so we can't wait for it to terminate.
  std::thread detached_thread([model_path = fd_or_model_path_,
                               storage_path = storage_path_,
                               data_directory_path = data_directory_path_,
                               tflite_settings = std::move(tflite_settings),
                               validation_entrypoint_name =
                                   validation_entrypoint_helper_.name().c_str(),
                               validation_entrypoint =
                                   validation_entrypoint_helper_
                                       .LoadEntrypoint(),
                               nnapi_sl_path =
                                   nnapi_helper_.nnapi_sl_path()]() {
    FileLock lock(storage_path + ".parent_lock");
    if (!lock.TryLock()) {
      return;
    }
    for (auto& one_setting : *tflite_settings) {
      FlatbufferStorage<BenchmarkEvent> storage(storage_path);
      TFLiteSettingsT tflite_settings_obj;
      flatbuffers::GetRoot<TFLiteSettings>(one_setting.GetBufferPointer())
          ->UnPackTo(&tflite_settings_obj);
      TFLITE_LOG_PROD(TFLITE_LOG_INFO, "Run validation with entry point '%s'",
                      validation_entrypoint_name);
      ProcessRunner runner(data_directory_path, validation_entrypoint_name,
                           validation_entrypoint);
      int exitcode = 0;
      int signal = 0;
      MinibenchmarkStatus status = runner.Init();
      if (status == kMinibenchmarkSuccess) {
        flatbuffers::FlatBufferBuilder fbb;
        status = storage.Append(
            &fbb,
            CreateBenchmarkEvent(
                fbb, CreateTFLiteSettings(fbb, &tflite_settings_obj),
                BenchmarkEventType_START, /* result */ 0, /* error */ 0,
                Validator::BootTimeMicros(), Validator::WallTimeMicros()));
        if (status == kMinibenchmarkSuccess) {
          std::vector<std::string> args{model_path, storage_path,
                                        data_directory_path};
          if (!nnapi_sl_path.empty() &&
              tflite_settings_obj.delegate == tflite::Delegate_NNAPI) {
            TFLITE_LOG_PROD(
                TFLITE_LOG_INFO,
                "Running benchmark using NNAPI support library at path '%s'",
                nnapi_sl_path.c_str());
            args.push_back(nnapi_sl_path);
          }
          std::string output;
          status = runner.Run(nullptr, args, &output, &exitcode, &signal);
        }
      }
      if (status != kMinibenchmarkSuccess) {
        std::cout << "Run() returned " << status << std::endl;
        flatbuffers::FlatBufferBuilder fbb;
        storage.Append(
            &fbb,
            CreateBenchmarkEvent(
                fbb, CreateTFLiteSettings(fbb, &tflite_settings_obj),
                BenchmarkEventType_ERROR, /* result */ 0,
                CreateBenchmarkError(fbb, BenchmarkStage_UNKNOWN, status,
                                     signal, {}, exitcode),
                Validator::BootTimeMicros(), Validator::WallTimeMicros()));
      }
    }
  });
  detached_thread.detach();
}

MinibenchmarkStatus
ValidatorRunnerImpl::ValidationEntrypointHelper::Validate() {
#ifndef _WIN32
  if (!LoadEntrypoint()) {
    TF_LITE_REPORT_ERROR(error_reporter_, "Could not load symbol '%s': '%s'",
                         validation_entrypoint_name_.c_str(), dlerror());
    return kMinibenchmarkValidationEntrypointSymbolNotFound;
  }
  return kMinibenchmarkSuccess;
#else   // _WIN32
  return kMinibenchmarkUnsupportedPlatform;
#endif  // !_WIN32
}

ValidatorRunnerImpl::ValidationEntrypointHelper::EntrypointFunc*
ValidatorRunnerImpl::ValidationEntrypointHelper::LoadEntrypoint() {
#ifndef _WIN32
  // We use dlsym() to lookup the entrypoint function every time because this
  // helper is used in a multi-threaded environment.
  return reinterpret_cast<int (*)(int, char**)>(
      dlsym(RTLD_DEFAULT, validation_entrypoint_name_.c_str()));
#endif  // !_WIN32
  return nullptr;
}

MinibenchmarkStatus ValidatorRunnerImpl::NnapiHelper::Load() {
  if (nnapi_sl_) {
#ifndef _WIN32
    Dl_info dl_info;
    // Looking for the file where the NNAPI SL is loaded from. We are using
    // the ANeuralNetworks_getRuntimeFeatureLevel because it is a required
    // function for NNAPI drivers.
    // If the function is not defined or it wasn't defined in any of the shared
    // libraries loaded by the calling process we fail with a specific error
    // code. This could happen only if the NNAPI Support Library pointer set
    // into our TfLiteSettings comes from an invalid NNAPI SL library or there
    // is some error in the NNAPI loading code.
    if (!nnapi_sl_->ANeuralNetworks_getRuntimeFeatureLevel) {
      return kMiniBenchmarkCannotLoadSupportLibrary;
    }
    int status = dladdr(reinterpret_cast<void*>(
                            nnapi_sl_->ANeuralNetworks_getRuntimeFeatureLevel),
                        &dl_info);
    if (status == 0 || !dl_info.dli_fname) {
      return kMiniBenchmarkCannotLoadSupportLibrary;
    }
    nnapi_sl_path_ = dl_info.dli_fname;
#else   // _WIN32
    return kMinibenchmarkUnsupportedPlatform;
#endif  // !_WIN32
  }
  return kMinibenchmarkSuccess;
}

}  // namespace acceleration
}  // namespace tflite
