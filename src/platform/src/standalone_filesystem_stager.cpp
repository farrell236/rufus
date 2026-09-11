/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/backend/standalone_filesystem_stager.hpp"
#include "rufus/backend/ntfs_iso_image_stager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rufus::backend {
namespace {

constexpr std::uint64_t kSectorBytes = 512U;
constexpr std::uint64_t kPartitionStart = 2048U;
constexpr std::uint64_t kTailSectors = 2048U;

class ScopedStagedFile final {
 public:
  explicit ScopedStagedFile(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~ScopedStagedFile() {
    if (!keep_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }
  ScopedStagedFile(const ScopedStagedFile&) = delete;
  ScopedStagedFile& operator=(const ScopedStagedFile&) = delete;
  void keep() noexcept { keep_ = true; }

 private:
  std::filesystem::path path_;
  bool keep_{};
};

void put32(unsigned char* output, const std::uint32_t value) {
  for (unsigned int index = 0; index < 4U; ++index) {
    output[index] =
        static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

bool createMbrImage(const std::filesystem::path& path,
                    const core::BlockDeviceInfo& target,
                    const StandaloneFilesystem filesystem,
                    std::string& error) {
  if (filesystem == StandaloneFilesystem::UefiNtfs) {
    const auto layout = createNtfsIsoMbrImage(path, target.capacityBytes);
    error = layout.error;
    return layout.success;
  }
  if (target.logicalSectorSize != kSectorBytes ||
      target.capacityBytes % kSectorBytes != 0U) {
    error = "Host-backed standalone formatting requires a 512-byte-sector target";
    return false;
  }
  const std::uint64_t sectors = target.capacityBytes / kSectorBytes;
  if (sectors <= kPartitionStart + kTailSectors + 65536U ||
      sectors > std::numeric_limits<std::uint32_t>::max()) {
    error = "The target is outside the supported 32 MiB to 2 TiB MBR format range";
    return false;
  }
  std::array<unsigned char, kSectorBytes> mbr{};
  unsigned char* const partition = mbr.data() + 446U;
  partition[1] = 0xfeU;
  partition[2] = 0xffU;
  partition[3] = 0xffU;
  partition[4] = filesystem == StandaloneFilesystem::Ext3 ? 0x83U : 0x07U;
  partition[5] = 0xfeU;
  partition[6] = 0xffU;
  partition[7] = 0xffU;
  put32(partition + 8U, static_cast<std::uint32_t>(kPartitionStart));
  put32(partition + 12U,
        static_cast<std::uint32_t>(sectors - kPartitionStart - kTailSectors));
  mbr[510] = 0x55U;
  mbr[511] = 0xaaU;

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Unable to create the standalone filesystem staging image";
    return false;
  }
  output.write(reinterpret_cast<const char*>(mbr.data()),
               static_cast<std::streamsize>(mbr.size()));
  output.close();
  std::error_code fileError;
  std::filesystem::resize_file(path, target.capacityBytes, fileError);
  if (fileError) {
    error = "Unable to size the sparse filesystem staging image: " +
            fileError.message();
    return false;
  }
  return true;
}

bool verifyFilesystem(const std::filesystem::path& path,
                      const StandaloneFilesystem filesystem,
                      std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Unable to reopen the formatted staging image";
    return false;
  }
  std::array<unsigned char, 512> mbr{};
  input.read(reinterpret_cast<char*>(mbr.data()),
             static_cast<std::streamsize>(mbr.size()));
  if (!input || mbr[510] != 0x55U || mbr[511] != 0xaaU ||
      (mbr[450] != 0x07U && mbr[450] != 0x83U)) {
    error = "Standalone filesystem MBR verification failed";
    return false;
  }
  if (filesystem == StandaloneFilesystem::UefiNtfs &&
      mbr[462U + 4U] != 0xefU) {
    error = "The UEFI:NTFS helper partition is missing from the staged image";
    return false;
  }
  const std::uint64_t partitionOffset = kPartitionStart * kSectorBytes;
  if (filesystem == StandaloneFilesystem::Ext3) {
    std::array<unsigned char, 230> superblock{};
    input.seekg(static_cast<std::streamoff>(partitionOffset + 1024U));
    input.read(reinterpret_cast<char*>(superblock.data()),
               static_cast<std::streamsize>(superblock.size()));
    const std::uint32_t compatibleFeatures =
        static_cast<std::uint32_t>(superblock[92]) |
        static_cast<std::uint32_t>(superblock[93]) << 8U |
        static_cast<std::uint32_t>(superblock[94]) << 16U |
        static_cast<std::uint32_t>(superblock[95]) << 24U;
    if (!input || superblock[56] != 0x53U || superblock[57] != 0xefU ||
        (compatibleFeatures & 0x4U) == 0U) {
      error = "The ext3 provider did not create a journaled ext filesystem";
      return false;
    }
    return true;
  }

  std::array<unsigned char, 512> boot{};
  input.seekg(static_cast<std::streamoff>(partitionOffset));
  input.read(reinterpret_cast<char*>(boot.data()),
             static_cast<std::streamsize>(boot.size()));
  if (!input) {
    error = "Unable to read the staged filesystem boot record";
    return false;
  }
  const std::string_view oem(reinterpret_cast<const char*>(boot.data() + 3U),
                             8U);
  if ((filesystem == StandaloneFilesystem::Ntfs ||
       filesystem == StandaloneFilesystem::UefiNtfs) &&
      oem != "NTFS    ") {
    error = "The NTFS provider did not create an NTFS volume";
    return false;
  }
  if (filesystem == StandaloneFilesystem::ExFat && oem != "EXFAT   ") {
    error = "The exFAT provider did not create an exFAT volume";
    return false;
  }
  if (filesystem == StandaloneFilesystem::ReFs) {
    const std::string_view refs(reinterpret_cast<const char*>(boot.data() + 3U),
                                4U);
    if (refs != "ReFS") {
      error = "The ReFS provider did not create a ReFS volume";
      return false;
    }
    return true;
  }
  if (filesystem == StandaloneFilesystem::Udf) {
    std::vector<char> descriptors(512U * 1024U);
    input.clear();
    input.seekg(static_cast<std::streamoff>(partitionOffset));
    input.read(descriptors.data(),
               static_cast<std::streamsize>(descriptors.size()));
    const std::string_view bytes(descriptors.data(),
                                 static_cast<std::size_t>(input.gcount()));
    if (bytes.find("NSR02") == std::string_view::npos &&
        bytes.find("NSR03") == std::string_view::npos) {
      error = "The UDF provider did not create a recognized UDF descriptor set";
      return false;
    }
  }
  return true;
}

std::string normalizedLabel(std::string label) {
  label.erase(std::remove_if(label.begin(), label.end(), [](const char value) {
                return value == '\r' || value == '\n' || value == '"';
              }),
              label.end());
  return label.empty() ? "NO_LABEL" : label;
}

core::StandaloneMediaResult finishResult(
    const StandaloneFilesystem filesystem,
    const core::BlockDeviceInfo& target,
    const std::filesystem::path& outputPath, std::string volumeLabel,
    const core::StandaloneMediaProgressCallback& onProgress,
    const core::StandaloneMediaCancelCallback& isCancelled) {
  core::StandaloneMediaResult result;
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone filesystem formatting was cancelled";
    return result;
  }
  if (onProgress) {
    onProgress({core::StandaloneMediaStage::Verifying, 0U,
                target.capacityBytes});
  }
  if (!verifyFilesystem(outputPath, filesystem, result.error)) {
    return result;
  }
  core::ImageInfo image;
  image.path = outputPath.string();
  image.displayName = outputPath.filename().string();
  image.volumeLabel = std::move(volumeLabel);
  image.sizeBytes = target.capacityBytes;
  image.expandedSizeBytes = target.capacityBytes;
  image.format = core::ImageFormat::Raw;
  image.partitionScheme = core::PartitionScheme::Mbr;
  image.capabilities.rawWrite = true;
  image.capabilities.validPartitionTable = true;
  image.capabilities.uefiBootable =
      filesystem == StandaloneFilesystem::UefiNtfs;
  image.bootable = image.capabilities.uefiBootable;
  result.stagedImage = std::move(image);
  result.success = true;
  if (onProgress) {
    onProgress({core::StandaloneMediaStage::Complete, target.capacityBytes,
                target.capacityBytes});
  }
  return result;
}

#if !defined(_WIN32)

std::filesystem::path findExecutable(const std::string_view name) {
#if defined(__APPLE__)
  constexpr std::array<std::string_view, 8> directories{
      "/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin",
      "/usr/local/sbin", "/usr/bin", "/usr/sbin", "/bin", "/sbin"};
#else
  constexpr std::array<std::string_view, 6> directories{
      "/usr/local/bin", "/usr/local/sbin", "/usr/bin", "/usr/sbin",
      "/bin", "/sbin"};
#endif
  for (const auto directory : directories) {
    const auto candidate = std::filesystem::path(directory) / name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return {};
}

struct CommandResult final {
  int exitCode{-1};
  bool cancelled{};
};

CommandResult runCommand(
    const std::vector<std::string>& arguments,
    const core::StandaloneMediaCancelCallback& isCancelled = {}) {
  CommandResult result;
  if (arguments.empty()) {
    return result;
  }
  const auto executable = findExecutable(arguments.front());
  if (executable.empty()) {
    return result;
  }
  const pid_t child = fork();
  if (child == 0) {
    static_cast<void>(setpgid(0, 0));
    const int nullOutput = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nullOutput >= 0) {
      static_cast<void>(dup2(nullOutput, STDOUT_FILENO));
      static_cast<void>(dup2(nullOutput, STDERR_FILENO));
      close(nullOutput);
    }
    std::vector<char*> nativeArguments;
    nativeArguments.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) {
      nativeArguments.push_back(const_cast<char*>(argument.c_str()));
    }
    nativeArguments.push_back(nullptr);
    execv(executable.c_str(), nativeArguments.data());
    _exit(127);
  }
  if (child < 0) {
    return result;
  }
  static_cast<void>(setpgid(child, child));
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
      }
      return result;
    }
    if (waited < 0 && errno != EINTR) {
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      static_cast<void>(kill(-child, SIGTERM));
      static_cast<void>(kill(child, SIGTERM));
      for (unsigned int attempt = 0; attempt < 20U; ++attempt) {
        if (waitpid(child, &status, WNOHANG) == child) {
          return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      static_cast<void>(kill(-child, SIGKILL));
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

class PosixFilesystemStager final : public StandaloneFilesystemStager {
 public:
  [[nodiscard]] StandaloneFilesystemAvailability availability(
      const StandaloneFilesystem filesystem,
      const core::BlockDeviceInfo& target) const override {
    if (target.logicalSectorSize != kSectorBytes ||
        target.capacityBytes / kSectorBytes >
            std::numeric_limits<std::uint32_t>::max()) {
      return {false, "Host-backed formats currently require a 512-byte-sector target smaller than 2 TiB"};
    }
#if !defined(__APPLE__)
    if (geteuid() != 0) {
      return {false, "Linux loop-device formatting requires an administrator launch"};
    }
#endif
    std::vector<std::string_view> tools{
#if defined(__APPLE__)
        "hdiutil"
#else
        "losetup"
#endif
    };
    switch (filesystem) {
      case StandaloneFilesystem::Ntfs:
      case StandaloneFilesystem::UefiNtfs:
        tools.push_back("mkntfs");
        break;
      case StandaloneFilesystem::ExFat:
#if defined(__APPLE__)
        tools.push_back("newfs_exfat");
#else
        tools.push_back("mkfs.exfat");
#endif
        break;
      case StandaloneFilesystem::Udf:
#if defined(__APPLE__)
        tools.push_back("newfs_udf");
#else
        tools.push_back("mkudffs");
#endif
        break;
      case StandaloneFilesystem::Ext3:
#if defined(__APPLE__)
        tools.push_back("mke2fs");
#else
        tools.push_back("mkfs.ext3");
#endif
        break;
      case StandaloneFilesystem::ReFs:
        return {false, "ReFS formatting is available only from Windows"};
    }
    std::string missing;
    for (const auto tool : tools) {
      if (findExecutable(tool).empty()) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += tool;
      }
    }
    return missing.empty()
               ? StandaloneFilesystemAvailability{true, {}}
               : StandaloneFilesystemAvailability{
                     false, "Required host tools are missing: " + missing};
  }

  [[nodiscard]] core::StandaloneMediaResult stage(
      const StandaloneFilesystem filesystem,
      const core::BlockDeviceInfo& target,
      const std::filesystem::path& outputPath, std::string volumeLabel,
      const core::StandaloneFormatOptions options,
      const core::StandaloneMediaProgressCallback& onProgress,
      const core::StandaloneMediaCancelCallback& isCancelled) const override {
    core::StandaloneMediaResult result;
    const auto ready = availability(filesystem, target);
    if (!ready.available) {
      result.error = ready.reason;
      return result;
    }
    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) || fileError) {
      result.error = fileError ? "Unable to inspect the filesystem staging path: " +
                                     fileError.message()
                               : "The filesystem staging path already exists";
      return result;
    }
    ScopedStagedFile stagedFile(outputPath);
    if (onProgress) {
      onProgress({core::StandaloneMediaStage::Formatting, 0U,
                  target.capacityBytes});
    }
    if (!createMbrImage(outputPath, target, filesystem, result.error)) {
      return result;
    }
    const auto cleanupOutput = [&] {
      std::error_code ignored;
      std::filesystem::remove(outputPath, ignored);
    };

    std::string disk;
    std::string partition;
#if defined(__APPLE__)
    // hdiutil prints one line per synthesized device; the first whole-disk
    // node always precedes its slices for an MBR raw image.
    const auto attach = runCommandWithOutput(outputPath, isCancelled);
    const auto deviceStart = attach.output.find("/dev/disk");
    const auto deviceEnd = attach.output.find_first_of(" \t\r\n", deviceStart);
    if (deviceStart != std::string::npos) {
      disk = attach.output.substr(deviceStart, deviceEnd - deviceStart);
    }
    if (attach.cancelled || attach.exitCode != 0) {
      if (!disk.empty()) {
        static_cast<void>(runCommand({"hdiutil", "detach", disk}));
      }
      result.cancelled = attach.cancelled;
      result.error = "Unable to attach the private filesystem staging image" +
                     (attach.output.empty() ? std::string{}
                                            : ": " + attach.output);
      cleanupOutput();
      return result;
    }
    if (disk.empty()) {
      result.error = "hdiutil did not return a staging disk device";
      cleanupOutput();
      return result;
    }
    partition = "/dev/r" + disk.substr(5) + "s1";
#else
    const auto attach = runCommandWithOutput(outputPath, isCancelled);
    const auto deviceStart = attach.output.find("/dev/loop");
    const auto deviceEnd = attach.output.find_first_of(" \t\r\n", deviceStart);
    if (deviceStart != std::string::npos) {
      disk = attach.output.substr(deviceStart, deviceEnd - deviceStart);
    }
    if (attach.cancelled || attach.exitCode != 0 || disk.empty()) {
      if (!disk.empty() && disk.rfind("/dev/loop", 0) == 0) {
        static_cast<void>(runCommand({"losetup", "-d", disk}));
      }
      result.cancelled = attach.cancelled;
      result.error = "Unable to attach the private filesystem staging image";
      cleanupOutput();
      return result;
    }
    partition = disk + "p1";
    for (unsigned int attempt = 0;
         attempt < 50U && !std::filesystem::exists(partition); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
    const auto detach = [&] {
#if defined(__APPLE__)
      return runCommand({"hdiutil", "detach", disk}).exitCode == 0;
#else
      return runCommand({"losetup", "-d", disk}).exitCode == 0;
#endif
    };
    const std::string label = normalizedLabel(volumeLabel);
    std::vector<std::string> command;
    switch (filesystem) {
      case StandaloneFilesystem::Ntfs:
      case StandaloneFilesystem::UefiNtfs:
        if (options.clusterSizeBytes != 0U &&
            (options.clusterSizeBytes < target.logicalSectorSize ||
             options.clusterSizeBytes > 65536U ||
             (options.clusterSizeBytes & (options.clusterSizeBytes - 1U)) != 0U)) {
          result.error = "NTFS cluster size must be a power of two through 64 KiB";
          detach();
          cleanupOutput();
          return result;
        }
        command = {"mkntfs", "-F"};
        if (options.quickFormat) {
          command.push_back("-Q");
        }
        if (options.clusterSizeBytes != 0U) {
          command.insert(command.end(), {"-c", std::to_string(options.clusterSizeBytes)});
        }
        command.insert(command.end(), {"-L", label, partition});
        break;
      case StandaloneFilesystem::ExFat:
#if defined(__APPLE__)
        command = {"newfs_exfat", "-v", label, partition};
        if (options.clusterSizeBytes != 0U) {
          command.insert(command.end() - 1,
                         {"-b", std::to_string(options.clusterSizeBytes)});
        }
#else
        command = {"mkfs.exfat", "-n", label, partition};
        if (options.clusterSizeBytes != 0U) {
          command.insert(command.end() - 1,
                         {"-c", std::to_string(options.clusterSizeBytes)});
        }
#endif
        break;
      case StandaloneFilesystem::Udf:
        if (options.clusterSizeBytes != 0U) {
          result.error =
              "UDF uses its logical block size and does not expose allocation-unit selection";
          detach();
          cleanupOutput();
          return result;
        }
#if defined(__APPLE__)
        command = {"newfs_udf", "-b", "512", "-r", "2.01", "-v", label,
                   partition};
#else
        command = {"mkudffs", "--utf8", "--media-type=hd", "--blocksize=512",
                   "--udfrev=0x0201", "--vid=" + label, partition};
#endif
        break;
      case StandaloneFilesystem::Ext3:
#if defined(__APPLE__)
        command = {"mke2fs", "-t", "ext3", "-F", "-L", label, partition};
#else
        command = {"mkfs.ext3", "-F", "-L", label, partition};
#endif
        if (options.clusterSizeBytes != 0U) {
          if (options.clusterSizeBytes < 1024U ||
              options.clusterSizeBytes > 4096U ||
              (options.clusterSizeBytes & (options.clusterSizeBytes - 1U)) != 0U) {
            result.error = "ext3 block size must be 1, 2, or 4 KiB";
            detach();
            cleanupOutput();
            return result;
          }
          command.insert(command.end() - 1,
                         {"-b", std::to_string(options.clusterSizeBytes)});
        }
        break;
      case StandaloneFilesystem::ReFs:
        break;
    }
    const auto formatted = runCommand(command, isCancelled);
    const bool detached = detach();
    if (formatted.exitCode != 0 || !detached) {
      result.cancelled = formatted.cancelled;
      result.error = formatted.cancelled
                         ? "Standalone filesystem formatting was cancelled"
                         : !detached
                               ? "Unable to detach the formatted staging image"
                               : "The installed filesystem provider failed";
      cleanupOutput();
      return result;
    }
    result = finishResult(filesystem, target, outputPath, std::move(volumeLabel),
                          onProgress, isCancelled);
    if (!result.success) {
      cleanupOutput();
    } else {
      stagedFile.keep();
    }
    return result;
  }

 private:
  struct OutputCommandResult final {
    int exitCode{-1};
    bool cancelled{};
    std::string output;
  };

  static OutputCommandResult runCommandWithOutput(
      const std::filesystem::path& image,
      const core::StandaloneMediaCancelCallback& isCancelled) {
    OutputCommandResult result;
    std::vector<std::string> arguments{
#if defined(__APPLE__)
        "hdiutil", "attach", "-imagekey",
        "diskimage-class=CRawDiskImage", "-nomount", "-noverify",
        image.string()
#else
        "losetup", "--find", "--show", "--partscan", image.string()
#endif
    };
    const auto executable = findExecutable(arguments.front());
    int descriptors[2]{};
    if (executable.empty() || pipe(descriptors) != 0) {
      return result;
    }
    const pid_t child = fork();
    if (child == 0) {
      static_cast<void>(setpgid(0, 0));
      close(descriptors[0]);
      static_cast<void>(dup2(descriptors[1], STDOUT_FILENO));
      static_cast<void>(dup2(descriptors[1], STDERR_FILENO));
      close(descriptors[1]);
      std::vector<char*> native;
      for (auto& argument : arguments) {
        native.push_back(argument.data());
      }
      native.push_back(nullptr);
      execv(executable.c_str(), native.data());
      _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) {
      close(descriptors[0]);
      return result;
    }
    static_cast<void>(setpgid(child, child));
    const int flags = fcntl(descriptors[0], F_GETFL);
    if (flags < 0 ||
        fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0) {
      static_cast<void>(kill(-child, SIGKILL));
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(descriptors[0]);
      return result;
    }
    int status = 0;
    std::array<char, 512> buffer{};
    for (;;) {
      const ssize_t count = read(descriptors[0], buffer.data(), buffer.size());
      if (count > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
      }
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child) {
        while (true) {
          const ssize_t tail = read(descriptors[0], buffer.data(), buffer.size());
          if (tail <= 0) {
            break;
          }
          result.output.append(buffer.data(), static_cast<std::size_t>(tail));
        }
        if (WIFEXITED(status)) {
          result.exitCode = WEXITSTATUS(status);
        }
        break;
      }
      if (waited < 0 && errno != EINTR) {
        break;
      }
      if (isCancelled && isCancelled()) {
        result.cancelled = true;
        static_cast<void>(kill(-child, SIGKILL));
        static_cast<void>(kill(child, SIGKILL));
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    close(descriptors[0]);
    return result;
  }
};

#else

void putBigEndian32(unsigned char* output, const std::uint32_t value) {
  output[0] = static_cast<unsigned char>(value >> 24U);
  output[1] = static_cast<unsigned char>(value >> 16U);
  output[2] = static_cast<unsigned char>(value >> 8U);
  output[3] = static_cast<unsigned char>(value);
}

void putBigEndian64(unsigned char* output, const std::uint64_t value) {
  for (unsigned int index = 0; index < 8U; ++index) {
    output[index] = static_cast<unsigned char>(
        value >> ((7U - index) * 8U));
  }
}

bool appendVhdFooter(const std::filesystem::path& path,
                     const std::uint64_t capacity, std::string& error) {
  std::array<unsigned char, 512> footer{};
  constexpr std::string_view cookie = "conectix";
  std::copy(cookie.begin(), cookie.end(), footer.begin());
  putBigEndian32(footer.data() + 8U, 2U);
  putBigEndian32(footer.data() + 12U, 0x00010000U);
  putBigEndian64(footer.data() + 16U,
                 std::numeric_limits<std::uint64_t>::max());
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  putBigEndian32(footer.data() + 24U,
                 static_cast<std::uint32_t>(std::max<std::int64_t>(
                     0, now - 946684800LL)));
  std::copy_n("RfQt", 4U, footer.begin() + 28U);
  putBigEndian32(footer.data() + 32U, 0x00010000U);
  std::copy_n("Wi2k", 4U, footer.begin() + 36U);
  putBigEndian64(footer.data() + 40U, capacity);
  putBigEndian64(footer.data() + 48U, capacity);
  const std::uint64_t sectors = std::min<std::uint64_t>(
      capacity / 512U, 65535ULL * 16ULL * 255ULL);
  std::uint32_t sectorsPerTrack = 17U;
  std::uint32_t heads = static_cast<std::uint32_t>(
      (sectors / sectorsPerTrack + 1023U) / 1024U);
  heads = std::max<std::uint32_t>(heads, 4U);
  if (sectors >= static_cast<std::uint64_t>(heads) * 1024U * sectorsPerTrack ||
      heads > 16U) {
    heads = 16U;
    sectorsPerTrack = 31U;
  }
  if (sectors >= static_cast<std::uint64_t>(heads) * 1024U * sectorsPerTrack) {
    sectorsPerTrack = 63U;
  }
  const auto cylinders = static_cast<std::uint32_t>(
      sectors / (static_cast<std::uint64_t>(heads) * sectorsPerTrack));
  footer[56] = static_cast<unsigned char>(cylinders >> 8U);
  footer[57] = static_cast<unsigned char>(cylinders);
  footer[58] = static_cast<unsigned char>(heads);
  footer[59] = static_cast<unsigned char>(sectorsPerTrack);
  putBigEndian32(footer.data() + 60U, 2U);
  std::random_device random;
  for (std::size_t index = 68U; index < 84U; ++index) {
    footer[index] = static_cast<unsigned char>(random());
  }
  std::uint32_t checksum = 0U;
  for (const auto byte : footer) {
    checksum += byte;
  }
  putBigEndian32(footer.data() + 64U, ~checksum);
  std::ofstream output(path, std::ios::binary | std::ios::app);
  output.write(reinterpret_cast<const char*>(footer.data()),
               static_cast<std::streamsize>(footer.size()));
  if (!output) {
    error = "Unable to append the temporary fixed-VHD footer";
    return false;
  }
  return true;
}

std::filesystem::path systemTool(const wchar_t* name) {
  std::array<wchar_t, 32768> directory{};
  const UINT length = GetSystemDirectoryW(
      directory.data(), static_cast<UINT>(directory.size()));
  if (length == 0U ||
      static_cast<std::size_t>(length) >= directory.size()) {
    return {};
  }
  const auto path = std::filesystem::path(directory.data()) / name;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES ||
                 (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
             ? std::filesystem::path{}
             : path;
}

std::wstring toWide(const std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), output.data(), count) !=
      count) {
    return {};
  }
  return output;
}

int runDiskpart(const std::filesystem::path& script,
                const core::StandaloneMediaCancelCallback& isCancelled,
                bool& cancelled) {
  const auto executable = systemTool(L"diskpart.exe");
  const std::vector<std::wstring> arguments{
      L"diskpart.exe", L"/s", script.native()};
  std::vector<const wchar_t*> native;
  for (const auto& argument : arguments) {
    native.push_back(argument.c_str());
  }
  native.push_back(nullptr);
  const intptr_t spawned =
      _wspawnv(_P_NOWAIT, executable.c_str(), native.data());
  if (spawned == -1) {
    return -1;
  }
  const HANDLE process = reinterpret_cast<HANDLE>(spawned);
  for (;;) {
    const DWORD wait = WaitForSingleObject(process, 100U);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_TIMEOUT) {
      CloseHandle(process);
      return -1;
    }
    if (isCancelled && isCancelled()) {
      cancelled = true;
      static_cast<void>(TerminateProcess(process, ERROR_CANCELLED));
      static_cast<void>(WaitForSingleObject(process, INFINITE));
      break;
    }
  }
  DWORD code = ERROR_GEN_FAILURE;
  const bool gotCode = GetExitCodeProcess(process, &code) != FALSE;
  CloseHandle(process);
  return !cancelled && gotCode &&
                 code <= static_cast<DWORD>(std::numeric_limits<int>::max())
             ? static_cast<int>(code)
             : -1;
}

class WindowsFilesystemStager final : public StandaloneFilesystemStager {
 public:
  [[nodiscard]] StandaloneFilesystemAvailability availability(
      const StandaloneFilesystem filesystem,
      const core::BlockDeviceInfo& target) const override {
    if (filesystem == StandaloneFilesystem::Udf ||
        filesystem == StandaloneFilesystem::Ext3) {
      return {false, "This filesystem has no installed Windows staging provider"};
    }
    if (target.logicalSectorSize != kSectorBytes ||
        target.capacityBytes / kSectorBytes >
            std::numeric_limits<std::uint32_t>::max()) {
      return {false, "Host-backed formats currently require a 512-byte-sector target smaller than 2 TiB"};
    }
    if (!IsUserAnAdmin()) {
      return {false, "Windows filesystem staging requires an elevated application"};
    }
    if (systemTool(L"diskpart.exe").empty()) {
      return {false, "Windows diskpart.exe is unavailable"};
    }
    return {true, {}};
  }

  [[nodiscard]] core::StandaloneMediaResult stage(
      const StandaloneFilesystem filesystem,
      const core::BlockDeviceInfo& target,
      const std::filesystem::path& outputPath, std::string volumeLabel,
      const core::StandaloneFormatOptions options,
      const core::StandaloneMediaProgressCallback& onProgress,
      const core::StandaloneMediaCancelCallback& isCancelled) const override {
    core::StandaloneMediaResult result;
    const auto ready = availability(filesystem, target);
    if (!ready.available) {
      result.error = ready.reason;
      return result;
    }
    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) || fileError) {
      result.error = "The standalone filesystem staging path is unavailable";
      return result;
    }
    ScopedStagedFile stagedFile(outputPath);
    if (onProgress) {
      onProgress({core::StandaloneMediaStage::Formatting, 0U,
                  target.capacityBytes});
    }
    if (!createMbrImage(outputPath, target, filesystem, result.error) ||
        !appendVhdFooter(outputPath, target.capacityBytes, result.error)) {
      return result;
    }
    auto script = outputPath;
    script += L".diskpart.txt";
    auto detachScript = outputPath;
    detachScript += L".diskpart-detach.txt";
    const auto absolute = std::filesystem::absolute(outputPath, fileError);
    if (fileError) {
      result.error = "Unable to resolve the staging image path";
      return result;
    }
    const std::wstring format =
        (filesystem == StandaloneFilesystem::Ntfs ||
         filesystem == StandaloneFilesystem::UefiNtfs) ? L"ntfs"
        : filesystem == StandaloneFilesystem::ExFat ? L"exfat"
                                                    : L"refs";
    const std::wstring label = toWide(normalizedLabel(volumeLabel));
    if (options.clusterSizeBytes != 0U &&
        (options.clusterSizeBytes < target.logicalSectorSize ||
         options.clusterSizeBytes > 65536U ||
         (options.clusterSizeBytes & (options.clusterSizeBytes - 1U)) != 0U)) {
      result.error = "Cluster size must be a power of two through 64 KiB";
      return result;
    }
    {
      std::wofstream commands(script, std::ios::trunc);
      commands << L"select vdisk file=\"" << absolute.native() << L"\"\n"
               << L"attach vdisk\nselect partition 1\nformat fs=" << format
               << (options.quickFormat ? L" quick" : L"")
               << (options.clusterSizeBytes == 0U
                       ? std::wstring{}
                       : L" unit=" + std::to_wstring(options.clusterSizeBytes))
               << L" label=\"" << label << L"\"\n";
      if (!commands) {
        result.error = "Unable to create the DiskPart formatting script";
        return result;
      }
    }
    {
      std::wofstream commands(detachScript, std::ios::trunc);
      commands << L"select vdisk file=\"" << absolute.native() << L"\"\n"
               << L"detach vdisk\n";
      if (!commands) {
        std::filesystem::remove(script, fileError);
        result.error = "Unable to create the DiskPart cleanup script";
        return result;
      }
    }
    bool cancelled = false;
    const int exitCode = runDiskpart(script, isCancelled, cancelled);
    bool detachCancelled = false;
    const int detachExitCode =
        runDiskpart(detachScript, {}, detachCancelled);
    std::filesystem::remove(script, fileError);
    std::filesystem::remove(detachScript, fileError);
    if (detachExitCode != 0) {
      result.cancelled = cancelled;
      result.error =
          "Windows DiskPart could not detach the private staging image";
      return result;
    }
    if (exitCode != 0) {
      result.cancelled = cancelled;
      result.error = cancelled ? "Standalone filesystem formatting was cancelled"
                               : "Windows DiskPart could not format the staging image";
      return result;
    }
    std::filesystem::resize_file(outputPath, target.capacityBytes, fileError);
    if (fileError) {
      result.error = "Unable to remove the temporary VHD attachment footer";
      return result;
    }
    result = finishResult(filesystem, target, outputPath, std::move(volumeLabel),
                          onProgress, isCancelled);
    if (!result.success) {
      std::filesystem::remove(outputPath, fileError);
    } else {
      stagedFile.keep();
    }
    return result;
  }
};

#endif

}  // namespace

std::unique_ptr<StandaloneFilesystemStager>
makePlatformStandaloneFilesystemStager() {
#if defined(_WIN32)
  return std::make_unique<WindowsFilesystemStager>();
#else
  return std::make_unique<PosixFilesystemStager>();
#endif
}

const char* standaloneFilesystemName(
    const StandaloneFilesystem filesystem) noexcept {
  switch (filesystem) {
    case StandaloneFilesystem::Ntfs:
      return "NTFS";
    case StandaloneFilesystem::UefiNtfs:
      return "UEFI:NTFS";
    case StandaloneFilesystem::ExFat:
      return "exFAT";
    case StandaloneFilesystem::Udf:
      return "UDF";
    case StandaloneFilesystem::ReFs:
      return "ReFS";
    case StandaloneFilesystem::Ext3:
      return "ext3";
  }
  return "filesystem";
}

}  // namespace rufus::backend
