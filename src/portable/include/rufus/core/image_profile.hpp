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

struct ImageContentEntry final {
  std::string path;
  std::uint64_t sizeBytes{};
  bool directory{};
};

class ImageProfileResolver final {
 public:
  static void apply(const std::vector<ImageContentEntry>& contents, ImageInfo& image);
};

}  // namespace rufus::core
