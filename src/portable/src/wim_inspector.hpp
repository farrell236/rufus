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
#include <string>
#include <vector>

#include "iso9660_reader.hpp"
#include "rufus/core/media.hpp"

namespace rufus::core::detail {

struct WimInspectionResult final {
  std::vector<std::string> warnings;
  std::vector<WindowsEditionInfo> editions;
  std::uint32_t imageCount{};
  std::uint32_t bootIndex{};
  std::uint32_t versionMajor{};
  std::uint32_t versionMinor{};
  std::uint32_t build{};
  ImageArchitecture architecture{ImageArchitecture::Unknown};
  bool found{};
  bool valid{};
};

[[nodiscard]] WimInspectionResult inspectWindowsImage(
    const std::filesystem::path& imagePath,
    const std::vector<ImageFileRecord>& files);

}  // namespace rufus::core::detail
