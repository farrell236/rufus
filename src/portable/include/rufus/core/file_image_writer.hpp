/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "rufus/core/media.hpp"

namespace rufus::core {

enum class FileWriteStage {
  Validating,
  Writing,
  Flushing,
  Verifying,
  Complete,
};

struct FileWriteProgress final {
  FileWriteStage stage{FileWriteStage::Validating};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct FileWriteOptions final {
  std::size_t blockSize{1024U * 1024U};
  bool verify{true};
};

struct FileWriteResult final {
  bool success{};
  bool cancelled{};
  std::uint64_t bytesWritten{};
  std::string error;
};

using FileWriteProgressCallback = std::function<void(const FileWriteProgress&)>;
using FileWriteCancelCallback = std::function<bool()>;

class FileImageWriter final {
 public:
  [[nodiscard]] FileWriteResult write(
      const ImageInfo& image,
      const std::filesystem::path& destination,
      const FileWriteOptions& options = {},
      const FileWriteProgressCallback& onProgress = {},
      const FileWriteCancelCallback& isCancelled = {}) const;
};

}  // namespace rufus::core
