/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/file_image_writer.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>

namespace rufus::core {

namespace {

constexpr std::size_t kMinimumBlockSize = 4096;
constexpr std::size_t kMaximumBlockSize = 64U * 1024U * 1024U;

struct PartialFile final {
  std::filesystem::path path;
  bool committed{};

  ~PartialFile() {
    if (!committed) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};

bool isRawDevicePath(const std::filesystem::path& path) {
  const std::string nativePath = path.string();
  if (nativePath.rfind("\\\\.\\", 0) == 0 || nativePath.rfind("/dev/", 0) == 0) {
    return true;
  }

  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  return !error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status);
}

void report(const FileWriteProgressCallback& callback, const FileWriteStage stage,
            const std::uint64_t processed, const std::uint64_t total) {
  if (callback) {
    callback({stage, processed, total});
  }
}

bool cancelled(const FileWriteCancelCallback& callback) {
  return callback && callback();
}

std::filesystem::path partialPathFor(const std::filesystem::path& destination) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return destination.string() + ".rufus-plus-plus-partial-" + std::to_string(nonce);
}

bool streamsMatch(std::ifstream& source, std::ifstream& written, std::vector<char>& sourceBuffer,
                  std::vector<char>& writtenBuffer, const std::uint64_t total,
                  const FileWriteProgressCallback& onProgress,
                  const FileWriteCancelCallback& isCancelled, bool& wasCancelled) {
  std::uint64_t verified = 0;
  while (source) {
    if (cancelled(isCancelled)) {
      wasCancelled = true;
      return false;
    }

    source.read(sourceBuffer.data(), static_cast<std::streamsize>(sourceBuffer.size()));
    const auto count = source.gcount();
    if (count <= 0) {
      break;
    }
    written.read(writtenBuffer.data(), count);
    if (written.gcount() != count ||
        !std::equal(sourceBuffer.begin(), sourceBuffer.begin() + count, writtenBuffer.begin())) {
      return false;
    }
    verified += static_cast<std::uint64_t>(count);
    report(onProgress, FileWriteStage::Verifying, verified, total);
  }
  return verified == total;
}

}  // namespace

FileWriteResult FileImageWriter::write(const ImageInfo& image,
                                       const std::filesystem::path& destination,
                                       const FileWriteOptions& options,
                                       const FileWriteProgressCallback& onProgress,
                                       const FileWriteCancelCallback& isCancelled) const {
  FileWriteResult result;
  report(onProgress, FileWriteStage::Validating, 0, image.sizeBytes);

  if (image.path.empty() || image.sizeBytes == 0) {
    result.error = "Source image metadata is incomplete";
    return result;
  }
  if (options.blockSize < kMinimumBlockSize || options.blockSize > kMaximumBlockSize) {
    result.error = "Block size must be between 4 KiB and 64 MiB";
    return result;
  }
  if (destination.empty() || isRawDevicePath(destination)) {
    result.error = "FileImageWriter refuses raw devices and non-regular destinations";
    return result;
  }

  std::error_code error;
  if (std::filesystem::exists(destination, error) || error) {
    result.error = error ? "Unable to inspect destination: " + error.message()
                         : "Destination already exists";
    return result;
  }

  const std::filesystem::path sourcePath = std::filesystem::u8path(image.path);
  const auto actualSize = std::filesystem::file_size(sourcePath, error);
  if (error || actualSize != image.sizeBytes) {
    result.error = error ? "Unable to inspect source: " + error.message()
                         : "Source image size changed after analysis";
    return result;
  }

  const auto parent = destination.has_parent_path() ? destination.parent_path()
                                                    : std::filesystem::current_path(error);
  if (error || !std::filesystem::is_directory(parent, error) || error) {
    result.error = "Destination directory is not available";
    return result;
  }

  std::ifstream source(sourcePath, std::ios::binary);
  if (!source) {
    result.error = "Unable to open source image";
    return result;
  }

  PartialFile partial{partialPathFor(destination)};
  std::ofstream output(partial.path, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create temporary destination file";
    return result;
  }

  std::vector<char> buffer(options.blockSize);
  while (source) {
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      result.error = "Write cancelled";
      return result;
    }

    source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = source.gcount();
    if (count <= 0) {
      break;
    }
    output.write(buffer.data(), count);
    if (!output) {
      result.error = "Destination write failed";
      return result;
    }
    result.bytesWritten += static_cast<std::uint64_t>(count);
    report(onProgress, FileWriteStage::Writing, result.bytesWritten, image.sizeBytes);
  }

  if (!source.eof() || result.bytesWritten != image.sizeBytes) {
    result.error = "Source read failed or changed while writing";
    return result;
  }

  report(onProgress, FileWriteStage::Flushing, result.bytesWritten, image.sizeBytes);
  output.flush();
  if (!output) {
    result.error = "Unable to flush destination file";
    return result;
  }
  output.close();

  if (options.verify) {
    source.clear();
    source.seekg(0, std::ios::beg);
    std::ifstream written(partial.path, std::ios::binary);
    std::vector<char> verificationBuffer(options.blockSize);
    bool wasCancelled = false;
    if (!written || !streamsMatch(source, written, buffer, verificationBuffer, image.sizeBytes,
                                  onProgress, isCancelled, wasCancelled)) {
      result.cancelled = wasCancelled;
      result.error = wasCancelled ? "Verification cancelled" : "Destination verification failed";
      return result;
    }
  }

  std::filesystem::rename(partial.path, destination, error);
  if (error) {
    result.error = "Unable to commit destination file: " + error.message();
    return result;
  }
  partial.committed = true;
  result.success = true;
  report(onProgress, FileWriteStage::Complete, image.sizeBytes, image.sizeBytes);
  return result;
}

}  // namespace rufus::core
