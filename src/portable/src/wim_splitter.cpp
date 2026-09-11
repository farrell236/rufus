/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/wim_splitter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
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
constexpr int kWimlibOpenCheckIntegrity = 0x00000001;
constexpr int kWimlibWriteCheckIntegrity = 0x00000001;
constexpr int kWimlibAllImages = -1;
constexpr int kWimlibCompressionLzx = 2;
constexpr int kWimlibProgressAbort = 1;
constexpr int kWimlibProgressSplitBeginPart = 19;
constexpr int kWimlibProgressSplitEndPart = 20;

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
using SplitFunction = int (*)(WimHandle*, const NativeCharacter*, std::uint64_t, int);
using CreateNewWimFunction = int (*)(int, WimHandle**);
using ExportImageFunction = int (*)(WimHandle*, int, WimHandle*,
                                    const NativeCharacter*, const NativeCharacter*, int);
using RegisterProgressFunction = void (*)(WimHandle*, ProgressFunction, void*);
using FreeFunction = void (*)(WimHandle*);
using GetErrorStringFunction = const NativeCharacter* (*)(int);
using GetVersionFunction = std::uint32_t (*)();

struct SplitProgressInfo final {
  std::uint64_t totalBytes;
  std::uint64_t completedBytes;
  unsigned int currentPart;
  unsigned int totalParts;
  NativeCharacter* partName;
};

struct ProgressContext final {
  const WimSplitProgressCallback* onProgress{};
  const WimSplitCancelCallback* isCancelled{};
  bool cancellationObserved{};
};

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

int reportProgress(const int message, void* rawInfo, void* rawContext) {
  auto* const context = static_cast<ProgressContext*>(rawContext);
  if (context == nullptr) {
    return 0;
  }
  if (context->isCancelled != nullptr && *context->isCancelled &&
      (*context->isCancelled)()) {
    context->cancellationObserved = true;
    return kWimlibProgressAbort;
  }
  if ((message == kWimlibProgressSplitBeginPart ||
       message == kWimlibProgressSplitEndPart) &&
      rawInfo != nullptr && context->onProgress != nullptr && *context->onProgress) {
    const auto* const info = static_cast<const SplitProgressInfo*>(rawInfo);
    (*context->onProgress)({info->completedBytes, info->totalBytes,
                            info->currentPart, info->totalParts});
  }
  return 0;
}

std::filesystem::path numberedPartPath(const std::filesystem::path& firstPart,
                                       const unsigned int partNumber) {
  if (partNumber <= 1) {
    return firstPart;
  }
  return firstPart.parent_path() /
         (firstPart.stem().native() +
#if defined(_WIN32)
          std::to_wstring(partNumber) +
#else
          std::to_string(partNumber) +
#endif
          firstPart.extension().native());
}

void removeProducedParts(const std::filesystem::path& firstPart) {
  for (unsigned int part = 1; part <= 9999; ++part) {
    const auto path = numberedPartPath(firstPart, part);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
      break;
    }
    std::filesystem::remove(path, error);
  }
}

bool hasEsdExtension(const std::filesystem::path& path) {
  auto extension = path.extension().string();
  for (char& character : extension) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return extension == ".esd";
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
    if (length != 0 &&
        static_cast<std::size_t>(length) < executablePath.size()) {
      const std::filesystem::path directory =
          std::filesystem::path(executablePath.data()).parent_path();
      for (const wchar_t* const name : names) {
        const std::filesystem::path candidate = directory / name;
        handle_ = LoadLibraryExW(candidate.c_str(), nullptr,
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

class SystemWimSplitter final : public WimSplitter {
 public:
  SystemWimSplitter()
      : openWim_(module_.symbol<OpenWimFunction>(
            "wimlib_open_wim_with_progress")),
        splitWim_(module_.symbol<SplitFunction>("wimlib_split")),
        createNewWim_(module_.symbol<CreateNewWimFunction>("wimlib_create_new_wim")),
        exportImage_(module_.symbol<ExportImageFunction>("wimlib_export_image")),
        registerProgress_(module_.symbol<RegisterProgressFunction>(
            "wimlib_register_progress_function")),
        freeWim_(module_.symbol<FreeFunction>("wimlib_free")),
        getErrorString_(module_.symbol<GetErrorStringFunction>("wimlib_get_error_string")),
        getVersion_(module_.symbol<GetVersionFunction>("wimlib_get_version")) {
    if (!module_.loaded()) {
      reason_ = "wimlib is not installed or bundled; install wimlib 1.13.4 or later to prepare a large install.wim or install.esd";
      return;
    }
    if (openWim_ == nullptr || splitWim_ == nullptr || createNewWim_ == nullptr ||
        exportImage_ == nullptr || registerProgress_ == nullptr ||
        freeWim_ == nullptr || getErrorString_ == nullptr || getVersion_ == nullptr) {
      reason_ = "The loaded wimlib library does not expose the required Windows image transformation API";
      return;
    }
    if (getVersion_() < kMinimumWimlibVersion) {
      reason_ = "wimlib 1.13.4 or later is required for cancellable WIM splitting";
    }
  }

  [[nodiscard]] bool available() const noexcept override { return reason_.empty(); }

  [[nodiscard]] std::string availabilityReason() const override { return reason_; }

  [[nodiscard]] WimSplitResult split(
      const std::filesystem::path& sourceWim,
      const std::filesystem::path& firstPartPath,
      const std::uint64_t maximumPartBytes,
      const WimSplitProgressCallback& onProgress,
      const WimSplitCancelCallback& isCancelled) const override {
    WimSplitResult result;
    if (!available()) {
      result.error = reason_;
      return result;
    }
    if (maximumPartBytes == 0 || sourceWim.empty() || firstPartPath.empty()) {
      result.error = "Invalid WIM split request";
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "WIM splitting was cancelled before it began";
      return result;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(sourceWim, fileError) || fileError) {
      result.error = "The extracted Windows install image is not a readable regular file";
      return result;
    }
    if (std::filesystem::exists(firstPartPath, fileError) || fileError) {
      result.error = fileError ? "Unable to validate the WIM split output: " +
                                     fileError.message()
                               : "The WIM split output already exists";
      return result;
    }

    ProgressContext context{&onProgress, &isCancelled, false};
    WimHandle* source = nullptr;
    int status = openWim_(sourceWim.c_str(), kWimlibOpenCheckIntegrity, &source,
                          reportProgress, &context);
    if (status != 0 || source == nullptr) {
      if (source != nullptr) {
        freeWim_(source);
      }
      result.cancelled = context.cancellationObserved || (isCancelled && isCancelled());
      result.error = result.cancelled
                         ? "Windows image preparation was cancelled while validating the source"
                         : "wimlib could not open the Windows install image: " + errorText(status);
      return result;
    }

    WimHandle* splitSource = source;
    WimHandle* converted = nullptr;
    if (hasEsdExtension(sourceWim)) {
      status = createNewWim_(kWimlibCompressionLzx, &converted);
      if (status == 0 && converted != nullptr) {
        registerProgress_(converted, reportProgress, &context);
        status = exportImage_(source, kWimlibAllImages, converted, nullptr,
                              nullptr, 0);
      }
      if (status != 0 || converted == nullptr) {
        if (converted != nullptr) {
          freeWim_(converted);
        }
        freeWim_(source);
        result.cancelled = context.cancellationObserved ||
                           (isCancelled && isCancelled());
        result.error = result.cancelled
                           ? "Windows image preparation was cancelled while converting install.esd"
                           : "wimlib could not convert install.esd to an ordinary Windows image: " +
                                 errorText(status);
        return result;
      }
      splitSource = converted;
    }

    status = splitWim_(splitSource, firstPartPath.c_str(), maximumPartBytes,
                       kWimlibWriteCheckIntegrity);
    if (converted != nullptr) {
      freeWim_(converted);
    }
    freeWim_(source);
    if (status != 0) {
      removeProducedParts(firstPartPath);
      result.cancelled = context.cancellationObserved || (isCancelled && isCancelled());
      result.error = result.cancelled
                         ? "Windows image preparation was cancelled"
                         : "wimlib could not create split Windows image parts: " +
                               errorText(status);
      return result;
    }

    for (unsigned int part = 1; part <= 9999; ++part) {
      const auto path = numberedPartPath(firstPartPath, part);
      fileError.clear();
      if (!std::filesystem::exists(path, fileError)) {
        break;
      }
      if (fileError || !std::filesystem::is_regular_file(path, fileError)) {
        removeProducedParts(firstPartPath);
        result.error = "wimlib produced an invalid split-WIM output";
        return result;
      }
      result.parts.push_back(path);
    }
    if (result.parts.empty()) {
      result.error = "wimlib reported success but produced no split-WIM parts";
      return result;
    }
    result.success = true;
    return result;
  }

 private:
  [[nodiscard]] std::string errorText(const int status) const {
    const std::string text = nativeString(getErrorString_(status));
    return text.empty() ? "error " + std::to_string(status) : text;
  }

  DynamicModule module_;
  OpenWimFunction openWim_{};
  SplitFunction splitWim_{};
  CreateNewWimFunction createNewWim_{};
  ExportImageFunction exportImage_{};
  RegisterProgressFunction registerProgress_{};
  FreeFunction freeWim_{};
  GetErrorStringFunction getErrorString_{};
  GetVersionFunction getVersion_{};
  std::string reason_;
};

}  // namespace

std::shared_ptr<const WimSplitter> createSystemWimSplitter() {
  static const std::shared_ptr<const WimSplitter> splitter =
      std::make_shared<SystemWimSplitter>();
  return splitter;
}

}  // namespace rufus::core
