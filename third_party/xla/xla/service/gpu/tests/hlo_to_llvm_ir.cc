/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include <iostream>
#include <string>
#include <vector>

#include "xla/hlo/ir/hlo_module.h"
#include "xla/service/gpu/compile_module_to_llvm_ir.h"
#include "xla/service/gpu/gpu_device_info_for_tests.h"
#include "xla/service/gpu/llvm_gpu_backend/gpu_backend_lib.h"
#include "xla/service/gpu/target_constants.h"
#include "xla/status.h"

#if GOOGLE_CUDA
#include "xla/stream_executor/cuda/cuda_platform_id.h"
#elif TENSORFLOW_USE_ROCM
#include "xla/stream_executor/rocm/rocm_platform_id.h"
#include "tsl/platform/rocm_rocdl_path.h"
#endif
#include "xla/tests/test_utils.h"
#include "xla/tools/hlo_module_loader.h"
#include "tsl/platform/init_main.h"
#include "tsl/util/command_line_flags.h"

const char* const kUsage = R"(
This tool reads in an HloModule from a file, compiles it using the NVPTX or AMDGPU
compiler and prints out the LLVM IR generated by the IR emitter.  The LLVM IR is
not optimized by the LLVM pass pipeline, so this tool can be used to unit test
the XLA GPU IR emitters.

Note that the LLVM IR does not contain the *full* module, but only parts that
will be code generated into PTX/Hsaco. The NVPTX/Hsaco compiler also generates a
GpuExecutable on the side that is not printed.

When passed the parameter `--ptx`, the LLVM IR will be optimized and PTX
will be emitted and printed instead of the non-optimized LLVM.
By default SM 70 is targeted. But this can be changed with `--sm=SM`.)";

namespace {
xla::Status CompileAndPrintLlvmIr(const std::string& hlo_text,
                                  bool generate_ptx, int sm) {
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<xla::HloModule> hlo_module,
      xla::LoadModuleFromData(/*data=*/hlo_text, /*format=*/"hlo"));

  TF_RETURN_IF_ERROR(VerifyHloModule(hlo_module.get(),
                                     /*layout_sensitive=*/false,
                                     /*allow_mixed_precision=*/true));
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  llvm::LLVMContext llvm_context;

  tensorflow::se::CudaComputeCapability cuda_compute_capability;
  cuda_compute_capability.major = sm / 10;
  cuda_compute_capability.minor = sm % 10;

#if GOOGLE_CUDA
  xla::gpu::GpuDeviceInfo gpu_device_info =
      xla::gpu::TestGpuDeviceInfo::RTXA6000DeviceInfo();
  gpu_device_info.compute_capability = cuda_compute_capability;
  std::string target_triple = xla::gpu::nvptx::TargetTriple();
  std::string data_layout = xla::gpu::nvptx::DataLayout();
  std::string platform_name = "CUDA";
  stream_executor::Platform::Id platform_id =
      stream_executor::cuda::kCudaPlatformId;
#elif TENSORFLOW_USE_ROCM
  xla::gpu::GpuDeviceInfo gpu_device_info =
      xla::gpu::TestGpuDeviceInfo::AMDMI210DeviceInfo();
  std::string target_triple = xla::gpu::amdgpu::TargetTriple();
  std::string data_layout = xla::gpu::amdgpu::DataLayout();
  std::string platform_name = "ROCm";
  stream_executor::Platform::Id platform_id =
      stream_executor::rocm::kROCmPlatformId;
#endif

  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<llvm::Module> llvm_module,
      xla::gpu::CompileModuleToLlvmIr(hlo_module.get(), &llvm_context,
                                      target_triple, data_layout, platform_name,
                                      platform_id, gpu_device_info,
                                      /*pointer_size=*/8));

  if (!generate_ptx) {
    llvm_module->print(llvm::outs(), nullptr);
  } else {
#if GOOGLE_CUDA
    TF_ASSIGN_OR_RETURN(std::string ptx,
                        xla::gpu::nvptx::CompileToPtx(
                            llvm_module.get(), cuda_compute_capability,
                            hlo_module->config().debug_options()));
    std::cout << ptx << std::endl;
#elif TENSORFLOW_USE_ROCM
    return {absl::StatusCode::kUnimplemented,
            "Feature not yet implemented in ROCm"};
#endif
  }
#endif
  return xla::OkStatus();
}

xla::Status CompileAndPrintLlvmIrFromFile(const std::string& file_name,
                                          bool ptx, int sm) {
  std::string full_text;
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), file_name, &full_text));

  std::vector<std::string> hlo_module_texts =
      absl::StrSplit(full_text, "// -----");
  for (const std::string& hlo_module_text : hlo_module_texts) {
    TF_RETURN_IF_ERROR(CompileAndPrintLlvmIr(hlo_module_text, ptx, sm));
  }

  return xla::OkStatus();
}
}  // namespace

int main(int argc, char** argv) {
  bool ptx = false;
  int sm = 70;
  std::vector<tsl::Flag> flag_list;
  xla::AppendDebugOptionsFlags(&flag_list);
  flag_list.emplace_back("ptx", &ptx,
                         "Print PTX instead of not optimized LLVM.");
  flag_list.emplace_back("sm", &sm,
                         "Specify the SM to target (useful only with --ptx).");
  // The usage string includes the message at the top of the file, the
  // DebugOptions flags and the flags defined above.
  const std::string kUsageString =
      absl::StrCat(kUsage, "\n\n", tsl::Flags::Usage(argv[0], flag_list));
  bool parse_ok = tsl::Flags::Parse(&argc, argv, flag_list);
  tsl::port::InitMain(kUsageString.c_str(), &argc, &argv);
  if (!parse_ok) {
    LOG(QFATAL) << kUsageString;
  }

  QCHECK(argc == 2) << "Must specify a single input file";
  TF_CHECK_OK(CompileAndPrintLlvmIrFromFile(argv[1], ptx, sm));

  return 0;
}
