/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/wim_applier.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

namespace rufus::core {
namespace {

constexpr std::uint32_t makeWimlibVersion(const unsigned int major,
                                          const unsigned int minor,
                                          const unsigned int patch) {
  return (major << 20U) | (minor << 10U) | patch;
}

constexpr std::uint32_t kMinimumWimlibVersion = makeWimlibVersion(1, 13, 4);
constexpr int kOpenCheckIntegrity = 0x00000001;
constexpr int kExtractNtfs = 0x00000001;
constexpr int kProgressAbort = 1;
constexpr int kProgressExtractBegin = 0;
constexpr int kProgressExtractStreams = 4;
constexpr int kProgressExtractEnd = 7;
constexpr int kUpdateAdd = 0;

#if defined(_WIN32)
using NativeCharacter = wchar_t;
using ModuleHandle = HMODULE;
#else
using NativeCharacter = char;
using ModuleHandle = void*;
#endif

struct WimHandle;
using ProgressFunction = int (*)(int, void*, void*);
using OpenWimFunction = int (*)(const NativeCharacter*, int, WimHandle**,
                                ProgressFunction, void*);
using UpdateImageFunction = int (*)(WimHandle*, int, const void*, std::size_t, int);
using ExtractImageFunction = int (*)(WimHandle*, int, const NativeCharacter*, int);
using FreeFunction = void (*)(WimHandle*);
using GetErrorStringFunction = const NativeCharacter* (*)(int);
using GetVersionFunction = std::uint32_t (*)();

struct WimAddCommand final {
  NativeCharacter* fileSystemSourcePath{};
  NativeCharacter* wimTargetPath{};
  NativeCharacter* configurationFile{};
  int addFlags{};
};

// WIMLIB_UPDATE_OP_ADD is the first union member in wimlib_update_command,
// therefore this standard-layout prefix has the exact ABI needed for one add.
struct WimUpdateAddCommand final {
  int operation{kUpdateAdd};
  WimAddCommand add;
};

struct ExtractProgressInfo final {
  std::uint32_t image{};
  std::uint32_t flags{};
  const NativeCharacter* wimFileName{};
  const NativeCharacter* imageName{};
  const NativeCharacter* target{};
  const NativeCharacter* reserved{};
  std::uint64_t totalBytes{};
  std::uint64_t completedBytes{};
};

struct ProgressContext final {
  const WimApplyProgressCallback* onProgress{};
  const WimApplyCancelCallback* isCancelled{};
  bool cancellationObserved{};
};

int reportProgress(const int message, void* rawInfo, void* rawContext) {
  auto* const context = static_cast<ProgressContext*>(rawContext);
  if (context == nullptr) {
    return 0;
  }
  if (context->isCancelled != nullptr && *context->isCancelled &&
      (*context->isCancelled)()) {
    context->cancellationObserved = true;
    return kProgressAbort;
  }
  if ((message == kProgressExtractBegin || message == kProgressExtractStreams ||
       message == kProgressExtractEnd) &&
      rawInfo != nullptr && context->onProgress != nullptr && *context->onProgress) {
    const auto* const info = static_cast<const ExtractProgressInfo*>(rawInfo);
    (*context->onProgress)({info->completedBytes, info->totalBytes});
  }
  return 0;
}

#if !defined(_WIN32)
std::filesystem::path executableDirectory() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  static_cast<void>(_NSGetExecutablePath(nullptr, &size));
  if (size == 0) {
    return {};
  }
  std::vector<char> path(size);
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::path(path.data()).parent_path();
#else
  std::array<char, 32768> path{};
  const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1U);
  if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
    return {};
  }
  path[static_cast<std::size_t>(length)] = '\0';
  return std::filesystem::path(path.data()).parent_path();
#endif
}
#endif

std::string nativeString(const NativeCharacter* value) {
  if (value == nullptr) {
    return {};
  }
#if defined(_WIN32)
  const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                           nullptr, 0, nullptr, nullptr);
  if (required <= 1) {
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                          result.data(), required, nullptr, nullptr) == 0) {
    return {};
  }
  result.resize(static_cast<std::size_t>(required - 1));
  return result;
#else
  return value;
#endif
}

class DynamicModule final {
 public:
  DynamicModule() { load(); }
  ~DynamicModule() {
    if (handle_ == nullptr) {
      return;
    }
#if defined(_WIN32)
    FreeLibrary(handle_);
#else
    dlclose(handle_);
#endif
  }

  DynamicModule(const DynamicModule&) = delete;
  DynamicModule& operator=(const DynamicModule&) = delete;

  [[nodiscard]] bool loaded() const noexcept { return handle_ != nullptr; }

  template <typename Function>
  [[nodiscard]] Function symbol(const char* name) const noexcept {
    if (handle_ == nullptr) {
      return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<Function>(GetProcAddress(handle_, name));
#else
    return reinterpret_cast<Function>(dlsym(handle_, name));
#endif
  }

 private:
  void load() {
#if defined(_WIN32)
    constexpr std::array<const wchar_t*, 3> names =
        {L"libwim-15.dll", L"libwim.dll", L"wimlib.dll"};
    std::array<wchar_t, 32768> executablePath{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
                                            static_cast<DWORD>(executablePath.size()));
    if (length != 0 && static_cast<std::size_t>(length) < executablePath.size()) {
      const auto directory = std::filesystem::path(executablePath.data()).parent_path();
      for (const wchar_t* const name : names) {
        handle_ = LoadLibraryExW((directory / name).c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (handle_ != nullptr) {
          return;
        }
      }
    }
    for (const wchar_t* const name : names) {
      handle_ = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (handle_ != nullptr) {
        return;
      }
    }
#elif defined(__APPLE__)
    const auto executable = executableDirectory();
    std::vector<std::filesystem::path> names;
    if (!executable.empty()) {
      names.push_back(executable / "libwim.15.dylib");
      names.push_back(executable / "libwim.dylib");
      names.push_back(executable / "../Frameworks/libwim.15.dylib");
      names.push_back(executable / "../Frameworks/libwim.dylib");
    }
    names.emplace_back("/opt/homebrew/lib/libwim.15.dylib");
    names.emplace_back("/usr/local/lib/libwim.15.dylib");
    names.emplace_back("libwim.15.dylib");
    names.emplace_back("/opt/homebrew/lib/libwim.dylib");
    names.emplace_back("/usr/local/lib/libwim.dylib");
    names.emplace_back("libwim.dylib");
    for (const auto& name : names) {
      handle_ = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (handle_ != nullptr) {
        return;
      }
    }
#else
    const auto executable = executableDirectory();
    std::vector<std::filesystem::path> names;
    if (!executable.empty()) {
      names.push_back(executable / "libwim.so.15");
      names.push_back(executable / "libwim.so");
    }
    names.emplace_back("libwim.so.15");
    names.emplace_back("libwim.so");
    for (const auto& name : names) {
      handle_ = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (handle_ != nullptr) {
        return;
      }
    }
#endif
  }

  ModuleHandle handle_{};
};

class SystemWimApplicator final : public WimApplicator {
 public:
  SystemWimApplicator()
      : openWim_(module_.symbol<OpenWimFunction>("wimlib_open_wim_with_progress")),
        updateImage_(module_.symbol<UpdateImageFunction>("wimlib_update_image")),
        extractImage_(module_.symbol<ExtractImageFunction>("wimlib_extract_image")),
        freeWim_(module_.symbol<FreeFunction>("wimlib_free")),
        getErrorString_(module_.symbol<GetErrorStringFunction>("wimlib_get_error_string")),
        getVersion_(module_.symbol<GetVersionFunction>("wimlib_get_version")) {
    if (!module_.loaded()) {
      reason_ = "wimlib is not installed or bundled; Windows To Go requires wimlib 1.13.4 or later";
      return;
    }
    if (openWim_ == nullptr || updateImage_ == nullptr || extractImage_ == nullptr ||
        freeWim_ == nullptr || getErrorString_ == nullptr || getVersion_ == nullptr) {
      reason_ = "The loaded wimlib library does not expose the required update/apply API";
      return;
    }
    if (getVersion_() < kMinimumWimlibVersion) {
      reason_ = "wimlib 1.13.4 or later is required for cancellable Windows image application";
    }
  }

  [[nodiscard]] bool available() const noexcept override { return reason_.empty(); }
  [[nodiscard]] std::string availabilityReason() const override { return reason_; }

  [[nodiscard]] WimApplyResult apply(
      const std::filesystem::path& sourceWim, const std::uint32_t editionIndex,
      const std::filesystem::path& targetPath, const bool directNtfsVolume,
      const std::filesystem::path& unattendedFile,
      const WimApplyProgressCallback& onProgress,
      const WimApplyCancelCallback& isCancelled) const override {
    WimApplyResult result;
    if (!available()) {
      result.error = reason_;
      return result;
    }
    if (editionIndex == 0 || sourceWim.empty() || targetPath.empty() ||
        unattendedFile.empty()) {
      result.error = "Invalid Windows image application request";
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Windows image application was cancelled before it began";
      return result;
    }

    ProgressContext context{&onProgress, &isCancelled, false};
    WimHandle* wim = nullptr;
    int status = openWim_(sourceWim.c_str(), kOpenCheckIntegrity, &wim,
                          reportProgress, &context);
    if (status != 0 || wim == nullptr) {
      if (wim != nullptr) {
        freeWim_(wim);
      }
      result.cancelled = context.cancellationObserved;
      result.error = "Unable to open the Windows install image: " +
                     nativeString(getErrorString_(status));
      return result;
    }

    std::basic_string<NativeCharacter> targetInWim =
#if defined(_WIN32)
        L"\\Windows\\Panther\\unattend.xml";
#else
        "\\Windows\\Panther\\unattend.xml";
#endif
    WimUpdateAddCommand command;
    command.add.fileSystemSourcePath =
        const_cast<NativeCharacter*>(unattendedFile.c_str());
    command.add.wimTargetPath = targetInWim.data();
    status = updateImage_(wim, static_cast<int>(editionIndex), &command, 1, 0);
    if (status == 0 && !(isCancelled && isCancelled())) {
      status = extractImage_(wim, static_cast<int>(editionIndex), targetPath.c_str(),
                             directNtfsVolume ? kExtractNtfs : 0);
    } else if (isCancelled && isCancelled()) {
      context.cancellationObserved = true;
    }
    freeWim_(wim);

    if (status != 0 || context.cancellationObserved) {
      result.cancelled = context.cancellationObserved;
      result.error = result.cancelled
                         ? "Windows image application was cancelled"
                         : "Unable to apply the selected Windows edition: " +
                               nativeString(getErrorString_(status));
      return result;
    }
    result.success = true;
    return result;
  }

 private:
  DynamicModule module_;
  OpenWimFunction openWim_{};
  UpdateImageFunction updateImage_{};
  ExtractImageFunction extractImage_{};
  FreeFunction freeWim_{};
  GetErrorStringFunction getErrorString_{};
  GetVersionFunction getVersion_{};
  std::string reason_;
};

}  // namespace

std::shared_ptr<const WimApplicator> createSystemWimApplicator() {
  return std::make_shared<SystemWimApplicator>();
}

}  // namespace rufus::core
