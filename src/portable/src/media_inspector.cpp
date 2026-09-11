/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/media_inspector.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "rufus/core/format.hpp"

namespace rufus::core {
namespace {

std::uint16_t little16(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t little32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t little64(const unsigned char* data) {
  return static_cast<std::uint64_t>(little32(data)) |
         (static_cast<std::uint64_t>(little32(data + 4U)) << 32U);
}

std::uint32_t crc32(const unsigned char* data, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned int bit = 0; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^
            (0xedb88320U & (0U - static_cast<std::uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

bool allZero(const unsigned char* data, const std::size_t size) {
  return std::all_of(data, data + size,
                     [](const unsigned char value) { return value == 0U; });
}

bool isEfiSystemGuid(const unsigned char* guid) {
  static constexpr std::array<unsigned char, 16> kEfiSystemGuid{
      0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  return std::equal(kEfiSystemGuid.begin(), kEfiSystemGuid.end(), guid);
}

std::string mbrType(const unsigned char type) {
  switch (type) {
    case 0x01:
      return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0e:
      return "FAT16";
    case 0x0b:
    case 0x0c:
      return "FAT32";
    case 0x07:
      return "NTFS/exFAT";
    case 0x83:
      return "Linux";
    case 0xee:
      return "GPT protective";
    case 0xef:
      return "EFI System";
    default: {
      std::ostringstream output;
      output << "MBR type 0x" << std::hex << std::setw(2)
             << std::setfill('0') << static_cast<unsigned int>(type);
      return output.str();
    }
  }
}

std::string detectFileSystem(const std::vector<unsigned char>& head,
                             const std::uint64_t byteOffset) {
  if (byteOffset > head.size() || head.size() - byteOffset < 512U) {
    return {};
  }
  const auto* boot = head.data() + static_cast<std::size_t>(byteOffset);
  if (std::memcmp(boot + 3U, "NTFS    ", 8U) == 0) {
    return "NTFS";
  }
  if (std::memcmp(boot + 3U, "EXFAT   ", 8U) == 0) {
    return "exFAT";
  }
  if (std::memcmp(boot + 82U, "FAT32   ", 8U) == 0) {
    return "FAT32";
  }
  if (std::memcmp(boot + 54U, "FAT16   ", 8U) == 0) {
    return "FAT16";
  }
  constexpr std::uint64_t kExtMagicOffset = 1024U + 56U;
  if (byteOffset <= head.size() &&
      kExtMagicOffset <= head.size() - byteOffset - 2U &&
      little16(boot + kExtMagicOffset) == 0xef53U) {
    return "ext2/ext3/ext4";
  }
  return {};
}

bool validGptHeader(const unsigned char* header, const std::size_t available,
                    const std::uint32_t sectorSize) {
  if (available < sectorSize || std::memcmp(header, "EFI PART", 8U) != 0) {
    return false;
  }
  const std::uint32_t headerSize = little32(header + 12U);
  if (headerSize < 92U || headerSize > sectorSize || headerSize > available) {
    return false;
  }
  std::vector<unsigned char> copy(header, header + headerSize);
  const std::uint32_t expected = little32(copy.data() + 16U);
  std::fill(copy.begin() + 16U, copy.begin() + 20U, 0U);
  return crc32(copy.data(), copy.size()) == expected;
}

}  // namespace

MediaInspectionResult inspectMediaSamples(
    const BlockDeviceInfo& device, const std::vector<unsigned char>& head,
    const std::vector<unsigned char>& tail) {
  MediaInspectionResult result;
  const std::uint32_t sector = device.logicalSectorSize;
  if (sector < 512U || sector > 4096U ||
      (sector & (sector - 1U)) != 0U || device.capacityBytes < sector ||
      device.capacityBytes % sector != 0U) {
    result.error = "The reported device geometry is not inspectable";
    return result;
  }
  if (head.size() < std::max<std::size_t>(512U, sector * 2U)) {
    result.error = "The read-only head sample is too short";
    return result;
  }

  const bool mbrSignature = head[510U] == 0x55U && head[511U] == 0xaaU;
  if (!mbrSignature) {
    result.warnings.emplace_back("No valid MBR signature was found at LBA 0");
  }
  for (unsigned int index = 0; index < 4U; ++index) {
    const auto* entry = head.data() + 446U + index * 16U;
    const unsigned char type = entry[4U];
    const std::uint32_t first = little32(entry + 8U);
    const std::uint32_t count = little32(entry + 12U);
    if (type == 0U || count == 0U) {
      continue;
    }
    if (type == 0xeeU) {
      result.protectiveMbr = true;
      continue;
    }
    InspectedPartition partition;
    partition.index = index + 1U;
    partition.firstLba = first;
    partition.lastLba = static_cast<std::uint64_t>(first) + count - 1U;
    partition.type = mbrType(type);
    partition.active = entry[0] == 0x80U;
    partition.efiSystem = type == 0xefU;
    partition.fileSystem = detectFileSystem(
        head, partition.firstLba * static_cast<std::uint64_t>(sector));
    result.partitions.push_back(std::move(partition));
  }

  const auto* primaryGpt = head.data() + sector;
  const bool gptSignature = std::memcmp(primaryGpt, "EFI PART", 8U) == 0;
  if (gptSignature || result.protectiveMbr) {
    result.partitionScheme = PartitionScheme::Gpt;
    result.primaryTableValid =
        validGptHeader(primaryGpt, head.size() - sector, sector);
    if (!result.primaryTableValid) {
      result.warnings.emplace_back(
          "The primary GPT header signature or CRC is invalid");
    } else {
      const std::uint64_t entriesLba = little64(primaryGpt + 72U);
      const std::uint32_t entryCount = little32(primaryGpt + 80U);
      const std::uint32_t entrySize = little32(primaryGpt + 84U);
      if (entrySize < 128U || entrySize > 4096U || entryCount == 0U ||
          entriesLba > std::numeric_limits<std::uint64_t>::max() / sector) {
        result.warnings.emplace_back("The GPT partition-entry geometry is invalid");
      } else {
        const std::uint64_t entriesOffset = entriesLba * sector;
        const std::uint32_t boundedCount = std::min<std::uint32_t>(entryCount, 128U);
        for (std::uint32_t index = 0; index < boundedCount; ++index) {
          const std::uint64_t offset = entriesOffset +
                                       static_cast<std::uint64_t>(index) * entrySize;
          if (offset > head.size() || head.size() - offset < 56U) {
            break;
          }
          const auto* entry = head.data() + static_cast<std::size_t>(offset);
          if (allZero(entry, 16U)) {
            continue;
          }
          const std::uint64_t first = little64(entry + 32U);
          const std::uint64_t last = little64(entry + 40U);
          if (first == 0U || last < first ||
              last >= device.capacityBytes / sector) {
            result.warnings.emplace_back("A GPT partition entry has invalid bounds");
            continue;
          }
          InspectedPartition partition;
          partition.index = index + 1U;
          partition.firstLba = first;
          partition.lastLba = last;
          partition.efiSystem = isEfiSystemGuid(entry);
          partition.type = partition.efiSystem ? "EFI System" : "GPT data";
          partition.fileSystem = detectFileSystem(head, first * sector);
          result.partitions.push_back(std::move(partition));
        }
      }
    }

    const std::uint64_t backupLba = little64(primaryGpt + 32U);
    const std::uint64_t tailStart =
        tail.size() > device.capacityBytes ? 0U
                                           : device.capacityBytes - tail.size();
    if (!tail.empty() && backupLba <=
                             std::numeric_limits<std::uint64_t>::max() / sector) {
      const std::uint64_t backupOffset = backupLba * sector;
      if (backupOffset >= tailStart && backupOffset - tailStart < tail.size()) {
        const std::size_t localOffset =
            static_cast<std::size_t>(backupOffset - tailStart);
        result.backupTableValid = validGptHeader(
            tail.data() + localOffset, tail.size() - localOffset, sector);
      }
    }
    if (!result.backupTableValid) {
      result.warnings.emplace_back(
          "The backup GPT header could not be validated from the tail sample");
    }
  } else if (mbrSignature) {
    result.partitionScheme = PartitionScheme::Mbr;
    result.primaryTableValid = true;
  }

  result.bootable = std::any_of(
      result.partitions.begin(), result.partitions.end(), [](const auto& partition) {
        return partition.active || partition.efiSystem;
      });
  if (result.partitionScheme == PartitionScheme::Unknown) {
    result.findings.emplace_back("No recognized partition table");
  } else {
    result.findings.push_back(
        std::string(partitionSchemeName(result.partitionScheme)) +
        " partition table detected");
  }
  result.findings.push_back(std::to_string(result.partitions.size()) +
                            " partition(s) detected");
  result.findings.push_back(result.bootable
                                ? "Boot indicators are present"
                                : "No BIOS-active or EFI System partition was found");
  result.success = true;
  return result;
}

std::string MediaInspectionResult::toText(const BlockDeviceInfo& device) const {
  std::ostringstream output;
  output << "Read-only media inspection\n\n";
  output << "Device: " << device.displayName << '\n';
  output << "Path: " << device.devicePath << '\n';
  output << "Capacity: " << formatByteSize(device.capacityBytes) << '\n';
  output << "Sector size: " << device.logicalSectorSize << " bytes\n";
  if (!success) {
    output << "\nInspection failed: " << error << '\n';
    return output.str();
  }
  output << "Partition scheme: " << partitionSchemeName(partitionScheme) << '\n';
  output << "Boot indicators: " << (bootable ? "present" : "not found") << '\n';
  output << "Primary table: " << (primaryTableValid ? "valid" : "not validated")
         << '\n';
  if (partitionScheme == PartitionScheme::Gpt) {
    output << "Backup GPT: " << (backupTableValid ? "valid" : "not validated")
           << '\n';
  }
  for (const auto& partition : partitions) {
    output << "\nPartition " << partition.index << ": " << partition.type
           << "\n  LBAs: " << partition.firstLba << "–" << partition.lastLba;
    if (!partition.fileSystem.empty()) {
      output << "\n  Filesystem: " << partition.fileSystem;
    }
    if (partition.active) {
      output << "\n  BIOS active";
    }
    if (partition.efiSystem) {
      output << "\n  EFI System partition";
    }
    output << '\n';
  }
  for (const auto& warning : warnings) {
    output << "\nWarning: " << warning;
  }
  output << '\n';
  return output.str();
}

}  // namespace rufus::core
