/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "macos_installer_operations.hpp"

#import <CoreFoundation/CoreFoundation.h>
#import <Security/Security.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/disk.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "macos_raw_device_operations.hpp"
#include "rufus/core/safety_policy.hpp"

extern char** environ;

namespace rufus::backend::macos {
namespace {

class FileDescriptor final {
 public:
  explicit FileDescriptor(const int value = -1) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }
  void reset() noexcept {
    if (value_ >= 0) {
      close(value_);
      value_ = -1;
    }
  }

 private:
  int value_;
};

template <typename T>
class CfObject final {
 public:
  explicit CfObject(T value = nullptr) : value_(value) {}
  ~CfObject() {
    if (value_ != nullptr) {
      CFRelease(value_);
    }
  }
  CfObject(const CfObject&) = delete;
  CfObject& operator=(const CfObject&) = delete;
  [[nodiscard]] T get() const noexcept { return value_; }

 private:
  T value_;
};

std::string cfString(CFStringRef value) {
  if (value == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(value);
  const CFIndex capacity =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  if (capacity <= 1) {
    return {};
  }
  std::vector<char> buffer(static_cast<std::size_t>(capacity));
  return CFStringGetCString(value, buffer.data(), capacity,
                            kCFStringEncodingUTF8)
             ? std::string(buffer.data())
             : std::string{};
}

std::string osStatusMessage(const OSStatus status) {
  CfObject<CFStringRef> message(SecCopyErrorMessageString(status, nullptr));
  const std::string detail = cfString(message.get());
  return detail.empty() ? "OSStatus " + std::to_string(status) : detail;
}

std::string infoString(CFBundleRef bundle, CFStringRef key) {
  const CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(bundle, key);
  return value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()
             ? cfString(static_cast<CFStringRef>(value))
             : std::string{};
}

bool unchangedRegularFile(const std::filesystem::path& path,
                          struct stat& status, std::string& error) {
  struct stat linkStatus {};
  if (lstat(path.c_str(), &linkStatus) != 0) {
    error = "Unable to inspect createinstallmedia: " +
            std::string(std::strerror(errno));
    return false;
  }
  if (S_ISLNK(linkStatus.st_mode) || !S_ISREG(linkStatus.st_mode) ||
      (linkStatus.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
    error = "createinstallmedia must be a directly contained executable regular file";
    return false;
  }
  status = linkStatus;
  return true;
}

bool appleSignedBundle(const std::filesystem::path& path, std::string& error) {
  CfObject<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(path.string().size()), true));
  if (url.get() == nullptr) {
    error = "Unable to create a security reference for the installer application";
    return false;
  }
  SecStaticCodeRef rawCode = nullptr;
  OSStatus status =
      SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &rawCode);
  CfObject<SecStaticCodeRef> code(rawCode);
  if (status != errSecSuccess || code.get() == nullptr) {
    error = "Unable to inspect the installer application's code signature: " +
            osStatusMessage(status);
    return false;
  }
  SecRequirementRef rawRequirement = nullptr;
  status = SecRequirementCreateWithString(CFSTR("anchor apple"),
                                          kSecCSDefaultFlags,
                                          &rawRequirement);
  CfObject<SecRequirementRef> requirement(rawRequirement);
  if (status != errSecSuccess || requirement.get() == nullptr) {
    error = "Unable to construct the Apple code-signing requirement: " +
            osStatusMessage(status);
    return false;
  }
  status = SecStaticCodeCheckValidity(code.get(), kSecCSStrictValidate,
                                      requirement.get());
  if (status != errSecSuccess) {
    error = "The selected application is not an intact Apple-signed macOS installer: " +
            osStatusMessage(status);
    return false;
  }
  return true;
}

std::uint64_t payloadSize(const std::filesystem::path& root,
                          std::vector<std::string>& warnings) {
  std::uint64_t total = 0;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    const auto size = iterator->file_size(error);
    if (error) {
      error.clear();
      continue;
    }
    if (size > std::numeric_limits<std::uint64_t>::max() - total) {
      warnings.emplace_back("Installer payload size overflowed; target sizing will rely on createinstallmedia.");
      return 0;
    }
    total += size;
  }
  if (error) {
    warnings.emplace_back(
        "Some installer files could not be sized; createinstallmedia will perform final validation.");
  }
  return total;
}

void report(const core::MacOsInstallerProgressCallback& callback,
            const core::MacOsInstallerStage stage,
            const std::uint64_t processed, const std::uint64_t total,
            std::string detail = {}) {
  if (callback) {
    callback({stage, processed, total, std::move(detail)});
  }
}

struct ProcessResult final {
  bool success{};
  bool cancelled{};
  int exitStatus{-1};
  std::string output;
  std::string error;
};

ProcessResult runProcess(
    const std::string& executable, const std::vector<std::string>& arguments,
    const core::MacOsInstallerCancelCallback& isCancelled,
    const std::function<void(const std::string&)>& onOutput = {}) {
  ProcessResult result;
  int outputPipe[2]{-1, -1};
  if (pipe(outputPipe) != 0) {
    result.error = "Unable to create a tool output pipe: " +
                   std::string(std::strerror(errno));
    return result;
  }
  FileDescriptor readEnd(outputPipe[0]);
  FileDescriptor writeEnd(outputPipe[1]);

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attributes);
  posix_spawn_file_actions_adddup2(&actions, writeEnd.get(), STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, writeEnd.get(), STDERR_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_addclose(&actions, readEnd.get());
  short flags = POSIX_SPAWN_SETPGROUP;
  posix_spawnattr_setflags(&attributes, flags);
  posix_spawnattr_setpgroup(&attributes, 0);

  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1U);
  storage.push_back(executable);
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1U);
  for (auto& value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);

  pid_t child = -1;
  const int spawnError = posix_spawn(&child, executable.c_str(), &actions,
                                     &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (spawnError != 0) {
    result.error = "Unable to start " + executable + ": " +
                   std::string(std::strerror(spawnError));
    return result;
  }
  writeEnd.reset();

  static_cast<void>(fcntl(readEnd.get(), F_SETFL,
                          fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK));
  bool terminationRequested = false;
  auto terminationAt = std::chrono::steady_clock::time_point{};
  int waitStatus = 0;
  bool exited = false;
  std::array<char, 8192> buffer{};
  while (!exited) {
    struct pollfd descriptor { readEnd.get(), POLLIN | POLLHUP, 0 };
    static_cast<void>(poll(&descriptor, 1, 100));
    for (;;) {
      const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
      if (count <= 0) {
        break;
      }
      const std::string chunk(buffer.data(), static_cast<std::size_t>(count));
      if (result.output.size() < 256U * 1024U) {
        const std::size_t remaining = 256U * 1024U - result.output.size();
        result.output.append(chunk.data(), std::min(remaining, chunk.size()));
      }
      if (onOutput) {
        onOutput(chunk);
      }
    }
    const pid_t waited = waitpid(child, &waitStatus, WNOHANG);
    exited = waited == child;
    if (!exited && isCancelled && isCancelled()) {
      if (!terminationRequested) {
        kill(-child, SIGTERM);
        terminationRequested = true;
        terminationAt = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - terminationAt >
                 std::chrono::seconds(3)) {
        kill(-child, SIGKILL);
      }
    }
  }
  for (;;) {
    const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
    if (count <= 0) {
      break;
    }
    result.output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  result.cancelled = terminationRequested;
  if (WIFEXITED(waitStatus)) {
    result.exitStatus = WEXITSTATUS(waitStatus);
  }
  result.success = !result.cancelled && result.exitStatus == 0;
  if (!result.success && result.error.empty()) {
    result.error = result.cancelled
                       ? "Operation cancelled"
                       : executable + " exited with status " +
                             std::to_string(result.exitStatus);
    if (!result.output.empty()) {
      const std::size_t offset =
          result.output.size() > 4096U ? result.output.size() - 4096U : 0U;
      result.error += ": " + result.output.substr(offset);
    }
  }
  return result;
}

std::string rawDevicePath(const std::string& devicePath) {
  constexpr const char prefix[] = "/dev/disk";
  if (devicePath.rfind(prefix, 0) != 0 || devicePath.size() <= 9U) {
    return {};
  }
  const std::string suffix = devicePath.substr(9U);
  if (!std::all_of(suffix.begin(), suffix.end(), [](const unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return {};
  }
  return "/dev/rdisk" + suffix;
}

bool writeAllAt(const int descriptor, const unsigned char* data,
                const std::size_t size, const std::uint64_t offset,
                std::string& error) {
  std::size_t completed = 0U;
  while (completed < size) {
    const ssize_t count =
        pwrite(descriptor, data + completed, size - completed,
               static_cast<off_t>(offset + completed));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      error = "Full overwrite failed: " + std::string(std::strerror(errno));
      return false;
    }
    completed += static_cast<std::size_t>(count);
  }
  return true;
}

bool readAllAt(const int descriptor, unsigned char* data,
               const std::size_t size, const std::uint64_t offset,
               std::string& error) {
  std::size_t completed = 0U;
  while (completed < size) {
    const ssize_t count =
        pread(descriptor, data + completed, size - completed,
              static_cast<off_t>(offset + completed));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      error = "Unable to read back the full overwrite: " +
              std::string(std::strerror(errno));
      return false;
    }
    completed += static_cast<std::size_t>(count);
  }
  return true;
}

core::RawWriteResult fullZeroAndVerify(
    const core::BlockDeviceInfo& target,
    const core::MacOsInstallerProgressCallback& onProgress,
    const core::MacOsInstallerCancelCallback& isCancelled) {
  core::RawWriteResult result;
  const auto unmount = runProcess("/usr/sbin/diskutil",
                                  {"unmountDisk", target.devicePath},
                                  isCancelled);
  if (!unmount.success) {
    result.cancelled = unmount.cancelled;
    result.error = "Unable to unmount the target before the full overwrite: " +
                   unmount.error;
    return result;
  }
  const auto afterUnmountDevices = discoverBlockDevices();
  const auto afterUnmount = std::find_if(
      afterUnmountDevices.devices.begin(), afterUnmountDevices.devices.end(),
      [&target](const auto& device) {
        return device.devicePath == target.devicePath &&
               device.stableId == target.stableId &&
               device.capacityBytes == target.capacityBytes &&
               device.logicalSectorSize == target.logicalSectorSize;
      });
  if (afterUnmount == afterUnmountDevices.devices.end()) {
    result.error =
        "The selected target identity changed after it was unmounted";
    return result;
  }
  const std::string rawPath = rawDevicePath(target.devicePath);
  FileDescriptor device(open(rawPath.c_str(), O_RDWR | O_EXCL | O_CLOEXEC));
  if (!device.valid()) {
    result.error = "Unable to open the target for the full overwrite: " +
                   std::string(std::strerror(errno));
    return result;
  }
  std::uint32_t sectorSize = 0;
  std::uint64_t blockCount = 0;
  std::uint32_t writable = 0;
  if (ioctl(device.get(), DKIOCGETBLOCKSIZE, &sectorSize) != 0 ||
      ioctl(device.get(), DKIOCGETBLOCKCOUNT, &blockCount) != 0 ||
      ioctl(device.get(), DKIOCISWRITABLE, &writable) != 0 || writable == 0U ||
      sectorSize == 0U || blockCount == 0U ||
      blockCount > std::numeric_limits<std::uint64_t>::max() / sectorSize ||
      blockCount * sectorSize != target.capacityBytes ||
      sectorSize != target.logicalSectorSize) {
    result.error = "The opened target geometry or writability no longer matches the selected device";
    return result;
  }
  static_cast<void>(fcntl(device.get(), F_NOCACHE, 1));
  constexpr std::size_t preferredBlock = 4U * 1024U * 1024U;
  const std::size_t blockSize =
      std::max<std::size_t>(sectorSize,
                            preferredBlock - (preferredBlock % sectorSize));
  std::vector<unsigned char> zeros(blockSize, 0U);
  std::vector<unsigned char> observed(blockSize, 0U);
  result.destructiveWriteStarted = true;
  for (std::uint64_t offset = 0; offset < target.capacityBytes;) {
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Full overwrite cancelled after the target was modified";
      return result;
    }
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(zeros.size(), target.capacityBytes - offset));
    if (!writeAllAt(device.get(), zeros.data(), count, offset, result.error)) {
      return result;
    }
    offset += count;
    result.bytesWritten = offset;
    report(onProgress, core::MacOsInstallerStage::Wiping, offset,
           target.capacityBytes);
  }
  if (fsync(device.get()) != 0 ||
      ioctl(device.get(), DKIOCSYNCHRONIZECACHE, nullptr) != 0) {
    result.error = "Unable to flush the full overwrite: " +
                   std::string(std::strerror(errno));
    return result;
  }
  for (std::uint64_t offset = 0; offset < target.capacityBytes;) {
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Full-overwrite verification cancelled";
      return result;
    }
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(observed.size(), target.capacityBytes - offset));
    if (!readAllAt(device.get(), observed.data(), count, offset,
                   result.error)) {
      return result;
    }
    if (!std::all_of(observed.begin(), observed.begin() + count,
                     [](const unsigned char value) { return value == 0U; })) {
      result.error = "Full-overwrite verification found non-zero data";
      return result;
    }
    offset += count;
    result.bytesVerified = offset;
    report(onProgress, core::MacOsInstallerStage::VerifyingWipe, offset,
           target.capacityBytes);
  }
  result.verificationCompleted = true;
  result.success = true;
  return result;
}

bool sameInstaller(const core::MacOsInstallerInfo& left,
                   const core::MacOsInstallerInfo& right) {
  return left.applicationPath == right.applicationPath &&
         left.createInstallMediaPath == right.createInstallMediaPath &&
         left.version == right.version && left.build == right.build &&
         left.payloadSizeBytes == right.payloadSizeBytes;
}

std::string safeVolumeName(const std::string& operationId) {
  std::string suffix;
  for (const unsigned char value : operationId) {
    if (std::isalnum(value) != 0) {
      suffix.push_back(static_cast<char>(std::toupper(value)));
      if (suffix.size() == 8U) {
        break;
      }
    }
  }
  return "RUFUSPP_INSTALLER_" + (suffix.empty() ? "TARGET" : suffix);
}

}  // namespace

core::MacOsInstallerAnalysisResult analyzeInstallerApplication(
    const std::filesystem::path& applicationPath) {
  core::MacOsInstallerAnalysisResult result;
  std::error_code fileError;
  const auto canonical = std::filesystem::canonical(applicationPath, fileError);
  if (fileError || !std::filesystem::is_directory(canonical, fileError) ||
      fileError || canonical.extension() != ".app") {
    result.error = "Select an existing macOS installer application bundle (.app)";
    return result;
  }
  const auto tool = canonical / "Contents" / "Resources" / "createinstallmedia";
  struct stat toolStatus {};
  if (!unchangedRegularFile(tool, toolStatus, result.error)) {
    return result;
  }
  if (!appleSignedBundle(canonical, result.error)) {
    return result;
  }

  CfObject<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(canonical.c_str()),
      static_cast<CFIndex>(canonical.string().size()), true));
  CfObject<CFBundleRef> bundle(url.get() == nullptr ? nullptr
                                                   : CFBundleCreate(
                                                         kCFAllocatorDefault,
                                                         url.get()));
  if (bundle.get() == nullptr) {
    result.error = "The selected application has no readable bundle metadata";
    return result;
  }
  core::MacOsInstallerInfo info;
  info.applicationPath = canonical.string();
  info.createInstallMediaPath = tool.string();
  info.displayName = infoString(bundle.get(), CFSTR("CFBundleDisplayName"));
  if (info.displayName.empty()) {
    info.displayName = infoString(bundle.get(), CFSTR("CFBundleName"));
  }
  if (info.displayName.empty()) {
    info.displayName = canonical.stem().string();
  }
  info.version =
      infoString(bundle.get(), CFSTR("CFBundleShortVersionString"));
  info.build = infoString(bundle.get(), kCFBundleVersionKey);
  info.payloadSizeBytes = payloadSize(canonical, result.warnings);
  if (info.payloadSizeBytes == 0U) {
    result.warnings.emplace_back(
        "Installer payload size is unknown; target capacity will be validated by createinstallmedia.");
  }
  result.installer = std::move(info);
  return result;
}

core::RawWriteResult createInstallerLocally(
    const core::MacOsInstallerInfo& selected,
    const core::BlockDeviceInfo& target, const bool fullWipe,
    const std::string& operationId,
    const core::MacOsInstallerProgressCallback& onProgress,
    const core::MacOsInstallerCancelCallback& isCancelled) {
  core::RawWriteResult result;
  report(onProgress, core::MacOsInstallerStage::Revalidating, 0U, 1U);
  const auto source = analyzeInstallerApplication(selected.applicationPath);
  if (!source.succeeded() || !sameInstaller(selected, *source.installer)) {
    result.error = source.succeeded()
                       ? "The macOS installer application changed after selection"
                       : source.error;
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Installer creation cancelled before the target was modified";
    return result;
  }
  const auto discovery = discoverBlockDevices();
  const auto observed = std::find_if(
      discovery.devices.begin(), discovery.devices.end(),
      [&target](const auto& device) {
        return device.devicePath == target.devicePath;
      });
  if (observed == discovery.devices.end()) {
    result.error = "The selected target is no longer connected";
    return result;
  }
  const core::SafetyPolicy policy;
  const auto identity = policy.validateIdentity(target, *observed);
  if (!identity.safe() ||
      core::evaluateDeviceEligibility(*observed) !=
          core::DeviceEligibility::Eligible) {
    result.error = !identity.safe()
                       ? identity.issues.front().message
                       : "The selected target is no longer an eligible removable device";
    return result;
  }
  if (target.capacityBytes < selected.minimumTargetBytes) {
    result.error = "The selected target is smaller than the 16 GiB minimum for a macOS installer";
    return result;
  }

  const auto help = runProcess(selected.createInstallMediaPath, {"--help"},
                               [] { return false; });
  if (help.cancelled || help.exitStatus < 0 ||
      help.output.find("--volume") == std::string::npos) {
    result.error =
        "The Apple installer tool does not expose the expected createinstallmedia interface";
    return result;
  }
  std::vector<std::string> installerArguments{"--volume"};
  const bool supportsNoInteraction =
      help.output.find("--nointeraction") != std::string::npos;
  if (!supportsNoInteraction) {
    result.error =
        "This legacy createinstallmedia tool requires interactive confirmation and is not supported safely";
    return result;
  }

  if (fullWipe) {
    result = fullZeroAndVerify(target, onProgress, isCancelled);
    if (!result.success) {
      return result;
    }
  }
  const std::uint64_t wipedBytes = result.bytesWritten;
  const std::uint64_t verifiedBytes = result.bytesVerified;
  const bool wipeVerified = result.verificationCompleted;

  const auto beforePrepareDevices = discoverBlockDevices();
  const auto beforePrepare = std::find_if(
      beforePrepareDevices.devices.begin(), beforePrepareDevices.devices.end(),
      [&target](const auto& device) {
        return device.devicePath == target.devicePath &&
               device.stableId == target.stableId &&
               device.capacityBytes == target.capacityBytes &&
               device.logicalSectorSize == target.logicalSectorSize &&
               core::evaluateDeviceEligibility(device) ==
                   core::DeviceEligibility::Eligible;
      });
  if (beforePrepare == beforePrepareDevices.devices.end()) {
    result.success = false;
    result.error =
        "The selected target identity or eligibility changed before installer preparation";
    return result;
  }

  const std::string volumeName = safeVolumeName(operationId);
  report(onProgress, core::MacOsInstallerStage::PreparingTarget, 0U, 1U,
         "Creating a temporary Mac OS Extended (Journaled) GPT volume");
  const auto prepare = runProcess(
      "/usr/sbin/diskutil",
      {"eraseDisk", "JHFS+", volumeName, "GPT", target.devicePath},
      isCancelled);
  result.destructiveWriteStarted = true;
  if (!prepare.success) {
    result.cancelled = prepare.cancelled;
    result.error = "Unable to prepare the target volume: " + prepare.error;
    return result;
  }

  const auto preparedDevices = discoverBlockDevices();
  const auto prepared = std::find_if(
      preparedDevices.devices.begin(), preparedDevices.devices.end(),
      [&target](const auto& device) {
        return device.devicePath == target.devicePath &&
               device.stableId == target.stableId &&
               device.capacityBytes == target.capacityBytes;
      });
  if (prepared == preparedDevices.devices.end() ||
      prepared->mountPoints.size() != 1U) {
    result.error = "The prepared target did not remount as one identifiable installer volume";
    return result;
  }
  const std::string volumePath = prepared->mountPoints.front();
  installerArguments.push_back(volumePath);
  if (help.output.find("--applicationpath") != std::string::npos) {
    installerArguments.push_back("--applicationpath");
    installerArguments.push_back(selected.applicationPath);
  }
  installerArguments.push_back("--nointeraction");
  std::string progressBuffer;
  std::uint64_t lastPercent = 0U;
  const std::regex percentage(R"(([0-9]{1,3})(?:\.[0-9]+)?%))");
  const auto tool = runProcess(
      selected.createInstallMediaPath, installerArguments, isCancelled,
      [&](const std::string& chunk) {
        progressBuffer += chunk;
        if (progressBuffer.size() > 16384U) {
          progressBuffer.erase(0U, progressBuffer.size() - 8192U);
        }
        for (std::sregex_iterator match(progressBuffer.begin(),
                                        progressBuffer.end(), percentage),
                                  end;
             match != end; ++match) {
          lastPercent = std::min<std::uint64_t>(
              100U, static_cast<std::uint64_t>(
                        std::stoul((*match)[1].str())));
        }
        report(onProgress, core::MacOsInstallerStage::CreatingInstaller,
               lastPercent, 100U);
      });
  result.bytesWritten = wipedBytes;
  result.bytesVerified = verifiedBytes;
  result.verificationCompleted = wipeVerified;
  if (!tool.success) {
    result.cancelled = tool.cancelled;
    result.error = "Apple createinstallmedia failed: " + tool.error;
    return result;
  }
  const auto finalSource =
      analyzeInstallerApplication(selected.applicationPath);
  if (!finalSource.succeeded() || !sameInstaller(selected, *finalSource.installer)) {
    result.error = "The macOS installer application changed while createinstallmedia was running";
    return result;
  }
  const auto finalDevices = discoverBlockDevices();
  const auto finalTarget = std::find_if(
      finalDevices.devices.begin(), finalDevices.devices.end(),
      [&target](const auto& device) {
        return device.devicePath == target.devicePath &&
               device.stableId == target.stableId &&
               device.capacityBytes == target.capacityBytes &&
               device.logicalSectorSize == target.logicalSectorSize;
      });
  if (finalTarget == finalDevices.devices.end()) {
    result.error =
        "createinstallmedia completed, but the original target identity could not be verified afterward";
    return result;
  }
  report(onProgress, core::MacOsInstallerStage::Complete, 1U, 1U);
  result.success = true;
  return result;
}

}  // namespace rufus::backend::macos
