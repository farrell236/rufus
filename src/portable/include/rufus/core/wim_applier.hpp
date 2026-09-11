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

namespace rufus::core {

struct WimApplyProgress final {
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct WimApplyResult final {
  bool success{};
  bool cancelled{};
  std::string error;
};

using WimApplyProgressCallback = std::function<void(const WimApplyProgress&)>;
using WimApplyCancelCallback = std::function<bool()>;

class WimApplicator {
 public:
  virtual ~WimApplicator() = default;

  [[nodiscard]] virtual bool available() const noexcept = 0;
  [[nodiscard]] virtual std::string availabilityReason() const = 0;

  // On POSIX hosts, directNtfsVolume asks wimlib's libntfs-3g backend to
  // apply directly to an unmounted NTFS filesystem image or block device.
  // On Windows, targetPath may instead be a mounted NTFS directory.
  [[nodiscard]] virtual WimApplyResult apply(
      const std::filesystem::path& sourceWim, std::uint32_t editionIndex,
      const std::filesystem::path& targetPath, bool directNtfsVolume,
      const std::filesystem::path& unattendedFile,
      const WimApplyProgressCallback& onProgress = {},
      const WimApplyCancelCallback& isCancelled = {}) const = 0;
};

[[nodiscard]] std::shared_ptr<const WimApplicator> createSystemWimApplicator();

}  // namespace rufus::core
