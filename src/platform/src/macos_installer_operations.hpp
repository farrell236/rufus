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
#include <string>

#include "rufus/backend/block_device_backend.hpp"

namespace rufus::backend::macos {

[[nodiscard]] core::MacOsInstallerAnalysisResult analyzeInstallerApplication(
    const std::filesystem::path& applicationPath);

[[nodiscard]] core::RawWriteResult createInstallerLocally(
    const core::MacOsInstallerInfo& selected,
    const core::BlockDeviceInfo& target, bool fullWipe,
    const std::string& operationId,
    const core::MacOsInstallerProgressCallback& onProgress,
    const core::MacOsInstallerCancelCallback& isCancelled);

}  // namespace rufus::backend::macos
