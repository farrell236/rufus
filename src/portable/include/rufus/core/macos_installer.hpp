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
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rufus::core {

struct MacOsInstallerInfo final {
  std::string applicationPath;
  std::string createInstallMediaPath;
  std::string displayName;
  std::string version;
  std::string build;
  std::uint64_t payloadSizeBytes{};
  std::uint64_t minimumTargetBytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
  std::uint64_t recommendedTargetBytes{32ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct MacOsInstallerAnalysisResult final {
  std::optional<MacOsInstallerInfo> installer;
  std::vector<std::string> warnings;
  std::string error;

  [[nodiscard]] bool succeeded() const noexcept {
    return installer.has_value();
  }
};

enum class MacOsInstallerStage {
  Revalidating,
  Wiping,
  VerifyingWipe,
  PreparingTarget,
  CreatingInstaller,
  Complete,
};

struct MacOsInstallerProgress final {
  MacOsInstallerStage stage{MacOsInstallerStage::Revalidating};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
  std::string detail;
};

using MacOsInstallerProgressCallback =
    std::function<void(const MacOsInstallerProgress&)>;
using MacOsInstallerCancelCallback = std::function<bool()>;

[[nodiscard]] const char* macOsInstallerStageName(
    MacOsInstallerStage stage) noexcept;

}  // namespace rufus::core
