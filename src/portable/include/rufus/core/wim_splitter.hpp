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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rufus::core {

struct WimSplitProgress final {
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
  unsigned int currentPart{};
  unsigned int totalParts{};
};

struct WimSplitResult final {
  bool success{};
  bool cancelled{};
  std::vector<std::filesystem::path> parts;
  std::string error;
};

using WimSplitProgressCallback = std::function<void(const WimSplitProgress&)>;
using WimSplitCancelCallback = std::function<bool()>;

// Splitting is isolated behind this interface so the ISO deployment engine
// remains testable without loading third-party code. The production backend
// resolves the cross-platform wimlib C API at runtime. Large solid-compressed
// install.esd inputs are converted to ordinary LZX resources before the split
// parts are written so Windows Setup can consume them from FAT32 media.
class WimSplitter {
 public:
  virtual ~WimSplitter() = default;

  [[nodiscard]] virtual bool available() const noexcept = 0;
  [[nodiscard]] virtual std::string availabilityReason() const = 0;
  [[nodiscard]] virtual WimSplitResult split(
      const std::filesystem::path& sourceWim,
      const std::filesystem::path& firstPartPath,
      std::uint64_t maximumPartBytes,
      const WimSplitProgressCallback& onProgress = {},
      const WimSplitCancelCallback& isCancelled = {}) const = 0;
};

// The returned object is always non-null. If a suitable wimlib shared library
// cannot be loaded, available() is false and availabilityReason() explains how
// the caller can make the capability available.
[[nodiscard]] std::shared_ptr<const WimSplitter> createSystemWimSplitter();

}  // namespace rufus::core
