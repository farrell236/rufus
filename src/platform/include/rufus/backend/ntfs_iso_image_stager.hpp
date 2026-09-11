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

#include "rufus/core/iso_deployment.hpp"

namespace rufus::backend {

struct NtfsIsoAvailability final {
  bool available{};
  std::string reason;
};

class NtfsIsoImageStager {
 public:
  virtual ~NtfsIsoImageStager() = default;

  [[nodiscard]] virtual NtfsIsoAvailability availability() const = 0;
  [[nodiscard]] virtual core::IsoDeploymentResult stage(
      const core::IsoDeploymentPlan& plan,
      const std::filesystem::path& outputPath,
      const core::IsoDeploymentProgressCallback& onProgress = {},
      const core::IsoDeploymentCancelCallback& isCancelled = {}) const = 0;
};

[[nodiscard]] std::unique_ptr<NtfsIsoImageStager>
makePlatformNtfsIsoImageStager();

struct NtfsIsoLayoutResult final {
  bool success{};
  std::string error;
};

// Creates the sparse MBR skeleton and installs the data-only UEFI:NTFS
// partition. It does not format or mount the NTFS data partition.
[[nodiscard]] NtfsIsoLayoutResult createNtfsIsoMbrImage(
    const std::filesystem::path& outputPath, std::uint64_t capacityBytes);

// Creates an MBR or GPT NTFS data-partition skeleton with the embedded
// UEFI:NTFS helper partition. It does not format or mount the NTFS partition.
[[nodiscard]] NtfsIsoLayoutResult createNtfsIsoDiskImage(
    const std::filesystem::path& outputPath, std::uint64_t capacityBytes,
    core::PartitionScheme partitionScheme);

}  // namespace rufus::backend
