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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rufus/core/media.hpp"
#include "rufus/core/safety_policy.hpp"

namespace rufus::core {

// These options deliberately contain no password. Rufus++-style local accounts
// use a blank initial password and require the user to choose one at first
// sign-in, which also keeps credentials out of the ISO staging area and logs.
struct WindowsUserExperienceOptions final {
  bool preventInternalDiskAccess{true};
  bool bypassHardwareRequirements{};
  bool bypassOnlineAccountRequirement{};
  bool createLocalAccount{};
  std::string localAccountName;
  bool useRegionalOptions{};
  std::string localeName;
  bool disableDataCollection{};
  bool disableAutomaticDeviceEncryption{};
  bool applyQualityOfLifeOptions{};
};

enum class WindowsDeploymentMode {
  StandardInstallation,
  WindowsToGo,
};

struct WindowsUnattendResult final {
  std::string xml;
  std::string error;

  [[nodiscard]] bool succeeded() const noexcept { return error.empty(); }
};

[[nodiscard]] WindowsUnattendResult createWindowsToGoUnattend(
    ImageArchitecture architecture,
    const WindowsUserExperienceOptions& options);

[[nodiscard]] WindowsUnattendResult createWindowsUnattend(
    ImageArchitecture architecture,
    const WindowsUserExperienceOptions& options,
    WindowsDeploymentMode mode);

struct WindowsToGoOptions final {
  std::uint32_t editionIndex{1};
  WindowsUserExperienceOptions userExperience;
};

class WindowsToGoPlan final {
 public:
  [[nodiscard]] const ImageInfo& image() const noexcept { return image_; }
  [[nodiscard]] const BlockDeviceInfo& target() const noexcept { return target_; }
  [[nodiscard]] const WindowsEditionInfo& edition() const noexcept { return edition_; }
  [[nodiscard]] const WindowsToGoOptions& options() const noexcept { return options_; }
  [[nodiscard]] const std::string& volumeLabel() const noexcept { return volumeLabel_; }
  [[nodiscard]] const std::string& unattendXml() const noexcept { return unattendXml_; }
  [[nodiscard]] std::filesystem::file_time_type sourceLastWriteTime() const noexcept {
    return sourceLastWriteTime_;
  }

 private:
  friend class WindowsToGoPlanner;

  WindowsToGoPlan(ImageInfo image, BlockDeviceInfo target,
                  WindowsEditionInfo edition, WindowsToGoOptions options,
                  std::string volumeLabel, std::string unattendXml,
                  std::filesystem::file_time_type sourceLastWriteTime)
      : image_(std::move(image)),
        target_(std::move(target)),
        edition_(std::move(edition)),
        options_(std::move(options)),
        volumeLabel_(std::move(volumeLabel)),
        unattendXml_(std::move(unattendXml)),
        sourceLastWriteTime_(sourceLastWriteTime) {}

  ImageInfo image_;
  BlockDeviceInfo target_;
  WindowsEditionInfo edition_;
  WindowsToGoOptions options_;
  std::string volumeLabel_;
  std::string unattendXml_;
  std::filesystem::file_time_type sourceLastWriteTime_{};
};

struct WindowsToGoPlanResult final {
  std::optional<WindowsToGoPlan> plan;
  std::vector<SafetyIssue> issues;
  std::vector<std::string> warnings;

  [[nodiscard]] bool succeeded() const noexcept { return plan.has_value(); }
};

class WindowsToGoPlanner final {
 public:
  [[nodiscard]] WindowsToGoPlanResult build(
      const ImageInfo& image, const BlockDeviceInfo& target,
      WindowsToGoOptions options, std::string volumeLabel) const;
};

struct WindowsToGoSourceProgress final {
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct WindowsToGoSourceExtractionResult final {
  bool success{};
  bool cancelled{};
  std::uint64_t bytesExtracted{};
  std::string sourceEntry;
  std::string error;
};

using WindowsToGoSourceProgressCallback =
    std::function<void(const WindowsToGoSourceProgress&)>;
using WindowsToGoCancelCallback = std::function<bool()>;

// Extracts only sources/install.wim or sources/install.esd from the validated
// optical image. outputPath must not exist; partial output is removed.
[[nodiscard]] WindowsToGoSourceExtractionResult extractWindowsToGoSource(
    const WindowsToGoPlan& plan, const std::filesystem::path& outputPath,
    const WindowsToGoSourceProgressCallback& onProgress = {},
    const WindowsToGoCancelCallback& isCancelled = {});

}  // namespace rufus::core
