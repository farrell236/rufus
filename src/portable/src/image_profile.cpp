/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/image_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace rufus::core {

namespace {

constexpr std::uint64_t kFat32MaximumFileSize = 4ULL * 1024ULL * 1024ULL * 1024ULL;

std::string normalizedPath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  const auto version = path.find(';');
  if (version != std::string::npos) {
    path.erase(version);
  }
  return path;
}

bool endsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool hasExactPath(const std::vector<std::string>& paths, const std::string& expected) {
  return std::find(paths.begin(), paths.end(), expected) != paths.end();
}

bool hasPathPrefix(const std::vector<std::string>& paths, const std::string& prefix) {
  return std::any_of(paths.begin(), paths.end(), [&prefix](const std::string& path) {
    return path.rfind(prefix, 0) == 0;
  });
}

bool hasPathSuffix(const std::vector<std::string>& paths, const std::string& suffix) {
  return std::any_of(paths.begin(), paths.end(), [&suffix](const std::string& path) {
    return endsWith(path, suffix);
  });
}

bool hasPathOrSuffix(const std::vector<std::string>& paths, const std::string& path) {
  return hasExactPath(paths, path) || hasPathSuffix(paths, '/' + path);
}

bool hasEfiBootLoader(const std::vector<std::string>& paths) {
  return std::any_of(paths.begin(), paths.end(), [](const std::string& path) {
    return path.rfind("efi/", 0) == 0 && endsWith(path, ".efi");
  });
}

ImageArchitecture mergeArchitecture(const ImageArchitecture left,
                                    const ImageArchitecture right) {
  if (left == ImageArchitecture::Unknown) {
    return right;
  }
  if (right == ImageArchitecture::Unknown || left == right) {
    return left;
  }
  return ImageArchitecture::Multiple;
}

ImageArchitecture architectureFromBootLoaders(const std::vector<std::string>& paths) {
  ImageArchitecture architecture = ImageArchitecture::Unknown;
  const struct {
    const char* filename;
    ImageArchitecture architecture;
  } loaders[] = {{"bootia32.efi", ImageArchitecture::X86},
                 {"bootx64.efi", ImageArchitecture::X64},
                 {"bootarm.efi", ImageArchitecture::Arm},
                 {"bootaa64.efi", ImageArchitecture::Arm64},
                 {"bootriscv64.efi", ImageArchitecture::RiscV64},
                 {"bootloongarch64.efi", ImageArchitecture::LoongArch64}};
  for (const auto& loader : loaders) {
    if (hasPathSuffix(paths, std::string("/") + loader.filename) ||
        hasExactPath(paths, loader.filename)) {
      architecture = mergeArchitecture(architecture, loader.architecture);
    }
  }
  return architecture;
}

}  // namespace

void ImageProfileResolver::apply(const std::vector<ImageContentEntry>& contents, ImageInfo& image) {
  const ImageCapabilities inspected = image.capabilities;
  image.capabilities = {};
  image.capabilities.iso9660 = inspected.iso9660;
  image.capabilities.joliet = inspected.joliet;
  image.capabilities.udf = inspected.udf;
  image.capabilities.validBootCatalog = inspected.validBootCatalog;
  image.capabilities.windowsImageMetadata = inspected.windowsImageMetadata;
  image.capabilities.compressedSizeKnown = inspected.compressedSizeKnown;
  image.capabilities.validContainerMetadata = inspected.validContainerMetadata;
  image.capabilities.validPartitionTable = inspected.validPartitionTable;
  image.family = ImageFamily::Unknown;
  std::vector<std::string> paths;
  paths.reserve(contents.size());
  for (const auto& entry : contents) {
    paths.push_back(normalizedPath(entry.path));
    if (!entry.directory && entry.sizeBytes >= kFat32MaximumFileSize) {
      image.capabilities.containsLargeFile = true;
    }
  }

  const bool hasInstallImage = hasPathOrSuffix(paths, "sources/install.wim") ||
                               hasPathOrSuffix(paths, "sources/install.esd") ||
                               hasPathOrSuffix(paths, "sources/install.swm");
  const bool hasBootManager = hasExactPath(paths, "bootmgr") ||
                              hasExactPath(paths, "bootmgr.efi") ||
                              hasPathPrefix(paths, "efi/microsoft/boot/");
  const bool hasEfiLoader = hasEfiBootLoader(paths);
  const bool hasWindows = hasInstallImage && hasBootManager;
  const bool supportsWindowsToGoVersion =
      image.windowsVersionMajor > 6U ||
      (image.windowsVersionMajor == 6U && image.windowsVersionMinor >= 2U);
  const bool hasReactOs = hasPathPrefix(paths, "reactos/") ||
                          hasPathOrSuffix(paths, "setupldr.sys") ||
                          hasPathOrSuffix(paths, "freeldr.sys");
  const bool hasKolibriOs = hasExactPath(paths, "kolibri.img") ||
                            hasPathPrefix(paths, "kolibrios/");
  const bool usesCasper = hasExactPath(paths, "casper") ||
                          hasPathPrefix(paths, "casper/") ||
                          std::any_of(paths.begin(), paths.end(), [](const std::string& path) {
                            return path.rfind("casper", 0) == 0 &&
                                   path.find("pop-os") == std::string::npos;
                          });
  const bool popOsCasper = std::any_of(
      paths.begin(), paths.end(), [](const std::string& path) {
        return path.rfind("casper", 0) == 0 && path.find("pop-os") != std::string::npos;
      });
  const bool hasStandardGrubBiosTree =
      (hasExactPath(paths, "boot/grub/grub.cfg") &&
       hasExactPath(paths, "boot/grub/i386-pc/normal.mod")) ||
      (hasExactPath(paths, "boot/grub2/grub.cfg") &&
       hasExactPath(paths, "boot/grub2/i386-pc/normal.mod")) ||
      (hasExactPath(paths, "grub/grub.cfg") &&
       hasExactPath(paths, "grub/i386-pc/normal.mod"));

  image.capabilities.usesSyslinux = hasPathSuffix(paths, "/isolinux.bin") ||
                                    hasExactPath(paths, "isolinux.bin") ||
                                    hasPathOrSuffix(paths, "isolinux.cfg") ||
                                    hasPathOrSuffix(paths, "syslinux.cfg") ||
                                    hasPathOrSuffix(paths, "extlinux.conf") ||
                                    hasPathOrSuffix(paths, "txt.cfg") ||
                                    hasPathOrSuffix(paths, "live.cfg");
  image.capabilities.usesGrub = hasPathPrefix(paths, "boot/grub/") ||
                               hasPathPrefix(paths, "boot/grub2/") ||
                               hasPathSuffix(paths, "/grub.cfg") ||
                               hasExactPath(paths, "grub.cfg") ||
                               hasExactPath(paths, "grldr");
  image.capabilities.usesCasper = usesCasper;
  image.capabilities.uefiBootable = inspected.uefiBootable || hasEfiLoader ||
                                    hasPathPrefix(paths, "efi/microsoft/boot/");
  image.capabilities.biosBootable = inspected.biosBootable || image.bootable || hasBootManager ||
                                    image.capabilities.usesSyslinux ||
                                    image.capabilities.usesGrub;
  image.capabilities.isoExtraction = image.format == ImageFormat::Iso && !contents.empty();
  image.capabilities.rawWrite = image.format == ImageFormat::Raw ||
                                ((image.format == ImageFormat::Vhd ||
                                  image.format == ImageFormat::Vhdx) &&
                                 image.capabilities.validContainerMetadata &&
                                 image.containerPayloadLayout !=
                                     ContainerPayloadLayout::None &&
                                 image.containerPayloadSizeBytes != 0U) ||
                                (image.compressed &&
                                 image.capabilities.validContainerMetadata &&
                                 image.capabilities.compressedSizeKnown) ||
                                (image.format == ImageFormat::Iso &&
                                 image.partitionScheme != PartitionScheme::Unknown);
  image.capabilities.standardWindowsInstallation =
      hasWindows && inspected.windowsImageMetadata;
  image.capabilities.windowsCustomization =
      hasWindows && inspected.windowsImageMetadata;
  image.capabilities.windowsToGo = hasWindows && inspected.windowsImageMetadata &&
                                   supportsWindowsToGoVersion &&
                                   image.capabilities.uefiBootable;
  image.capabilities.linuxPersistence =
      image.format == ImageFormat::Iso &&
      (image.capabilities.usesSyslinux || image.capabilities.usesGrub) &&
      (image.capabilities.uefiBootable || hasStandardGrubBiosTree) &&
      !hasWindows && !hasReactOs && !hasKolibriOs && !popOsCasper;
  image.capabilities.requiresNtfs = image.capabilities.containsLargeFile;
  image.architecture = mergeArchitecture(image.architecture,
                                          architectureFromBootLoaders(paths));

  if (hasWindows) {
    image.family = ImageFamily::WindowsInstaller;
  } else if (image.capabilities.linuxPersistence) {
    image.family = ImageFamily::LinuxLive;
  } else if (image.bootable || image.capabilities.biosBootable ||
             image.capabilities.uefiBootable) {
    image.family = ImageFamily::OtherBootable;
  } else {
    image.family = ImageFamily::Unknown;
  }
  image.bootable = image.bootable || image.capabilities.biosBootable ||
                   image.capabilities.uefiBootable;
}

}  // namespace rufus::core
