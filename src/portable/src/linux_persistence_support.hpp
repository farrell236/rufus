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

#include <string>
#include <string_view>
#include <unordered_map>

namespace rufus::core::detail {

[[nodiscard]] std::string normalizedPersistencePath(std::string path);
[[nodiscard]] bool isLinuxBootConfigurationPath(std::string_view path);
[[nodiscard]] bool isMd5ManifestPath(std::string_view path);
[[nodiscard]] std::string md5Hex(std::string_view contents);
[[nodiscard]] bool updateMd5Manifest(
    std::string& manifest,
    const std::unordered_map<std::string, std::string>& replacements);

}  // namespace rufus::core::detail
