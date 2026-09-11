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

#include <filesystem>
#include <memory>
#include <string>

#include "rufus/core/standalone_media.hpp"

namespace rufus::backend {

enum class StandaloneFilesystem {
  Ntfs,
  UefiNtfs,
  ExFat,
  Udf,
  ReFs,
  Ext3,
};

struct StandaloneFilesystemAvailability final {
  bool available{};
  std::string reason;
};

class StandaloneFilesystemStager {
 public:
  virtual ~StandaloneFilesystemStager() = default;

  [[nodiscard]] virtual StandaloneFilesystemAvailability availability(
      StandaloneFilesystem filesystem,
      const core::BlockDeviceInfo& target) const = 0;

  // Formats a private sparse disk image rather than the physical device. The
  // resulting MBR image is then committed by the normal guarded raw writer.
  [[nodiscard]] virtual core::StandaloneMediaResult stage(
      StandaloneFilesystem filesystem, const core::BlockDeviceInfo& target,
      const std::filesystem::path& outputPath, std::string volumeLabel,
      core::StandaloneFormatOptions options = {},
      const core::StandaloneMediaProgressCallback& onProgress = {},
      const core::StandaloneMediaCancelCallback& isCancelled = {}) const = 0;
};

[[nodiscard]] std::unique_ptr<StandaloneFilesystemStager>
makePlatformStandaloneFilesystemStager();

[[nodiscard]] const char* standaloneFilesystemName(
    StandaloneFilesystem filesystem) noexcept;

}  // namespace rufus::backend
