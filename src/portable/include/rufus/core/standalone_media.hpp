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

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rufus/core/media.hpp"

namespace rufus::core {

enum class StandaloneMediaStage {
  Planning,
  Formatting,
  Verifying,
  Complete,
};

struct StandaloneMediaProgress final {
  StandaloneMediaStage stage{StandaloneMediaStage::Planning};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct StandaloneMediaResult final {
  bool success{};
  bool cancelled{};
  std::optional<ImageInfo> stagedImage;
  std::string error;
};

enum class StandaloneBootMode {
  None,
  FreeDos,
  MsDos,
  Grub2,
  Grub4Dos,
  ReactOs,
  Syslinux,
  Uefi,
};

struct StandaloneMediaFile final {
  std::string path;
  std::vector<unsigned char> data;
};

struct StandaloneFormatOptions final {
  // Zero asks the formatter to choose a compatible allocation unit.
  std::uint32_t clusterSizeBytes{};
  // Full format expands the staged image to the complete target so the guarded
  // raw writer overwrites and verifies every otherwise-unused sector.
  bool quickFormat{true};
};

using StandaloneMediaProgressCallback =
    std::function<void(const StandaloneMediaProgress&)>;
using StandaloneMediaCancelCallback = std::function<bool()>;

// Creates the sector-aligned prefix of a complete MBR/FAT32 disk. The FAT32
// partition advertises the full target capacity, while the compact staging
// image contains only initialized filesystem metadata. The guarded raw writer
// clears stale end-of-disk metadata when committing the prefix.
[[nodiscard]] StandaloneMediaResult stageBlankFat32Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel,
    const StandaloneMediaProgressCallback& onProgress = {},
    const StandaloneMediaCancelCallback& isCancelled = {},
    StandaloneFormatOptions options = {});

// Creates a quick-format MBR/FAT16 prefix for removable media from 16 MiB to
// 2 GiB. The volume is intentionally non-bootable.
[[nodiscard]] StandaloneMediaResult stageBlankFat16Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel,
    const StandaloneMediaProgressCallback& onProgress = {},
    const StandaloneMediaCancelCallback& isCancelled = {},
    StandaloneFormatOptions options = {});

// Creates an MBR disk with one Linux type-0x83 ext2 partition using the
// bundled host-independent formatter. The sparse staging file represents the
// full target, so the guarded raw writer can commit and verify every byte.
[[nodiscard]] StandaloneMediaResult stageBlankExt2Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel,
    const StandaloneMediaProgressCallback& onProgress = {},
    const StandaloneMediaCancelCallback& isCancelled = {},
    StandaloneFormatOptions options = {});

[[nodiscard]] StandaloneMediaResult stageFat32Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel, StandaloneBootMode bootMode,
    std::vector<StandaloneMediaFile> files,
    const StandaloneMediaProgressCallback& onProgress = {},
    const StandaloneMediaCancelCallback& isCancelled = {},
    StandaloneFormatOptions options = {});

[[nodiscard]] const char* standaloneMediaStageName(
    StandaloneMediaStage stage) noexcept;

}  // namespace rufus::core
