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

#include <cstdint>
#include <string>
#include <string_view>

namespace rufus::core {

enum class LinuxPersistenceStyle {
  None,
  Casper,
  DebianLive,
};

struct LinuxPersistenceOptions final {
  // A zero-sized request disables persistence. Non-zero requests are aligned
  // to one MiB and must be at least 256 MiB.
  std::uint64_t sizeBytes{};
};

struct LinuxPersistencePatchResult final {
  std::string contents;
  bool recognizedConfiguration{};
  bool modified{};
};

// Applies the same Casper/Debian kernel-argument conventions used by Rufus.
// The operation is idempotent and only changes recognized GRUB/Syslinux files.
[[nodiscard]] LinuxPersistencePatchResult patchLinuxPersistenceBootConfiguration(
    std::string_view path, std::string_view contents,
    LinuxPersistenceStyle style);

[[nodiscard]] const char* linuxPersistenceStyleName(
    LinuxPersistenceStyle style) noexcept;

}  // namespace rufus::core
