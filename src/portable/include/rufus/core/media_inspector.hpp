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
#include <string>
#include <vector>

#include "rufus/core/media.hpp"

namespace rufus::core {

struct InspectedPartition final {
  unsigned int index{};
  std::uint64_t firstLba{};
  std::uint64_t lastLba{};
  std::string type;
  std::string fileSystem;
  bool active{};
  bool efiSystem{};
};

struct MediaInspectionResult final {
  bool success{};
  bool protectiveMbr{};
  bool primaryTableValid{};
  bool backupTableValid{};
  bool bootable{};
  PartitionScheme partitionScheme{PartitionScheme::Unknown};
  std::vector<InspectedPartition> partitions;
  std::vector<std::string> findings;
  std::vector<std::string> warnings;
  std::string error;

  [[nodiscard]] std::string toText(const BlockDeviceInfo& device) const;
};

// Inspects bounded, read-only samples. `head` begins at LBA 0 and should
// normally cover at least the first 2 MiB. `tail` ends at the reported device
// capacity and is used only for backup-GPT validation.
[[nodiscard]] MediaInspectionResult inspectMediaSamples(
    const BlockDeviceInfo& device, const std::vector<unsigned char>& head,
    const std::vector<unsigned char>& tail = {});

}  // namespace rufus::core
