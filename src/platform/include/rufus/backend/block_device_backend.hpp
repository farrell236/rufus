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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rufus/core/media.hpp"
#include "rufus/core/macos_installer.hpp"
#include "rufus/core/bad_block_test.hpp"
#include "rufus/core/media_capture.hpp"
#include "rufus/core/media_inspector.hpp"
#include "rufus/core/raw_image_writer.hpp"

namespace rufus::backend {

struct DeviceDiscoveryResult final {
  std::vector<core::BlockDeviceInfo> devices;
  std::vector<std::string> warnings;
};

struct BackendCapabilities final {
  bool physicalDeviceDiscovery{};
  bool readOnlyInspection{};
  bool hotplugMonitoring{};
  bool unmountVolumes{};
  bool exclusiveAccess{};
  bool rawWrite{};
  bool flush{};
  bool identityRevalidation{};
  bool rawVerification{};
  // FFU is not a raw byte stream. On Windows this capability delegates a
  // validated image to the operating system's DISM FFU provider.
  bool ffuApply{};
  bool mediaCapture{};
  bool badBlockTest{};
  bool eject{};
  bool macOsInstallerCreation{};
};

struct RawWriteAvailability final {
  bool available{};
  bool authorizationCanBeRequested{};
  std::string reason;
};

enum class PrivilegeRoute {
  Unprivileged,
  Elevated,
  PrivilegedHelper,
};

struct PrivilegeStatus final {
  PrivilegeRoute route{PrivilegeRoute::Unprivileged};
  std::string detail;
};

class BlockDeviceBackend {
 public:
  virtual ~BlockDeviceBackend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual BackendCapabilities capabilities() const noexcept = 0;
  [[nodiscard]] virtual PrivilegeStatus privilegeStatus() const {
    return {PrivilegeRoute::Unprivileged,
            "No privileged device transport is available"};
  }
  [[nodiscard]] virtual DeviceDiscoveryResult discover() const = 0;
  [[nodiscard]] virtual core::MediaInspectionResult inspectReadOnly(
      const core::BlockDeviceInfo&) const;
  [[nodiscard]] virtual RawWriteAvailability rawWriteAvailability(
      const core::BlockDeviceInfo&) const {
    return {false, false,
            "Raw-device writing is not provided by this platform backend"};
  }
  [[nodiscard]] virtual RawWriteAvailability requestRawWriteAuthorization() const {
    return {false, false,
            "Raw-device authorization is not provided by this platform backend"};
  }
  [[nodiscard]] virtual core::MacOsInstallerAnalysisResult
  analyzeMacOsInstallerApplication(const std::filesystem::path&) const {
    core::MacOsInstallerAnalysisResult result;
    result.error =
        "macOS installer applications are supported only by the macOS backend";
    return result;
  }
  [[nodiscard]] virtual RawWriteAvailability macOsInstallerAvailability(
      const core::MacOsInstallerInfo&,
      const core::BlockDeviceInfo&) const {
    return {false, false,
            "Bootable macOS installer creation is not provided by this platform backend"};
  }
  [[nodiscard]] virtual core::RawWriteResult createMacOsInstaller(
      const core::MacOsInstallerInfo&, const core::BlockDeviceInfo&, bool,
      const core::MacOsInstallerProgressCallback& = {},
      const core::MacOsInstallerCancelCallback& = {}) const {
    core::RawWriteResult result;
    result.error =
        "Bootable macOS installer creation is not provided by this platform backend";
    return result;
  }
  [[nodiscard]] virtual core::RawWriteResult writeRaw(
      const core::RawWritePlan&, const core::RawWriteProgressCallback& = {},
      const core::RawWriteCancelCallback& = {}) const {
    core::RawWriteResult result;
    result.error = "Raw-device writing is not provided by this platform backend";
    return result;
  }
  [[nodiscard]] virtual RawWriteAvailability ffuApplyAvailability(
      const core::ImageInfo&, const core::BlockDeviceInfo&) const {
    return {false, false,
            "FFU deployment is not provided by this platform backend"};
  }
  [[nodiscard]] virtual core::RawWriteResult applyFfu(
      const core::ImageInfo&, const core::BlockDeviceInfo&,
      const core::RawWriteProgressCallback& = {},
      const core::RawWriteCancelCallback& = {}) const {
    core::RawWriteResult result;
    result.error = "FFU deployment is not provided by this platform backend";
    return result;
  }
  [[nodiscard]] virtual RawWriteAvailability captureAvailability(
      const core::BlockDeviceInfo&, core::MediaCaptureFormat) const {
    return {false, false,
            "Device capture is not provided by this platform backend"};
  }
  [[nodiscard]] virtual core::MediaCaptureResult capture(
      const core::BlockDeviceInfo&, const std::filesystem::path&,
      const core::MediaCaptureOptions&,
      const core::MediaCaptureProgressCallback& = {},
      const core::MediaCaptureCancelCallback& = {}) const {
    core::MediaCaptureResult result;
    result.error = "Device capture is not provided by this platform backend";
    return result;
  }
  [[nodiscard]] virtual RawWriteAvailability badBlockTestAvailability(
      const core::BlockDeviceInfo& target) const {
    return rawWriteAvailability(target);
  }
  [[nodiscard]] virtual core::BadBlockTestResult testBadBlocks(
      const core::BlockDeviceInfo&, const core::BadBlockTestOptions&,
      const core::BadBlockTestProgressCallback& = {},
      const core::BadBlockTestCancelCallback& = {}) const {
    core::BadBlockTestResult result;
    result.error = "Bad-block testing is not provided by this platform backend";
    return result;
  }
};

[[nodiscard]] std::unique_ptr<BlockDeviceBackend> makePlatformBlockDeviceBackend();

}  // namespace rufus::backend
