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

#include <string_view>

namespace rufus::core {

enum class Platform {
  Windows,
  Linux,
  MacOS,
  Unknown,
};

[[nodiscard]] constexpr Platform currentPlatform() noexcept {
#if defined(_WIN32)
  return Platform::Windows;
#elif defined(__APPLE__)
  return Platform::MacOS;
#elif defined(__linux__)
  return Platform::Linux;
#else
  return Platform::Unknown;
#endif
}

[[nodiscard]] std::string_view platformName(Platform platform) noexcept;

}  // namespace rufus::core
