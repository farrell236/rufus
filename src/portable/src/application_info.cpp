/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/application_info.hpp"

namespace rufus::core {

static_assert(!ApplicationInfo::name.empty());
static_assert(!ApplicationInfo::displayName.empty());
static_assert(!ApplicationInfo::version.empty());
static_assert(!ApplicationInfo::organization.empty());

}  // namespace rufus::core
