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
#include <vector>

namespace rufus::core {

enum class DeviceBus {
  Unknown,
  Usb,
  Sd,
  Thunderbolt,
  Nvme,
  Sata,
  Virtual,
};

enum class DeviceEligibility {
  Eligible,
  MissingIdentity,
  NotWholeDevice,
  ReadOnly,
  SystemDevice,
  NotRemovable,
};

struct BlockDeviceInfo final {
  std::string stableId;
  std::string devicePath;
  std::string displayName;
  std::string vendor;
  std::string model;
  std::string serialNumber;
  std::uint64_t capacityBytes{};
  std::uint32_t logicalSectorSize{};
  DeviceBus bus{DeviceBus::Unknown};
  bool removable{};
  bool ejectable{};
  bool writable{};
  bool systemDevice{};
  bool wholeDevice{};
  std::vector<std::string> mountPoints;
};

enum class ImageFormat {
  Unknown,
  Iso,
  Raw,
  Vhd,
  Vhdx,
  Ffu,
  Gzip,
  Bzip2,
  Zip,
  Lzma,
  Xz,
  Zstd,
  MacOsInstallerApplication,
};

enum class PartitionScheme {
  Unknown,
  Mbr,
  Gpt,
};

enum class ImageFamily {
  Unknown,
  WindowsInstaller,
  LinuxLive,
  OtherBootable,
  MacOsInstaller,
};

enum class ImageArchitecture {
  Unknown,
  X86,
  X64,
  Arm,
  Arm64,
  Itanium,
  RiscV64,
  LoongArch64,
  Multiple,
};

enum class ContainerPayloadLayout {
  None,
  Contiguous,
  DynamicVhd,
  DynamicVhdx,
};

enum class LinuxPersistenceStyle {
  None,
  Casper,
  DebianLive,
};

struct ImageCapabilities final {
  bool isoExtraction{};
  bool rawWrite{};
  bool standardWindowsInstallation{};
  bool windowsToGo{};
  bool windowsCustomization{};
  bool linuxPersistence{};
  bool biosBootable{};
  bool uefiBootable{};
  bool requiresNtfs{};
  bool containsLargeFile{};
  bool usesSyslinux{};
  bool usesGrub{};
  LinuxPersistenceStyle linuxPersistenceStyle{LinuxPersistenceStyle::None};
  bool iso9660{};
  bool joliet{};
  bool udf{};
  bool validBootCatalog{};
  bool windowsImageMetadata{};
  bool compressedSizeKnown{};
  bool validContainerMetadata{};
  bool validPartitionTable{};
  bool macOsInstallerCreation{};
};

struct WindowsEditionInfo final {
  std::uint32_t index{};
  std::string name;
  std::string description;
  std::uint64_t totalBytes{};
  std::uint32_t versionMajor{};
  std::uint32_t versionMinor{};
  std::uint32_t build{};
  ImageArchitecture architecture{ImageArchitecture::Unknown};
};

struct ImageInfo final {
  std::string path;
  std::string displayName;
  std::string volumeLabel;
  std::uint64_t sizeBytes{};
  std::uint64_t expandedSizeBytes{};
  // Validated byte range containing a directly deployable raw disk inside a
  // container. A zero payload size means the complete source file is used.
  std::uint64_t containerPayloadOffsetBytes{};
  std::uint64_t containerPayloadSizeBytes{};
  std::uint64_t containerAllocationTableOffsetBytes{};
  std::uint32_t containerBlockSizeBytes{};
  std::uint32_t containerLogicalSectorSize{};
  ContainerPayloadLayout containerPayloadLayout{ContainerPayloadLayout::None};
  std::uint32_t windowsImageCount{};
  std::uint32_t windowsBootIndex{};
  std::uint32_t windowsVersionMajor{};
  std::uint32_t windowsVersionMinor{};
  std::uint32_t windowsBuild{};
  ImageFormat format{ImageFormat::Unknown};
  PartitionScheme partitionScheme{PartitionScheme::Unknown};
  ImageFamily family{ImageFamily::Unknown};
  ImageArchitecture architecture{ImageArchitecture::Unknown};
  ImageCapabilities capabilities;
  std::vector<WindowsEditionInfo> windowsEditions;
  bool bootable{};
  bool compressed{};

  [[nodiscard]] std::uint64_t deploymentSizeBytes() const noexcept {
    if (compressed) {
      return expandedSizeBytes;
    }
    return containerPayloadSizeBytes == 0U ? sizeBytes
                                          : containerPayloadSizeBytes;
  }
};

[[nodiscard]] DeviceEligibility evaluateDeviceEligibility(const BlockDeviceInfo& device) noexcept;
[[nodiscard]] std::string_view deviceEligibilityName(DeviceEligibility eligibility) noexcept;
[[nodiscard]] std::string_view deviceBusName(DeviceBus bus) noexcept;
[[nodiscard]] std::string_view imageFormatName(ImageFormat format) noexcept;
[[nodiscard]] std::string_view partitionSchemeName(PartitionScheme scheme) noexcept;
[[nodiscard]] std::string_view imageFamilyName(ImageFamily family) noexcept;
[[nodiscard]] std::string_view imageArchitectureName(ImageArchitecture architecture) noexcept;

}  // namespace rufus::core
