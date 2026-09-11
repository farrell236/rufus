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

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "rufus/core/media.hpp"

namespace rufus::core::detail {

struct CompressedMeasurement final {
  std::array<unsigned char, 512> prefix{};
  std::size_t prefixBytes{};
  std::uint64_t expandedSizeBytes{};
  std::string error;
  bool success{};
};

// Fully decodes the source once. Besides resolving gzip's modulo-4-GiB size,
// this validates stream checksums before a physical target can be modified.
[[nodiscard]] CompressedMeasurement measureCompressedImage(
    const std::filesystem::path& path, ImageFormat format,
    const std::function<bool()>& isCancelled = {});

}  // namespace rufus::core::detail
