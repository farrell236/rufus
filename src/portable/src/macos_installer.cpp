/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/macos_installer.hpp"

namespace rufus::core {

const char* macOsInstallerStageName(const MacOsInstallerStage stage) noexcept {
  switch (stage) {
    case MacOsInstallerStage::Revalidating:
      return "Revalidating macOS installer";
    case MacOsInstallerStage::Wiping:
      return "Overwriting target";
    case MacOsInstallerStage::VerifyingWipe:
      return "Verifying target overwrite";
    case MacOsInstallerStage::PreparingTarget:
      return "Preparing installer volume";
    case MacOsInstallerStage::CreatingInstaller:
      return "Creating macOS installer";
    case MacOsInstallerStage::Complete:
      return "Complete";
  }
  return "Unknown";
}

}  // namespace rufus::core
