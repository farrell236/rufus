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
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>

namespace rufus::core::detail {

struct Ext2FormatOptions final {
  std::uint64_t offsetBytes{};
  std::uint64_t sizeBytes{};
  std::string volumeLabel;
  bool createPersistenceConf{};
};

using Ext2CancelCallback = std::function<bool()>;

// Writes a self-contained ext2 filesystem at an offset in an already-open,
// newly-created sparse disk image. This deliberately avoids mounting the
// filesystem, which keeps persistence staging identical on every host OS.
[[nodiscard]] bool writeExt2Filesystem(std::ofstream& output,
                                       const Ext2FormatOptions& options,
                                       const Ext2CancelCallback& isCancelled,
                                       std::string& error);

[[nodiscard]] bool verifyExt2Filesystem(const std::filesystem::path& imagePath,
                                        const Ext2FormatOptions& options,
                                        std::string& error);

}  // namespace rufus::core::detail
