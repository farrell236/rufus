/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/format.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace rufus::core {

std::string formatByteSize(const std::uint64_t bytes) {
  constexpr std::array<const char*, 6> suffixes{
      "bytes", "KiB", "MiB", "GiB", "TiB", "PiB"};

  auto value = static_cast<double>(bytes);
  std::size_t suffix = 0;
  while (value >= 1024.0 && suffix + 1 < suffixes.size()) {
    value /= 1024.0;
    ++suffix;
  }

  std::ostringstream stream;
  if (suffix == 0) {
    stream << bytes;
  } else {
    stream << std::fixed << std::setprecision(value < 10.0 ? 1 : 0) << value;
  }
  stream << ' ' << suffixes[suffix];
  return stream.str();
}

}  // namespace rufus::core
