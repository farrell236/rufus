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
#include <memory>
#include <optional>
#include <string>

#include "rufus/core/media.hpp"
#include "rufus/core/windows_to_go.hpp"

namespace rufus::backend {

struct WindowsToGoAvailability final {
  bool available{};
  std::string reason;
};

enum class WindowsToGoStage {
  ExtractingInstallImage,
  CreatingDiskLayout,
  FormattingFilesystems,
  ApplyingWindows,
  ConfiguringBoot,
  Finalizing,
  Complete,
};

struct WindowsToGoProgress final {
  WindowsToGoStage stage{WindowsToGoStage::ExtractingInstallImage};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
  std::string detail;
};

struct WindowsToGoStageResult final {
  bool success{};
  bool cancelled{};
  std::optional<core::ImageInfo> stagedImage;
  std::string error;
};

using WindowsToGoProgressCallback =
    std::function<void(const WindowsToGoProgress&)>;
using WindowsToGoCancelCallback = std::function<bool()>;

class WindowsToGoImageStager {
 public:
  virtual ~WindowsToGoImageStager() = default;

  [[nodiscard]] virtual WindowsToGoAvailability availability() const = 0;
  [[nodiscard]] virtual WindowsToGoStageResult stage(
      const core::WindowsToGoPlan& plan,
      const std::filesystem::path& outputPath,
      const WindowsToGoProgressCallback& onProgress = {},
      const WindowsToGoCancelCallback& isCancelled = {}) const = 0;
};

[[nodiscard]] std::unique_ptr<WindowsToGoImageStager>
makePlatformWindowsToGoImageStager();

[[nodiscard]] const char* windowsToGoStageName(WindowsToGoStage stage) noexcept;

struct WindowsToGoLayoutResult final {
  bool success{};
  std::string error;
};

// Creates only the sparse GPT partition skeleton used by the stager. This is
// public so the layout and backup GPT can be validated without formatting or
// attaching any device.
[[nodiscard]] WindowsToGoLayoutResult createWindowsToGoGptImage(
    const std::filesystem::path& outputPath, std::uint64_t capacityBytes);

}  // namespace rufus::backend
