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
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "rufus/core/image_profile.hpp"

namespace rufus::core::detail {

struct ImageFileExtent final {
  std::uint64_t imageOffset{};
  std::uint64_t fileOffset{};
  std::uint64_t length{};
};

struct ImageFileRecord final {
  std::string path;
  std::uint64_t sizeBytes{};
  std::vector<ImageFileExtent> extents;
  // UDF permits small file bodies to live directly in the file entry. ISO-9660
  // records leave this empty and use extents exclusively.
  std::vector<unsigned char> embeddedData;
  // ISO deployment transformations can replace an optical entry with a
  // private, regular staging file (for example install.swm). Readers outside
  // the deployment path leave this empty.
  std::filesystem::path externalPath;
};

struct Iso9660ReadResult final {
  std::vector<ImageContentEntry> entries;
  std::vector<ImageFileRecord> files;
  std::vector<std::string> warnings;
  std::string volumeLabel;
  bool valid{};
  bool joliet{};
  bool bootCatalogPresent{};
  bool bootCatalogValid{};
  bool biosBootable{};
  bool uefiBootable{};
};

[[nodiscard]] Iso9660ReadResult readIso9660Contents(const std::filesystem::path& path);
[[nodiscard]] bool readImageFile(const std::filesystem::path& imagePath,
                                 const ImageFileRecord& file,
                                 std::uint64_t offset, unsigned char* data,
                                 std::size_t size, std::string& error);
[[nodiscard]] bool readImageFile(std::ifstream& image,
                                 const ImageFileRecord& file,
                                 std::uint64_t offset, unsigned char* data,
                                 std::size_t size, std::string& error);

}  // namespace rufus::core::detail
