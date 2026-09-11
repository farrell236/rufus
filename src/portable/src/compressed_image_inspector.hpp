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

#include "rufus/core/media.hpp"

namespace rufus::core::detail {

struct CompressedInspectionResult final {
  std::vector<std::string> warnings;
  std::uint64_t expandedSizeBytes{};
  bool valid{};
  bool sizeKnown{};
};

[[nodiscard]] CompressedInspectionResult inspectCompressedImage(
    const std::filesystem::path& path, ImageFormat format);

}  // namespace rufus::core::detail
