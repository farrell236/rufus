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
#include <string>
#include <vector>

#include "iso9660_reader.hpp"

namespace rufus::core::detail {

struct UdfReadResult final {
  std::vector<ImageContentEntry> entries;
  std::vector<ImageFileRecord> files;
  std::vector<std::string> warnings;
  std::string volumeLabel;
  bool valid{};
};

[[nodiscard]] UdfReadResult readUdfContents(const std::filesystem::path& path);

}  // namespace rufus::core::detail
