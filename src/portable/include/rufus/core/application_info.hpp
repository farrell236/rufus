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

#ifndef RUFUSPP_VERSION
#define RUFUSPP_VERSION "development"
#endif

struct ApplicationInfo final {
  static constexpr std::string_view name{"Rufus++"};
  static constexpr std::string_view displayName{"Rufus++"};
  static constexpr std::string_view version{RUFUSPP_VERSION};
  static constexpr std::string_view organization{"Rufus++ Contributors"};
};

}  // namespace rufus::core
