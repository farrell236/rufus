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

#include "rufus/backend/block_device_backend.hpp"

namespace rufus::backend::macos {

[[nodiscard]] DeviceDiscoveryResult discoverBlockDevices();
[[nodiscard]] core::RawWriteResult writeRawLocally(
    const core::RawWritePlan& plan, core::RawSourceIo* source,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled);
[[nodiscard]] core::BadBlockTestResult testBadBlocksLocally(
    const core::BlockDeviceInfo& target,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled);
[[nodiscard]] core::MediaCaptureResult captureRawLocally(
    const core::BlockDeviceInfo& source, int destinationDescriptor,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled);

}  // namespace rufus::backend::macos
