/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/backend/block_device_backend.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace rufus::backend {

core::MediaInspectionResult BlockDeviceBackend::inspectReadOnly(
    const core::BlockDeviceInfo& device) const {
  core::MediaInspectionResult result;
  if (device.devicePath.empty() || device.stableId.empty() ||
      !device.wholeDevice || device.capacityBytes == 0U) {
    result.error =
        "Read-only inspection requires an identified whole physical device";
    return result;
  }
  constexpr std::uint64_t kHeadBytes = 2ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t kTailBytes = 1024ULL * 1024ULL;
  const auto headSize = static_cast<std::size_t>(
      std::min<std::uint64_t>(kHeadBytes, device.capacityBytes));
  const auto tailSize = static_cast<std::size_t>(
      std::min<std::uint64_t>(kTailBytes, device.capacityBytes));
  if (device.capacityBytes >
      static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    result.error = "The device is too large for this host's read-only stream API";
    return result;
  }

  std::ifstream input(std::filesystem::u8path(device.devicePath),
                      std::ios::binary);
  if (!input) {
    result.error =
        "Unable to open the selected device for read-only inspection; additional read permission may be required";
    return result;
  }
  std::vector<unsigned char> head(headSize);
  input.read(reinterpret_cast<char*>(head.data()),
             static_cast<std::streamsize>(head.size()));
  if (input.gcount() != static_cast<std::streamsize>(head.size())) {
    result.error = "Unable to read the complete bounded device-header sample";
    return result;
  }

  std::vector<unsigned char> tail(tailSize);
  input.clear();
  input.seekg(static_cast<std::streamoff>(device.capacityBytes - tailSize));
  input.read(reinterpret_cast<char*>(tail.data()),
             static_cast<std::streamsize>(tail.size()));
  if (input.gcount() != static_cast<std::streamsize>(tail.size())) {
    result.error = "Unable to read the complete bounded device-tail sample";
    return result;
  }
  return core::inspectMediaSamples(device, head, tail);
}

}  // namespace rufus::backend
