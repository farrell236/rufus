/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/platform.hpp"

namespace rufus::core {

std::string_view platformName(const Platform platform) noexcept {
  switch (platform) {
    case Platform::Windows:
      return "Windows";
    case Platform::Linux:
      return "Linux";
    case Platform::MacOS:
      return "macOS";
    case Platform::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

}  // namespace rufus::core
