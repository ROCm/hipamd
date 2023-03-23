/*
Copyright (c) 2022 - 2023 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "hiprtcInternal.hpp"

#include <fstream>
#include <streambuf>
#include <vector>

#include <sys/stat.h>

#include "vdi_common.hpp"
#include "utils/flags.hpp"

namespace hiprtc {
using namespace helpers;

std::vector<std::string> getLinkOptions(const LinkArguments& args) {
  std::vector<std::string> res;
  auto irArgCount = args.linkerIRArgCount();
  if (irArgCount > 0) {
    res.reserve(irArgCount);
    auto irArg = args.linkerIRArg();
    for (size_t i = 0; i < irArgCount; i++) {
      res.emplace_back(std::string(irArg[i]));
    }
  }
  return res;
}

// RTC Program Member Functions
RTCProgram::RTCProgram(std::string name) : name_(name) {
  constexpr bool kComgrVersioned = true;
  std::call_once(amd::Comgr::initialized, amd::Comgr::LoadLib, kComgrVersioned);
  if (amd::Comgr::create_data_set(&exec_input_) != AMD_COMGR_STATUS_SUCCESS) {
    crashWithMessage("Failed to allocate internal hiprtc structure");
  }
}

bool RTCProgram::findIsa() {
  const char* libName;
#ifdef _WIN32
  libName = "amdhip64.dll";
#else
  libName = "libamdhip64.so";
#endif

  void* handle = amd::Os::loadLibrary(libName);

  if (!handle) {
    LogInfo("hip runtime failed to load using dlopen");
    build_log_ +=
        "hip runtime failed to load.\n"
        "Error: Please provide architecture for which code is to be "
        "generated.\n";
    return false;
  }

  void* sym_hipGetDevice = amd::Os::getSymbol(handle, "hipGetDevice");
  void* sym_hipGetDeviceProperties = amd::Os::getSymbol(handle, "hipGetDeviceProperties");

  if (sym_hipGetDevice == nullptr || sym_hipGetDeviceProperties == nullptr) {
    LogInfo("ISA cannot be found to dlsym failure");
    build_log_ +=
        "ISA cannot be found from hip runtime.\n"
        "Error: Please provide architecture for which code is to be "
        "generated.\n";
    return false;
  }

  hipError_t (*dyn_hipGetDevice)(int*) = reinterpret_cast<hipError_t (*)(int*)>(sym_hipGetDevice);

  hipError_t (*dyn_hipGetDeviceProperties)(hipDeviceProp_t*, int) =
      reinterpret_cast<hipError_t (*)(hipDeviceProp_t*, int)>(sym_hipGetDeviceProperties);

  int device;
  hipError_t status = dyn_hipGetDevice(&device);
  if (status != hipSuccess) {
    return false;
  }
  hipDeviceProp_t props;
  status = dyn_hipGetDeviceProperties(&props, device);
  if (status != hipSuccess) {
    return false;
  }
  isa_ = "amdgcn-amd-amdhsa--";
  isa_.append(props.gcnArchName);

  amd::Os::unloadLibrary(handle);
  return true;
}

// RTC Compile Program Member Functions
RTCCompileProgram::RTCCompileProgram(std::string name_) : RTCProgram(name_), fgpu_rdc_(false) {
  if ((amd::Comgr::create_data_set(&compile_input_) != AMD_COMGR_STATUS_SUCCESS) ||
      (amd::Comgr::create_data_set(&link_input_) != AMD_COMGR_STATUS_SUCCESS)) {
    crashWithMessage("Failed to allocate internal hiprtc structure");
  }
  // Add internal header
  if (!addBuiltinHeader()) {
    crashWithMessage("Unable to add internal header");
  }

  // Add compile options
  const std::string hipVerOpt{"--hip-version=" + std::to_string(HIP_VERSION_MAJOR) + '.' +
                              std::to_string(HIP_VERSION_MINOR) + '.' +
                              std::to_string(HIP_VERSION_PATCH)};
  const std::string hipVerMajor{"-DHIP_VERSION_MAJOR=" + std::to_string(HIP_VERSION_MAJOR)};
  const std::string hipVerMinor{"-DHIP_VERSION_MINOR=" + std::to_string(HIP_VERSION_MINOR)};
  const std::string hipVerPatch{"-DHIP_VERSION_PATCH=" + std::to_string(HIP_VERSION_PATCH)};

  compile_options_.reserve(20);  // count of options below
  compile_options_.push_back("-O3");

  if (GPU_ENABLE_WGP_MODE) compile_options_.push_back("-mcumode");

  if (!GPU_ENABLE_WAVE32_MODE) compile_options_.push_back("-mwavefrontsize64");

  compile_options_.push_back(hipVerOpt);
  compile_options_.push_back(hipVerMajor);
  compile_options_.push_back(hipVerMinor);
  compile_options_.push_back(hipVerPatch);
  compile_options_.push_back("-D__HIPCC_RTC__");
  compile_options_.push_back("-include");
  compile_options_.push_back("hiprtc_runtime.h");
  compile_options_.push_back("-std=c++14");
  compile_options_.push_back("-nogpuinc");
  compile_options_.push_back("-Wno-gnu-line-marker");
  compile_options_.push_back("-Wno-missing-prototypes");
#ifdef _WIN32
  compile_options_.push_back("-target");
  compile_options_.push_back("x86_64-pc-windows-msvc");
  compile_options_.push_back("-fms-extensions");
  compile_options_.push_back("-fms-compatibility");
#endif

  exe_options_.push_back("-O3");
}

bool RTCCompileProgram::addSource(const std::string& source, const std::string& name) {
  if (source.size() == 0 || name.size() == 0) {
    LogError("Error in hiprtc: source or name is of size 0 in addSource");
    return false;
  }
  source_code_ += source;
  source_name_ = name;
  return true;
}

// addSource_impl is a different function because we need to add source when we track mangled
// objects
bool RTCCompileProgram::addSource_impl() {
  std::vector<char> vsource(source_code_.begin(), source_code_.end());
  if (!addCodeObjData(compile_input_, vsource, source_name_, AMD_COMGR_DATA_KIND_SOURCE)) {
    return false;
  }
  return true;
}

bool RTCCompileProgram::addHeader(const std::string& source, const std::string& name) {
  if (source.size() == 0 || name.size() == 0) {
    LogError("Error in hiprtc: source or name is of size 0 in addHeader");
    return false;
  }
  std::vector<char> vsource(source.begin(), source.end());
  if (!addCodeObjData(compile_input_, vsource, name, AMD_COMGR_DATA_KIND_INCLUDE)) {
    return false;
  }
  return true;
}

bool RTCCompileProgram::addBuiltinHeader() {
  std::vector<char> source(__hipRTC_header, __hipRTC_header + __hipRTC_header_size);
  std::string name{"hiprtc_runtime.h"};
  if (!addCodeObjData(compile_input_, source, name, AMD_COMGR_DATA_KIND_INCLUDE)) {
    return false;
  }
  return true;
}

bool RTCCompileProgram::findLLVMOptions(const std::vector<std::string>& options,
                                        std::vector<std::string>& llvm_options) {
  for (size_t i = 0; i < options.size(); ++i) {
    if (options[i] == "-mllvm") {
      if (options.size() == (i + 1)) {
        LogInfo(
            "-mllvm option passed by the app, it comes as a pair but there is no option after "
            "this");
        return false;
      }
      llvm_options.push_back(options[i]);
      llvm_options.push_back(options[i + 1]);
    }
  }
  return true;
}

bool RTCCompileProgram::transformOptions(std::vector<std::string>& compile_options) {
  auto getValueOf = [](const std::string& option) {
    std::string res;
    auto f = std::find(option.begin(), option.end(), '=');
    if (f != option.end()) res = std::string(f + 1, option.end());
    return res;
  };

  for (auto& i : compile_options) {
    if (i == "-hip-pch") {
      LogInfo(
          "-hip-pch is deprecated option, has no impact on execution of new hiprtc programs, it "
          "can be removed");
      i.clear();
      continue;
    }
    // Some rtc samples use --gpu-architecture
    if (i.rfind("--gpu-architecture=", 0) == 0) {
      LogInfo("--gpu-architecture is nvcc option, transforming it to --offload-arch option");
      auto val = getValueOf(i);
      i = "--offload-arch=" + val;
      continue;
    }
    if (i == "--save-temps") {
      settings_.dumpISA = true;
      continue;
    }
  }

  // Removed consumed options
  compile_options.erase(
      std::remove(compile_options.begin(), compile_options.end(), std::string("")),
      compile_options.end());

  if (auto res = std::find_if(
          compile_options.begin(), compile_options.end(),
          [](const std::string& str) { return str.find("--offload-arch=") != std::string::npos; });
      res != compile_options.end()) {
    auto isaName = getValueOf(*res);
    isa_ = "amdgcn-amd-amdhsa--" + isaName;
    settings_.offloadArchProvided = true;
    return true;
  }
  // App has not provided the gpu archiecture, need to find it
  return findIsa();
}

amd::Monitor RTCProgram::lock_("HIPRTC Program", true);

bool RTCCompileProgram::compile(const std::vector<std::string>& options, bool fgpu_rdc) {
  if (!addSource_impl()) {
    LogError("Error in hiprtc: unable to add source code");
    return false;
  }

  fgpu_rdc_ = fgpu_rdc;

  // Append compile options
  std::vector<std::string> compileOpts(compile_options_);
  compileOpts.reserve(compile_options_.size() + options.size() + 2);
  compileOpts.insert(compileOpts.end(), options.begin(), options.end());

  if (!fgpu_rdc_) {
    compileOpts.push_back("-Xclang");
    compileOpts.push_back("-disable-llvm-passes");
  }

  if (!transformOptions(compileOpts)) {
    LogError("Error in hiprtc: unable to transform options");
    return false;
  }

  if (!compileToBitCode(compile_input_, isa_, compileOpts, build_log_, LLVMBitcode_)) {
    LogError("Error in hiprtc: unable to compile source to bitcode");
    return false;
  }

  if (fgpu_rdc_) {
    std::vector<std::string> mangledNames;
    if (!fillMangledNames(LLVMBitcode_, mangledNames, true)) {
      LogError("Error in hiprtc: unable to fill mangled names");
      return false;
    }

    if (!getDemangledNames(mangledNames, demangled_names_)) {
      LogError("Error in hiprtc: unable to get demangled names");
      return false;
    }
    return true;
  }

  std::string linkFileName = "linked";
  if (!addCodeObjData(link_input_, LLVMBitcode_, linkFileName, AMD_COMGR_DATA_KIND_BC)) {
    LogError("Error in hiprtc: unable to add linked code object");
    return false;
  }

  std::vector<char> LinkedLLVMBitcode;
  if (!linkLLVMBitcode(link_input_, isa_, link_options_, build_log_, LinkedLLVMBitcode)) {
    LogError("Error in hiprtc: unable to add device libs to linked bitcode");
    return false;
  }

  std::string linkedFileName = "LLVMBitcode.bc";
  if (!addCodeObjData(exec_input_, LinkedLLVMBitcode, linkedFileName, AMD_COMGR_DATA_KIND_BC)) {
    LogError("Error in hiprtc: unable to add device libs linked code object");
    return false;
  }

  std::vector<std::string> llvmOptions;
  // Find the -mllvm options passed by the app such as "-mllvm" "-amdgpu-early-inline-all=true"
  if (!findLLVMOptions(options, llvmOptions)) {
    LogError("Error in hiprtc: unable to match -mllvm options");
    return false;
  }

  std::vector<std::string> exeOpts(exe_options_);
  exeOpts.reserve(exeOpts.size() + llvmOptions.size() + 2);
  // Add these options by default for optimizations during BC to Relocatable phase.
  exeOpts.push_back("-mllvm");
  exeOpts.push_back("-amdgpu-internalize-symbols");
  // User provided -mllvm options are appended at the end since they can override the above
  // default options if necessary
  exeOpts.insert(exeOpts.end(), llvmOptions.begin(), llvmOptions.end());

  if (settings_.dumpISA) {
    if (!dumpIsaFromBC(exec_input_, isa_, exeOpts, name_, build_log_)) {
      LogError("Error in hiprtc: unable to dump isa code");
      return false;
    }
  }

  if (!createExecutable(exec_input_, isa_, exeOpts, build_log_, executable_)) {
    LogError("Error in hiprtc: unable to create executable");
    return false;
  }

  std::vector<std::string> mangledNames;
  if (!fillMangledNames(executable_, mangledNames, false)) {
    LogError("Error in hiprtc: unable to fill mangled names");
    return false;
  }

  if (!getDemangledNames(mangledNames, demangled_names_)) {
    LogError("Error in hiprtc: unable to get demangled names");
    return false;
  }

  return true;
}

void RTCCompileProgram::stripNamedExpression(std::string& strippedName) {
  if (strippedName.back() == ')') {
    strippedName.pop_back();
    strippedName.erase(0, strippedName.find('('));
  }
  if (strippedName.front() == '&') {
    strippedName.erase(0, 1);
  }
  // Removes the spaces from strippedName if present
  strippedName.erase(std::remove_if(strippedName.begin(), strippedName.end(),
                                    [](unsigned char c) { return std::isspace(c); }),
                     strippedName.end());
}

bool RTCCompileProgram::trackMangledName(std::string& name) {
  amd::ScopedLock lock(lock_);

  if (name.size() == 0) return false;

  std::string strippedNameNoSpace = name;
  stripNamedExpression(strippedNameNoSpace);

  stripped_names_.insert(std::pair<std::string, std::string>(name, strippedNameNoSpace));
  demangled_names_.insert(std::pair<std::string, std::string>(strippedNameNoSpace, ""));

  const auto var{"__hiprtc_" + std::to_string(stripped_names_.size())};
  const auto code{"\nextern \"C\" constexpr auto " + var + " = " + name + ";\n"};

  source_code_ += code;
  return true;
}

bool RTCCompileProgram::getMangledName(const char* name_expression, const char** loweredName) {
  std::string strippedName = name_expression;
  stripNamedExpression(strippedName);

  if (auto dres = demangled_names_.find(strippedName); dres != demangled_names_.end()) {
    if (dres->second.size() != 0) {
      *loweredName = dres->second.c_str();
      return true;
    } else
      return false;
  }
  return false;
}

bool RTCCompileProgram::GetBitcode(char* bitcode) {
  if (!fgpu_rdc_ || LLVMBitcode_.size() <= 0) {
    return false;
  }

  std::copy(LLVMBitcode_.begin(), LLVMBitcode_.end(), bitcode);
  return true;
}

bool RTCCompileProgram::GetBitcodeSize(size_t* bitcode_size) {
  if (!fgpu_rdc_ || LLVMBitcode_.size() <= 0) {
    return false;
  }

  *bitcode_size = LLVMBitcode_.size();
  return true;
}

// RTC Link Program Member Functions
RTCLinkProgram::RTCLinkProgram(std::string name) : RTCProgram(name) {
  if (amd::Comgr::create_data_set(&link_input_) != AMD_COMGR_STATUS_SUCCESS) {
    crashWithMessage("Failed to allocate internal hiprtc structure");
  }
}

bool RTCLinkProgram::AddLinkerOptions(unsigned int num_options, hiprtcJIT_option* options_ptr,
                                      void** options_vals_ptr) {
  for (size_t opt_idx = 0; opt_idx < num_options; ++opt_idx) {
    if (options_vals_ptr[opt_idx] == nullptr) {
      LogError("Options value can not be nullptr");
      return false;
    }

    switch (options_ptr[opt_idx]) {
      case HIPRTC_JIT_MAX_REGISTERS:
        link_args_.max_registers_ = *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_THREADS_PER_BLOCK:
        link_args_.threads_per_block_ =
            *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_WALL_TIME:
        link_args_.wall_time_ = *(reinterpret_cast<long*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_INFO_LOG_BUFFER:
        link_args_.info_log_ = (reinterpret_cast<char*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_INFO_LOG_BUFFER_SIZE_BYTES:
        link_args_.info_log_size_ = (reinterpret_cast<size_t>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_ERROR_LOG_BUFFER:
        link_args_.error_log_ = reinterpret_cast<char*>(options_vals_ptr[opt_idx]);
        break;
      case HIPRTC_JIT_ERROR_LOG_BUFFER_SIZE_BYTES:
        link_args_.error_log_size_ = (reinterpret_cast<size_t>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_OPTIMIZATION_LEVEL:
        link_args_.optimization_level_ =
            *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_TARGET_FROM_HIPCONTEXT:
        link_args_.target_from_hip_context_ =
            *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_TARGET:
        link_args_.jit_target_ = *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_FALLBACK_STRATEGY:
        link_args_.fallback_strategy_ =
            *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_GENERATE_DEBUG_INFO:
        link_args_.generate_debug_info_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_LOG_VERBOSE:
        link_args_.log_verbose_ = reinterpret_cast<long>(options_vals_ptr[opt_idx]);
        break;
      case HIPRTC_JIT_GENERATE_LINE_INFO:
        link_args_.generate_line_info_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_CACHE_MODE:
        link_args_.cache_mode_ = *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_NEW_SM3X_OPT:
        link_args_.sm3x_opt_ = *(reinterpret_cast<bool*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_FAST_COMPILE:
        link_args_.fast_compile_ = *(reinterpret_cast<bool*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_GLOBAL_SYMBOL_NAMES:
        link_args_.global_symbol_names_ = reinterpret_cast<const char**>(options_vals_ptr[opt_idx]);
        break;
      case HIPRTC_JIT_GLOBAL_SYMBOL_ADDRESS:
        link_args_.global_symbol_addresses_ = reinterpret_cast<void**>(options_vals_ptr[opt_idx]);
        break;
      case HIPRTC_JIT_GLOBAL_SYMBOL_COUNT:
        link_args_.global_symbol_count_ =
            *(reinterpret_cast<unsigned int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_LTO:
        link_args_.lto_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_FTZ:
        link_args_.ftz_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_PREC_DIV:
        link_args_.prec_div_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_PREC_SQRT:
        link_args_.prec_sqrt_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_FMA:
        link_args_.fma_ = *(reinterpret_cast<int*>(options_vals_ptr[opt_idx]));
        break;
      case HIPRTC_JIT_IR_TO_ISA_OPT_EXT:
        link_args_.linker_ir2isa_args_ = reinterpret_cast<const char**>(options_vals_ptr[opt_idx]);
        break;
      case HIPRTC_JIT_IR_TO_ISA_OPT_COUNT_EXT:
        link_args_.linker_ir2isa_args_count_ = reinterpret_cast<size_t>(options_vals_ptr[opt_idx]);
        break;
      default:
        break;
    }
  }

  return true;
}

amd_comgr_data_kind_t RTCLinkProgram::GetCOMGRDataKind(hiprtcJITInputType input_type) {
  amd_comgr_data_kind_t data_kind = AMD_COMGR_DATA_KIND_UNDEF;

  // Map the hiprtc input type to comgr data kind
  switch (input_type) {
    case HIPRTC_JIT_INPUT_LLVM_BITCODE:
      data_kind = AMD_COMGR_DATA_KIND_BC;
      break;
    case HIPRTC_JIT_INPUT_LLVM_BUNDLED_BITCODE:
      data_kind =
          HIPRTC_USE_RUNTIME_UNBUNDLER ? AMD_COMGR_DATA_KIND_BC : AMD_COMGR_DATA_KIND_BC_BUNDLE;
      break;
    case HIPRTC_JIT_INPUT_LLVM_ARCHIVES_OF_BUNDLED_BITCODE:
      data_kind = AMD_COMGR_DATA_KIND_AR_BUNDLE;
      break;
    default:
      LogError("Cannot find the corresponding comgr data kind");
      break;
  }

  return data_kind;
}

bool RTCLinkProgram::AddLinkerDataImpl(std::vector<char>& link_data, hiprtcJITInputType input_type,
                                       std::string& link_file_name) {
  std::vector<char> llvm_bitcode;
  // If this is bundled bitcode then unbundle this.
  if (HIPRTC_USE_RUNTIME_UNBUNDLER && input_type == HIPRTC_JIT_INPUT_LLVM_BUNDLED_BITCODE) {
    if (!findIsa()) {
      return false;
    }

    size_t co_offset = 0;
    size_t co_size = 0;
    if (!UnbundleBitCode(link_data, isa_, co_offset, co_size)) {
      LogError("Error in hiprtc: unable to unbundle the llvm bitcode");
      return false;
    }

    llvm_bitcode.assign(link_data.begin() + co_offset, link_data.begin() + co_offset + co_size);
  } else {
    llvm_bitcode.assign(link_data.begin(), link_data.end());
  }

  amd_comgr_data_kind_t data_kind;
  if ((data_kind = GetCOMGRDataKind(input_type)) == AMD_COMGR_DATA_KIND_UNDEF) {
    LogError("Cannot find the correct COMGR data kind");
    return false;
  }

  if (!addCodeObjData(link_input_, llvm_bitcode, link_file_name, data_kind)) {
    LogError("Error in hiprtc: unable to add linked code object");
    return false;
  }

  return true;
}

bool RTCLinkProgram::AddLinkerFile(std::string file_path, hiprtcJITInputType input_type) {
  std::vector<char> link_file_info;

  std::ifstream file_stream{file_path};
  if (!file_stream.good()) {
    return false;
  }
  std::copy(std::istream_iterator<char>(file_stream), std::istream_iterator<char>(),
            std::back_inserter(link_file_info));
  file_stream.close();

  // Read the file contents
  std::string link_file_name("Linker Program");

  return AddLinkerDataImpl(link_file_info, input_type, link_file_name);
}

bool RTCLinkProgram::AddLinkerData(void* image_ptr, size_t image_size, std::string link_file_name,
                                   hiprtcJITInputType input_type) {
  char* image_char_buf = reinterpret_cast<char*>(image_ptr);
  std::vector<char> bundled_llvm_bitcode(image_char_buf, image_char_buf + image_size);

  return AddLinkerDataImpl(bundled_llvm_bitcode, input_type, link_file_name);
}

bool RTCLinkProgram::LinkComplete(void** bin_out, size_t* size_out) {
  if (!findIsa()) {
    return false;
  }

  std::vector<char> linked_llvm_bitcode;
  std::vector<std::string> linkopts;
  if (!linkLLVMBitcode(link_input_, isa_, linkopts, build_log_, linked_llvm_bitcode)) {
    LogError("Error in hiprtc: unable to add device libs to linked bitcode");
    return false;
  }

  std::string linkedFileName = "LLVMBitcode.bc";
  if (!addCodeObjData(exec_input_, linked_llvm_bitcode, linkedFileName, AMD_COMGR_DATA_KIND_BC)) {
    LogError("Error in hiprtc: unable to add linked bitcode");
    return false;
  }

  std::vector<std::string> exe_options = getLinkOptions(link_args_);
  exe_options.push_back("-O3");
  if (!createExecutable(exec_input_, isa_, exe_options, build_log_, executable_)) {
    LogError("Error in hiprtc: unable to create exectuable");
    return false;
  }

  *size_out = executable_.size();
  char* bin_out_c = new char[*size_out];
  std::copy(executable_.begin(), executable_.end(), bin_out_c);
  *bin_out = reinterpret_cast<void*>(bin_out_c);

  return true;
}

}  // namespace hiprtc
