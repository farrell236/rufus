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
#include <string>

#include "rufus/core/raw_image_writer.hpp"

namespace rufus::core {

enum class MediaCaptureFormat {
  Raw,
  FixedVhd,
  DynamicVhd,
  DynamicVhdx,
  Ffu,
  UdfIso,
};

enum class MediaCaptureStage {
  Capturing,
  Finalizing,
  Verifying,
  Complete,
};

struct MediaCaptureProgress final {
  MediaCaptureStage stage{MediaCaptureStage::Capturing};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct MediaCaptureOptions final {
  MediaCaptureFormat format{MediaCaptureFormat::Raw};
  std::size_t transferBytes{8U * 1024U * 1024U};
  bool verify{true};
};

struct MediaCaptureResult final {
  bool success{};
  bool cancelled{};
  std::uint64_t bytesCaptured{};
  std::uint64_t outputSizeBytes{};
  std::string error;
};

using MediaCaptureProgressCallback =
    std::function<void(const MediaCaptureProgress&)>;
using MediaCaptureCancelCallback = std::function<bool()>;

class MediaCaptureWriter final {
 public:
  // Raw, fixed/dynamic VHD, and dynamic VHDX are generated portably. FFU and
  // filesystem-aware UDF capture require a platform provider.
  [[nodiscard]] MediaCaptureResult capture(
      RawSourceIo& source, const std::filesystem::path& destination,
      const MediaCaptureOptions& options = {},
      const MediaCaptureProgressCallback& onProgress = {},
      const MediaCaptureCancelCallback& isCancelled = {}) const;
};

[[nodiscard]] const char* mediaCaptureFormatName(MediaCaptureFormat format) noexcept;
[[nodiscard]] const char* mediaCaptureStageName(MediaCaptureStage stage) noexcept;

}  // namespace rufus::core
