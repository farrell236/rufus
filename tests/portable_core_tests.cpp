/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <cstdlib>
#include <iostream>
#include <string>

#include "rufus/core/application_info.hpp"
#include "rufus/core/format.hpp"
#include "rufus/core/macos_installer.hpp"
#include "rufus/core/platform.hpp"

namespace {

void expectEqual(const std::string& actual, const std::string& expected, const char* description) {
  if (actual == expected) {
    return;
  }

  std::cerr << description << ": expected '" << expected << "', got '" << actual << "'\n";
  std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
  using rufus::core::ApplicationInfo;
  using rufus::core::formatByteSize;
  using rufus::core::platformName;

  static_assert(ApplicationInfo::name == "Rufus++");
  static_assert(ApplicationInfo::displayName == "Rufus++");
  expectEqual(formatByteSize(0), "0 bytes", "zero bytes");
  expectEqual(formatByteSize(1023), "1023 bytes", "bytes below one KiB");
  expectEqual(formatByteSize(1024), "1.0 KiB", "one KiB");
  expectEqual(formatByteSize(10 * 1024), "10 KiB", "ten KiB");
  expectEqual(formatByteSize(1536), "1.5 KiB", "fractional KiB");
  expectEqual(rufus::core::macOsInstallerStageName(
                  rufus::core::MacOsInstallerStage::CreatingInstaller),
              "Creating macOS installer", "macOS installer stage name");
  if (rufus::core::currentPlatform() == rufus::core::Platform::Unknown ||
      platformName(rufus::core::currentPlatform()) == "Unknown") {
    std::cerr << "the configured host platform should be recognized\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
