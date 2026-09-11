/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/media.hpp"

namespace rufus::core {

DeviceEligibility evaluateDeviceEligibility(const BlockDeviceInfo& device) noexcept {
  if (device.stableId.empty() || device.devicePath.empty()) {
    return DeviceEligibility::MissingIdentity;
  }
  if (!device.wholeDevice) {
    return DeviceEligibility::NotWholeDevice;
  }
  if (!device.writable) {
    return DeviceEligibility::ReadOnly;
  }
  if (device.systemDevice) {
    return DeviceEligibility::SystemDevice;
  }
  if (!device.removable && device.bus != DeviceBus::Usb && device.bus != DeviceBus::Sd) {
    return DeviceEligibility::NotRemovable;
  }
  return DeviceEligibility::Eligible;
}

std::string_view deviceEligibilityName(const DeviceEligibility eligibility) noexcept {
  switch (eligibility) {
    case DeviceEligibility::Eligible:
      return "Eligible";
    case DeviceEligibility::MissingIdentity:
      return "Missing stable identity";
    case DeviceEligibility::NotWholeDevice:
      return "Not a whole device";
    case DeviceEligibility::ReadOnly:
      return "Read-only device";
    case DeviceEligibility::SystemDevice:
      return "System device";
    case DeviceEligibility::NotRemovable:
      return "Fixed device";
  }
  return "Unknown";
}

std::string_view deviceBusName(const DeviceBus bus) noexcept {
  switch (bus) {
    case DeviceBus::Unknown:
      return "Unknown";
    case DeviceBus::Usb:
      return "USB";
    case DeviceBus::Sd:
      return "SD";
    case DeviceBus::Thunderbolt:
      return "Thunderbolt";
    case DeviceBus::Nvme:
      return "NVMe";
    case DeviceBus::Sata:
      return "SATA";
    case DeviceBus::Virtual:
      return "Virtual";
  }
  return "Unknown";
}

std::string_view imageFormatName(const ImageFormat format) noexcept {
  switch (format) {
    case ImageFormat::Unknown:
      return "Unknown";
    case ImageFormat::Iso:
      return "ISO";
    case ImageFormat::Raw:
      return "Raw image";
    case ImageFormat::Vhd:
      return "VHD";
    case ImageFormat::Vhdx:
      return "VHDX";
    case ImageFormat::Ffu:
      return "FFU";
    case ImageFormat::Gzip:
      return "gzip-compressed image";
    case ImageFormat::Bzip2:
      return "bzip2-compressed image";
    case ImageFormat::Zip:
      return "ZIP archive";
    case ImageFormat::Lzma:
      return "LZMA-compressed image";
    case ImageFormat::Xz:
      return "XZ-compressed image";
    case ImageFormat::Zstd:
      return "Zstandard-compressed image";
    case ImageFormat::MacOsInstallerApplication:
      return "macOS installer application";
  }
  return "Unknown";
}

std::string_view partitionSchemeName(const PartitionScheme scheme) noexcept {
  switch (scheme) {
    case PartitionScheme::Unknown:
      return "Unknown";
    case PartitionScheme::Mbr:
      return "MBR";
    case PartitionScheme::Gpt:
      return "GPT";
  }
  return "Unknown";
}

std::string_view imageFamilyName(const ImageFamily family) noexcept {
  switch (family) {
    case ImageFamily::Unknown:
      return "Unknown";
    case ImageFamily::WindowsInstaller:
      return "Windows installer";
    case ImageFamily::LinuxLive:
      return "Linux live image";
    case ImageFamily::OtherBootable:
      return "Bootable image";
    case ImageFamily::MacOsInstaller:
      return "macOS installer";
  }
  return "Unknown";
}

std::string_view imageArchitectureName(const ImageArchitecture architecture) noexcept {
  switch (architecture) {
    case ImageArchitecture::Unknown:
      return "Unknown";
    case ImageArchitecture::X86:
      return "x86";
    case ImageArchitecture::X64:
      return "x86-64";
    case ImageArchitecture::Arm:
      return "ARM";
    case ImageArchitecture::Arm64:
      return "ARM64";
    case ImageArchitecture::Itanium:
      return "Itanium";
    case ImageArchitecture::RiscV64:
      return "RISC-V 64";
    case ImageArchitecture::LoongArch64:
      return "LoongArch64";
    case ImageArchitecture::Multiple:
      return "Multiple architectures";
  }
  return "Unknown";
}

}  // namespace rufus::core
