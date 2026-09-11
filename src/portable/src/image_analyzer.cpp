/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/image_analyzer.hpp"

#include "compressed_image_inspector.hpp"
#include "compressed_image_source.hpp"
#include "iso9660_reader.hpp"
#include "linux_persistence_support.hpp"
#include "udf_reader.hpp"
#include "wim_inspector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "rufus/core/image_profile.hpp"
#include "rufus/core/linux_persistence.hpp"

namespace rufus::core {

namespace {

constexpr std::uint64_t kIsoSectorSize = 2048;
constexpr std::uint64_t kIsoDescriptorStart = 16;
constexpr std::uint64_t kIsoDescriptorLimit = 64;
constexpr std::uint64_t kMaximumLinuxBootConfigurationBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumLinuxBootConfigurationsTotalBytes =
    8ULL * 1024ULL * 1024ULL;

struct LinuxPersistenceInspection final {
  LinuxPersistenceStyle style{LinuxPersistenceStyle::None};
  std::vector<std::string> warnings;
  bool cancelled{};
};

LinuxPersistenceInspection inspectLinuxPersistence(
    const std::filesystem::path& imagePath,
    const std::vector<detail::ImageFileRecord>& files,
    const ImageAnalysisCancelCallback& isCancelled) {
  LinuxPersistenceInspection result;
  std::uint64_t inspectedBytes = 0;
  for (const auto& file : files) {
    if (!detail::isLinuxBootConfigurationPath(file.path)) {
      continue;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      return result;
    }
    if (file.sizeBytes == 0U ||
        file.sizeBytes > kMaximumLinuxBootConfigurationBytes ||
        inspectedBytes > kMaximumLinuxBootConfigurationsTotalBytes -
                             file.sizeBytes) {
      result.warnings.emplace_back(
          "A Linux boot configuration was too large to inspect safely for persistence: " +
          file.path);
      continue;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(file.sizeBytes));
    std::string error;
    if (!detail::readImageFile(imagePath, file, 0U, bytes.data(), bytes.size(),
                               error)) {
      result.warnings.emplace_back(
          "Unable to inspect a Linux boot configuration for persistence: " +
          file.path + " (" + error + ")");
      continue;
    }
    inspectedBytes += file.sizeBytes;
    const LinuxPersistenceStyle detected = detectLinuxPersistenceStyle(
        file.path, std::string_view(
                       reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (detected == LinuxPersistenceStyle::None) {
      continue;
    }
    if (result.style != LinuxPersistenceStyle::None &&
        result.style != detected) {
      result.style = LinuxPersistenceStyle::None;
      result.warnings.emplace_back(
          "Conflicting Casper and Debian Live persistence entries were detected; "
          "automatic persistence is disabled");
      return result;
    }
    result.style = detected;
  }
  return result;
}

template <std::size_t Size>
bool readAt(std::ifstream& stream, const std::uint64_t offset, std::array<unsigned char, Size>& data) {
  stream.clear();
  stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream) {
    return false;
  }
  stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return stream.gcount() == static_cast<std::streamsize>(data.size());
}

bool readAt(std::ifstream& stream, const std::uint64_t offset,
            unsigned char* data, const std::size_t size) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  stream.clear();
  stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream) {
    return false;
  }
  stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
  return stream.gcount() == static_cast<std::streamsize>(size);
}

std::uint32_t littleEndian32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t littleEndian64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

std::uint32_t bigEndian32(const unsigned char* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) |
         static_cast<std::uint32_t>(data[3]);
}

std::uint64_t bigEndian64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value = (value << 8U) | data[index];
  }
  return value;
}

std::uint32_t crc32(const unsigned char* data, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

std::uint32_t crc32c(const unsigned char* data, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool startsWith(const unsigned char* data, const std::size_t size, const char* value,
                const std::size_t valueSize) {
  return size >= valueSize && std::equal(value, value + valueSize, data);
}

std::string lowerExtension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return extension;
}

std::string trimIsoText(const unsigned char* data, const std::size_t size) {
  std::string text(reinterpret_cast<const char*>(data), size);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) {
    text.pop_back();
  }
  const auto first = text.find_first_not_of(' ');
  return first == std::string::npos ? std::string{} : text.substr(first);
}

bool hasIsoIdentifier(const std::array<unsigned char, kIsoSectorSize>& descriptor) {
  constexpr std::array<unsigned char, 5> identifier{'C', 'D', '0', '0', '1'};
  return std::equal(identifier.begin(), identifier.end(), descriptor.begin() + 1);
}

void inspectIso(std::ifstream& stream, ImageInfo& image) {
  std::array<unsigned char, kIsoSectorSize> descriptor{};
  bool foundIso = false;

  for (std::uint64_t sector = kIsoDescriptorStart; sector < kIsoDescriptorLimit; ++sector) {
    if (!readAt(stream, sector * kIsoSectorSize, descriptor) || !hasIsoIdentifier(descriptor)) {
      break;
    }

    foundIso = true;
    const unsigned char type = descriptor[0];
    if (type == 1) {
      image.volumeLabel = trimIsoText(descriptor.data() + 40, 32);
    } else if (type == 255) {
      break;
    }
  }

  if (foundIso) {
    image.format = ImageFormat::Iso;
  }
}

bool validGpt(std::ifstream& stream, const std::uint64_t fileSize,
              const std::array<unsigned char, 512>& header, ImageInfo& image) {
  constexpr std::array<unsigned char, 8> signature{'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
  if (!std::equal(signature.begin(), signature.end(), header.begin()) ||
      littleEndian32(header.data() + 8) != 0x00010000U) {
    return false;
  }
  const std::uint32_t headerSize = littleEndian32(header.data() + 12);
  if (headerSize < 92U || headerSize > header.size()) {
    return false;
  }
  std::array<unsigned char, 512> checked = header;
  const std::uint32_t expectedHeaderCrc = littleEndian32(checked.data() + 16);
  std::fill(checked.begin() + 16, checked.begin() + 20, 0);
  if (crc32(checked.data(), headerSize) != expectedHeaderCrc ||
      littleEndian64(header.data() + 24) != 1U) {
    return false;
  }
  const std::uint64_t lastLba = fileSize / 512U - 1U;
  const std::uint64_t backupLba = littleEndian64(header.data() + 32);
  const std::uint64_t firstUsable = littleEndian64(header.data() + 40);
  const std::uint64_t lastUsable = littleEndian64(header.data() + 48);
  const std::uint64_t entriesLba = littleEndian64(header.data() + 72);
  const std::uint32_t entryCount = littleEndian32(header.data() + 80);
  const std::uint32_t entrySize = littleEndian32(header.data() + 84);
  if (backupLba > lastLba || firstUsable > lastUsable || lastUsable > lastLba ||
      entriesLba > lastLba || entryCount == 0 || entryCount > 1048576U ||
      entrySize < 128U || entrySize > 4096U || entrySize % 8U != 0) {
    return false;
  }
  const std::uint64_t entriesSize = static_cast<std::uint64_t>(entryCount) * entrySize;
  const std::uint64_t entriesOffset = entriesLba * 512U;
  if (entriesSize > 64ULL * 1024ULL * 1024ULL || entriesOffset > fileSize ||
      entriesSize > fileSize - entriesOffset ||
      entriesSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  std::vector<unsigned char> entries(static_cast<std::size_t>(entriesSize));
  if (!readAt(stream, entriesOffset, entries.data(), entries.size()) ||
      crc32(entries.data(), entries.size()) != littleEndian32(header.data() + 88)) {
    return false;
  }
  constexpr std::array<unsigned char, 16> efiSystemPartition{
      0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  for (std::uint32_t index = 0; index < entryCount; ++index) {
    const unsigned char* entry = entries.data() + static_cast<std::size_t>(index) * entrySize;
    const bool unused = std::all_of(entry, entry + 16, [](const unsigned char value) {
      return value == 0;
    });
    if (unused) {
      continue;
    }
    const std::uint64_t partitionFirst = littleEndian64(entry + 32);
    const std::uint64_t partitionLast = littleEndian64(entry + 40);
    if (partitionFirst < firstUsable || partitionFirst > partitionLast ||
        partitionLast > lastUsable) {
      return false;
    }
    if (std::equal(efiSystemPartition.begin(), efiSystemPartition.end(), entry)) {
      image.capabilities.uefiBootable = true;
      image.bootable = true;
      break;
    }
  }
  return true;
}

void inspectPartitionTable(std::ifstream& stream, const std::uint64_t fileSize,
                           ImageInfo& image, std::vector<std::string>& warnings) {
  std::array<unsigned char, 512> sector{};
  if (!readAt(stream, 0, sector) || sector[510] != 0x55 || sector[511] != 0xaa) {
    return;
  }

  std::array<unsigned char, 512> gptHeader{};
  constexpr std::array<unsigned char, 8> gptSignature{'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
  if (readAt(stream, 512, gptHeader) &&
      std::equal(gptSignature.begin(), gptSignature.end(), gptHeader.begin())) {
    if (validGpt(stream, fileSize, gptHeader, image)) {
      image.partitionScheme = PartitionScheme::Gpt;
      image.capabilities.validPartitionTable = true;
    } else {
      warnings.emplace_back("GPT signature is present but its header or entry checksum is invalid");
    }
    return;
  }

  bool hasPartition = false;
  bool activePartition = false;
  for (std::size_t offset = 446; offset < 510; offset += 16) {
    const unsigned char bootIndicator = sector[offset];
    const unsigned char type = sector[offset + 4U];
    const std::uint32_t firstLba = littleEndian32(sector.data() + offset + 8U);
    const std::uint32_t sectors = littleEndian32(sector.data() + offset + 12U);
    if (bootIndicator != 0 && bootIndicator != 0x80U) {
      warnings.emplace_back("MBR partition table contains an invalid boot indicator");
      return;
    }
    if (type != 0 && sectors != 0 && firstLba < fileSize / 512U &&
        sectors <= fileSize / 512U - firstLba) {
      hasPartition = true;
      activePartition = activePartition || bootIndicator == 0x80U;
    }
  }
  if (hasPartition) {
    image.partitionScheme = PartitionScheme::Mbr;
    image.capabilities.validPartitionTable = true;
    image.bootable = activePartition;
    image.capabilities.biosBootable = activePartition;
  }
}

bool inspectVhd(std::ifstream& stream, const std::uint64_t fileSize, ImageInfo& image) {
  if (fileSize < 512U) {
    return false;
  }
  std::array<unsigned char, 512> footer{};
  if (!readAt(stream, fileSize - footer.size(), footer) ||
      !startsWith(footer.data(), footer.size(), "conectix", 8)) {
    return false;
  }
  const std::uint32_t storedChecksum = bigEndian32(footer.data() + 64);
  const std::uint32_t features = bigEndian32(footer.data() + 8);
  const std::uint32_t version = bigEndian32(footer.data() + 12);
  const std::uint64_t dataOffset = bigEndian64(footer.data() + 16);
  std::fill(footer.begin() + 64, footer.begin() + 68, 0);
  std::uint32_t sum = 0;
  for (const unsigned char value : footer) {
    sum += value;
  }
  const std::uint64_t currentSize = bigEndian64(footer.data() + 48);
  const std::uint32_t diskType = bigEndian32(footer.data() + 60);
  if (~sum != storedChecksum || (features & 0xfffffffeU) != 2U ||
      version != 0x00010000U || currentSize == 0 || currentSize % 512U != 0 ||
      diskType < 2U || diskType > 4U ||
      (diskType == 2U && dataOffset != std::numeric_limits<std::uint64_t>::max()) ||
      (diskType != 2U && dataOffset >= fileSize)) {
    return false;
  }
  image.format = ImageFormat::Vhd;
  image.expandedSizeBytes = currentSize;
  image.capabilities.validContainerMetadata = true;
  if (diskType == 2U && fileSize >= 512U && currentSize == fileSize - 512U) {
    image.containerPayloadOffsetBytes = 0U;
    image.containerPayloadSizeBytes = currentSize;
    image.containerPayloadLayout = ContainerPayloadLayout::Contiguous;
  } else if (diskType == 3U) {
    std::array<unsigned char, 1024> header{};
    if (!readAt(stream, dataOffset, header) ||
        !startsWith(header.data(), header.size(), "cxsparse", 8)) {
      return false;
    }
    const std::uint32_t headerChecksum = bigEndian32(header.data() + 36U);
    std::fill(header.begin() + 36U, header.begin() + 40U, 0);
    std::uint32_t headerSum = 0;
    for (const unsigned char value : header) {
      headerSum += value;
    }
    const std::uint64_t tableOffset = bigEndian64(header.data() + 16U);
    const std::uint32_t headerVersion = bigEndian32(header.data() + 24U);
    const std::uint32_t tableEntries = bigEndian32(header.data() + 28U);
    const std::uint32_t blockSize = bigEndian32(header.data() + 32U);
    const std::uint64_t tableBytes =
        static_cast<std::uint64_t>(tableEntries) * 4U;
    if (~headerSum != headerChecksum ||
        bigEndian64(header.data() + 8U) !=
            std::numeric_limits<std::uint64_t>::max() ||
        headerVersion != 0x00010000U || blockSize < 512U ||
        blockSize % 512U != 0U || (blockSize & (blockSize - 1U)) != 0U ||
        tableEntries == 0U || tableBytes > 64ULL * 1024ULL * 1024ULL ||
        tableOffset % 512U != 0U ||
        tableOffset > fileSize || tableBytes > fileSize - tableOffset) {
      return false;
    }
    const std::uint64_t requiredEntries =
        (currentSize - 1U) / blockSize + 1U;
    if (requiredEntries > tableEntries) {
      return false;
    }
    std::vector<unsigned char> table(static_cast<std::size_t>(tableBytes));
    if (!readAt(stream, tableOffset, table.data(), table.size())) {
      return false;
    }
    const std::uint64_t bitmapBytes =
        (((static_cast<std::uint64_t>(blockSize) / 512U + 7U) / 8U + 511U) /
         512U) * 512U;
    const std::uint64_t tableEnd =
        ((tableOffset + tableBytes + 511U) / 512U) * 512U;
    std::vector<std::uint64_t> allocatedOffsets;
    allocatedOffsets.reserve(static_cast<std::size_t>(requiredEntries));
    for (std::uint64_t index = 0; index < requiredEntries; ++index) {
      const std::uint32_t entry =
          bigEndian32(table.data() + static_cast<std::size_t>(index) * 4U);
      if (entry == 0xffffffffU) {
        continue;
      }
      const std::uint64_t blockOffset = static_cast<std::uint64_t>(entry) * 512U;
      if (blockOffset < tableEnd || blockOffset >= fileSize - 512U ||
          bitmapBytes > fileSize - 512U - blockOffset ||
          blockSize > fileSize - 512U - blockOffset - bitmapBytes) {
        return false;
      }
      allocatedOffsets.push_back(blockOffset);
    }
    std::sort(allocatedOffsets.begin(), allocatedOffsets.end());
    if (std::adjacent_find(allocatedOffsets.begin(), allocatedOffsets.end()) !=
        allocatedOffsets.end()) {
      return false;
    }
    image.containerPayloadSizeBytes = currentSize;
    image.containerAllocationTableOffsetBytes = tableOffset;
    image.containerBlockSizeBytes = blockSize;
    image.containerLogicalSectorSize = 512U;
    image.containerPayloadLayout = ContainerPayloadLayout::DynamicVhd;
  }
  return true;
}

bool inspectVhdx(std::ifstream& stream, const std::uint64_t fileSize, ImageInfo& image) {
  if (fileSize < 1024U * 1024U) {
    return false;
  }
  bool validHeader = false;
  bool activeLog = false;
  std::uint64_t activeSequence = 0;
  for (const std::uint64_t offset : {64ULL * 1024ULL, 128ULL * 1024ULL}) {
    std::array<unsigned char, 4096> header{};
    if (!readAt(stream, offset, header) ||
        !startsWith(header.data(), header.size(), "head", 4)) {
      continue;
    }
    const std::uint32_t checksum = littleEndian32(header.data() + 4);
    const std::uint64_t sequence = littleEndian64(header.data() + 8);
    const bool hasLog = std::any_of(header.begin() + 48U, header.begin() + 64U,
                                    [](const unsigned char byte) {
                                      return byte != 0U;
                                    });
    std::fill(header.begin() + 4, header.begin() + 8, 0);
    if (sequence != 0U && crc32c(header.data(), header.size()) == checksum &&
        (!validHeader || sequence > activeSequence)) {
      validHeader = true;
      activeSequence = sequence;
      activeLog = hasLog;
    }
  }
  if (!validHeader) {
    return false;
  }

  constexpr std::array<unsigned char, 16> batRegion{
      0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
      0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
  constexpr std::array<unsigned char, 16> metadataRegion{
      0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
      0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
  std::uint64_t metadataOffset = 0;
  std::uint32_t metadataLength = 0;
  std::uint64_t batOffset = 0;
  std::uint32_t batLength = 0;
  bool validRegions = false;
  for (const std::uint64_t tableOffset : {192ULL * 1024ULL, 256ULL * 1024ULL}) {
    std::array<unsigned char, 64U * 1024U> table{};
    if (!readAt(stream, tableOffset, table) ||
        !startsWith(table.data(), table.size(), "regi", 4)) {
      continue;
    }
    const std::uint32_t checksum = littleEndian32(table.data() + 4);
    std::fill(table.begin() + 4, table.begin() + 8, 0);
    const std::uint32_t entryCount = littleEndian32(table.data() + 8);
    if (crc32c(table.data(), table.size()) != checksum || entryCount > 2047U) {
      continue;
    }
    bool hasBat = false;
    bool hasMetadata = false;
    bool tableValid = true;
    for (std::uint32_t index = 0; index < entryCount; ++index) {
      const unsigned char* entry = table.data() + 16U + static_cast<std::size_t>(index) * 32U;
      const std::uint64_t offset = littleEndian64(entry + 16);
      const std::uint32_t length = littleEndian32(entry + 24);
      const bool required = (littleEndian32(entry + 28) & 1U) != 0;
      const bool isBat = std::equal(batRegion.begin(), batRegion.end(), entry);
      const bool isMetadata = std::equal(metadataRegion.begin(), metadataRegion.end(), entry);
      if (offset % (1024U * 1024U) != 0 || length == 0 || offset > fileSize ||
          length > fileSize - offset || (required && !isBat && !isMetadata)) {
        tableValid = false;
        break;
      }
      if (isBat) {
        hasBat = true;
        batOffset = offset;
        batLength = length;
      } else if (isMetadata) {
        hasMetadata = true;
        metadataOffset = offset;
        metadataLength = length;
      }
    }
    if (tableValid && hasBat && hasMetadata) {
      validRegions = true;
      break;
    }
  }
  if (!validRegions || metadataLength < 64U * 1024U) {
    return false;
  }

  std::array<unsigned char, 64U * 1024U> metadata{};
  if (!readAt(stream, metadataOffset, metadata) ||
      !startsWith(metadata.data(), metadata.size(), "metadata", 8)) {
    return false;
  }
  const std::uint16_t metadataEntries =
      static_cast<std::uint16_t>(metadata[10]) |
      (static_cast<std::uint16_t>(metadata[11]) << 8U);
  if (metadataEntries == 0 || metadataEntries > 2047U) {
    return false;
  }
  constexpr std::array<unsigned char, 16> virtualDiskSize{
      0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
      0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
  constexpr std::array<unsigned char, 16> fileParameters{
      0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
      0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
  constexpr std::array<unsigned char, 16> logicalSectorSize{
      0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
      0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
  bool hasVirtualSize = false;
  bool hasFileParameters = false;
  bool hasLogicalSectorSize = false;
  bool hasParent = false;
  std::uint32_t blockSize = 0;
  std::uint32_t sectorSize = 0;
  for (std::uint16_t index = 0; index < metadataEntries; ++index) {
    const unsigned char* entry = metadata.data() + 32U + static_cast<std::size_t>(index) * 32U;
    const std::uint32_t itemOffset = littleEndian32(entry + 16);
    const std::uint32_t itemLength = littleEndian32(entry + 20);
    if (itemOffset > metadataLength ||
        itemLength > metadataLength - itemOffset) {
      return false;
    }
    if (std::equal(virtualDiskSize.begin(), virtualDiskSize.end(), entry)) {
      std::array<unsigned char, 8> sizeBytes{};
      if (itemLength != sizeBytes.size() ||
          !readAt(stream, metadataOffset + itemOffset, sizeBytes) ||
          littleEndian64(sizeBytes.data()) == 0) {
        return false;
      }
      image.expandedSizeBytes = littleEndian64(sizeBytes.data());
      hasVirtualSize = true;
    } else if (std::equal(fileParameters.begin(), fileParameters.end(), entry)) {
      std::array<unsigned char, 8> parameters{};
      if (itemLength != parameters.size() ||
          !readAt(stream, metadataOffset + itemOffset, parameters)) {
        return false;
      }
      blockSize = littleEndian32(parameters.data());
      hasParent = (littleEndian32(parameters.data() + 4U) & 2U) != 0U;
      hasFileParameters = true;
    } else if (std::equal(logicalSectorSize.begin(), logicalSectorSize.end(), entry)) {
      std::array<unsigned char, 4> sizeBytes{};
      if (itemLength != sizeBytes.size() ||
          !readAt(stream, metadataOffset + itemOffset, sizeBytes)) {
        return false;
      }
      sectorSize = littleEndian32(sizeBytes.data());
      hasLogicalSectorSize = true;
    }
  }
  if (!hasVirtualSize) {
    return false;
  }
  image.format = ImageFormat::Vhdx;
  image.capabilities.validContainerMetadata = true;
  if (!activeLog && !hasParent && hasFileParameters && hasLogicalSectorSize &&
      blockSize >= 1024U * 1024U &&
      (blockSize & (blockSize - 1U)) == 0U &&
      (sectorSize == 512U || sectorSize == 4096U) &&
      image.expandedSizeBytes % sectorSize == 0U) {
    const std::uint64_t blockCount =
        (image.expandedSizeBytes - 1U) / blockSize + 1U;
    const std::uint64_t chunkRatio =
        (static_cast<std::uint64_t>(1U) << 23U) * sectorSize / blockSize;
    if (chunkRatio != 0U) {
      const std::uint64_t highestBatIndex =
          blockCount - 1U + (blockCount - 1U) / chunkRatio;
      const std::uint64_t requiredBatBytes = (highestBatIndex + 1U) * 8U;
      if (requiredBatBytes <= batLength && requiredBatBytes <=
                                               64ULL * 1024ULL * 1024ULL) {
        std::vector<unsigned char> bat(static_cast<std::size_t>(requiredBatBytes));
        if (!readAt(stream, batOffset, bat.data(), bat.size())) {
          return false;
        }
        bool batValid = true;
        std::vector<std::uint64_t> allocatedOffsets;
        for (std::uint64_t block = 0; block < blockCount; ++block) {
          const std::uint64_t batIndex = block + block / chunkRatio;
          const std::uint64_t entry =
              littleEndian64(bat.data() + static_cast<std::size_t>(batIndex) * 8U);
          const unsigned int state = static_cast<unsigned int>(entry & 7U);
          if (state == 0U || state == 2U || state == 3U) {
            continue;
          }
          const std::uint64_t payloadOffset = entry & 0xfffffffffff00000ULL;
          const bool overlapsMetadata =
              payloadOffset < metadataOffset + metadataLength &&
              metadataOffset < payloadOffset + blockSize;
          const bool overlapsBat = payloadOffset < batOffset + batLength &&
                                   batOffset < payloadOffset + blockSize;
          if (state != 6U || (entry & 0x00000000000ffff8ULL) != 0U ||
              payloadOffset < 1024U * 1024U || payloadOffset > fileSize ||
              blockSize > fileSize - payloadOffset || overlapsMetadata ||
              overlapsBat) {
            batValid = false;
            break;
          }
          allocatedOffsets.push_back(payloadOffset);
        }
        std::sort(allocatedOffsets.begin(), allocatedOffsets.end());
        if (std::adjacent_find(allocatedOffsets.begin(), allocatedOffsets.end()) !=
            allocatedOffsets.end()) {
          batValid = false;
        }
        if (batValid) {
          image.containerPayloadSizeBytes = image.expandedSizeBytes;
          image.containerAllocationTableOffsetBytes = batOffset;
          image.containerBlockSizeBytes = blockSize;
          image.containerLogicalSectorSize = sectorSize;
          image.containerPayloadLayout = ContainerPayloadLayout::DynamicVhdx;
        }
      }
    }
  }
  return true;
}

bool inspectFfu(std::ifstream& stream,
                const std::array<unsigned char, 512>& firstSector,
                const std::uint64_t fileSize, ImageInfo& image) {
  if (!startsWith(firstSector.data() + 4, firstSector.size() - 4, "SignedImage ", 12)) {
    return false;
  }
  const std::uint32_t securityHeaderSize = littleEndian32(firstSector.data());
  const std::uint64_t chunkBytes =
      static_cast<std::uint64_t>(littleEndian32(firstSector.data() + 16U)) *
      1024U;
  const std::uint64_t catalogBytes = littleEndian32(firstSector.data() + 24U);
  const std::uint64_t hashTableBytes = littleEndian32(firstSector.data() + 28U);
  if (securityHeaderSize < 32U || chunkBytes < 4096U ||
      chunkBytes > 16ULL * 1024ULL * 1024ULL ||
      (chunkBytes & (chunkBytes - 1U)) != 0U ||
      securityHeaderSize > fileSize ||
      catalogBytes > fileSize - securityHeaderSize ||
      hashTableBytes > fileSize - securityHeaderSize - catalogBytes) {
    return false;
  }
  const auto rounded = [](const std::uint64_t value,
                          const std::uint64_t alignment,
                          std::uint64_t& output) {
    if (value > std::numeric_limits<std::uint64_t>::max() -
                    (alignment - 1U)) {
      return false;
    }
    output = (value + alignment - 1U) / alignment * alignment;
    return true;
  };
  std::uint64_t securityBytes = 0;
  if (!rounded(static_cast<std::uint64_t>(securityHeaderSize) + catalogBytes +
                   hashTableBytes,
               chunkBytes, securityBytes) ||
      securityBytes > fileSize || fileSize - securityBytes < 28U) {
    return false;
  }
  std::array<unsigned char, 28> imageHeader{};
  if (!readAt(stream, securityBytes, imageHeader) ||
      !startsWith(imageHeader.data() + 4U, imageHeader.size() - 4U,
                  "ImageFlash ", 12)) {
    return false;
  }
  const std::uint64_t imageHeaderSize = littleEndian32(imageHeader.data());
  const std::uint64_t manifestBytes = littleEndian32(imageHeader.data() + 16U);
  if (imageHeaderSize < imageHeader.size() || imageHeaderSize > fileSize ||
      manifestBytes > fileSize - imageHeaderSize) {
    return false;
  }
  std::uint64_t imageBytes = 0;
  if (!rounded(imageHeaderSize + manifestBytes, chunkBytes, imageBytes) ||
      imageBytes > fileSize - securityBytes ||
      fileSize - securityBytes - imageBytes < 248U) {
    return false;
  }
  const std::uint64_t storeOffset = securityBytes + imageBytes;
  std::array<unsigned char, 248> storeHeader{};
  if (!readAt(stream, storeOffset, storeHeader)) {
    return false;
  }
  const std::uint32_t storeBlockBytes =
      littleEndian32(storeHeader.data() + 204U);
  const std::uint64_t writeDescriptorCount =
      littleEndian32(storeHeader.data() + 208U);
  const std::uint64_t writeDescriptorBytes =
      littleEndian32(storeHeader.data() + 212U);
  const std::uint64_t validateDescriptorBytes =
      littleEndian32(storeHeader.data() + 220U);
  if (storeBlockBytes == 0U || storeBlockBytes % 512U != 0U ||
      writeDescriptorCount == 0U || writeDescriptorCount > 16ULL * 1024ULL * 1024ULL ||
      writeDescriptorBytes < writeDescriptorCount * 8U ||
      writeDescriptorBytes > fileSize - storeOffset - storeHeader.size() ||
      validateDescriptorBytes > fileSize - storeOffset - storeHeader.size() -
                                    writeDescriptorBytes) {
    return false;
  }
  std::uint64_t storeBytes = 0;
  if (!rounded(storeHeader.size() + writeDescriptorBytes +
                   validateDescriptorBytes,
               chunkBytes, storeBytes) ||
      storeBytes > fileSize - storeOffset ||
      fileSize - storeOffset - storeBytes < chunkBytes) {
    return false;
  }
  image.format = ImageFormat::Ffu;
  image.capabilities.validContainerMetadata = true;
  return true;
}

ImageFormat formatFromExtension(const std::string& extension) {
  if (extension == ".iso") {
    return ImageFormat::Iso;
  }
  if (extension == ".img" || extension == ".raw" || extension == ".dd") {
    return ImageFormat::Raw;
  }
  if (extension == ".vhd") {
    return ImageFormat::Vhd;
  }
  if (extension == ".vhdx") {
    return ImageFormat::Vhdx;
  }
  if (extension == ".ffu") {
    return ImageFormat::Ffu;
  }
  if (extension == ".gz" || extension == ".gzip") {
    return ImageFormat::Gzip;
  }
  if (extension == ".bz2" || extension == ".bzip2") {
    return ImageFormat::Bzip2;
  }
  if (extension == ".zip") {
    return ImageFormat::Zip;
  }
  if (extension == ".xz") {
    return ImageFormat::Xz;
  }
  if (extension == ".lzma") {
    return ImageFormat::Lzma;
  }
  if (extension == ".zst" || extension == ".zstd") {
    return ImageFormat::Zstd;
  }
  return ImageFormat::Unknown;
}

bool isCompressed(const ImageFormat format) {
  return format == ImageFormat::Gzip || format == ImageFormat::Bzip2 ||
         format == ImageFormat::Zip || format == ImageFormat::Lzma ||
         format == ImageFormat::Xz || format == ImageFormat::Zstd;
}

}  // namespace

ImageAnalysisResult ImageAnalyzer::analyze(const std::filesystem::path& path) const {
  return analyze(path, path.filename().string(), {});
}

ImageAnalysisResult ImageAnalyzer::analyze(const std::filesystem::path& path,
                                           const std::string_view filenameHint) const {
  return analyze(path, filenameHint, {});
}

ImageAnalysisResult ImageAnalyzer::analyze(
    const std::filesystem::path& path, const std::string_view filenameHint,
    const ImageAnalysisCancelCallback& isCancelled) const {
  ImageAnalysisResult result;
  const auto cancelled = [&] {
    if (!isCancelled || !isCancelled()) {
      return false;
    }
    result.cancelled = true;
    result.error = "Image analysis was cancelled";
    return true;
  };
  if (cancelled()) {
    return result;
  }
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) {
    result.error = "Image file does not exist";
    return result;
  }
  if (!std::filesystem::is_regular_file(path, error) || error) {
    result.error = "Image path is not a regular file";
    return result;
  }

  const auto fileSize = std::filesystem::file_size(path, error);
  if (error) {
    result.error = "Unable to determine image size: " + error.message();
    return result;
  }
  if (fileSize == 0) {
    result.error = "Image file is empty";
    return result;
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    result.error = "Unable to open image for analysis";
    return result;
  }

  ImageInfo image;
  image.path = path.u8string();
  image.displayName =
      filenameHint.empty() ? path.filename().u8string() : std::string(filenameHint);
  image.sizeBytes = fileSize;

  std::array<unsigned char, 512> firstSector{};
  const std::size_t headerBytes =
      static_cast<std::size_t>(std::min<std::uint64_t>(fileSize, firstSector.size()));
  if (!readAt(stream, 0, firstSector.data(), headerBytes)) {
    result.error = "Unable to read the image header";
    return result;
  }

  inspectIso(stream, image);
  if (cancelled()) {
    return result;
  }
  auto udfContents = detail::readUdfContents(path);
  if (udfContents.valid) {
    image.format = ImageFormat::Iso;
  }
  if (image.format == ImageFormat::Unknown) {
    if (startsWith(firstSector.data(), firstSector.size(), "vhdxfile", 8)) {
      if (!inspectVhdx(stream, fileSize, image)) {
        result.warnings.emplace_back("VHDX identifier is present but both metadata headers are invalid");
      }
    } else if (firstSector[0] == 0x1f && firstSector[1] == 0x8b) {
      image.format = ImageFormat::Gzip;
    } else if (firstSector[0] == 'B' && firstSector[1] == 'Z' &&
               firstSector[2] == 'h') {
      image.format = ImageFormat::Bzip2;
    } else if (firstSector[0] == 'P' && firstSector[1] == 'K') {
      image.format = ImageFormat::Zip;
    } else {
      constexpr std::array<unsigned char, 6> xzSignature{0xfd, '7', 'z', 'X', 'Z', 0x00};
      constexpr std::array<unsigned char, 4> zstdSignature{0x28, 0xb5, 0x2f, 0xfd};
      if (std::equal(xzSignature.begin(), xzSignature.end(), firstSector.begin())) {
        image.format = ImageFormat::Xz;
      } else if (std::equal(zstdSignature.begin(), zstdSignature.end(), firstSector.begin())) {
        image.format = ImageFormat::Zstd;
      } else if (startsWith(firstSector.data() + 4, firstSector.size() - 4, "SignedImage ", 12)) {
        if (!inspectFfu(stream, firstSector, fileSize, image)) {
          result.warnings.emplace_back(
              "FFU signature is present but its security, image, or store metadata is invalid");
        }
      }
    }
  }

  if (image.format == ImageFormat::Unknown && fileSize >= 512) {
    std::array<unsigned char, 512> footer{};
    if (readAt(stream, fileSize - footer.size(), footer) &&
        startsWith(footer.data(), footer.size(), "conectix", 8) &&
        !inspectVhd(stream, fileSize, image)) {
      result.warnings.emplace_back("VHD footer signature is present but its checksum is invalid");
    }
  }

  if (image.format == ImageFormat::Unknown) {
    image.format = formatFromExtension(lowerExtension(image.displayName));
  }
  image.compressed = isCompressed(image.format);

  if ((image.format == ImageFormat::Vhd || image.format == ImageFormat::Vhdx ||
       image.format == ImageFormat::Ffu) &&
      !image.capabilities.validContainerMetadata) {
    result.warnings.emplace_back(
        "Container type was inferred from its filename; structural metadata did not validate");
  }

  if (image.compressed) {
    auto compressed = detail::inspectCompressedImage(path, image.format);
    if (compressed.valid) {
      const auto measured = detail::measureCompressedImage(path, image.format,
                                                            isCancelled);
      if (!measured.success) {
        if (isCancelled && isCancelled()) {
          result.cancelled = true;
          result.error = "Image analysis was cancelled";
          return result;
        }
        compressed.valid = false;
        compressed.sizeKnown = false;
        compressed.expandedSizeBytes = 0;
        compressed.warnings.emplace_back(
            "Compressed payload validation failed: " + measured.error);
      } else if (compressed.sizeKnown &&
                 compressed.expandedSizeBytes != measured.expandedSizeBytes) {
        compressed.valid = false;
        compressed.sizeKnown = false;
        compressed.expandedSizeBytes = 0;
        compressed.warnings.emplace_back(
            "Compressed payload size does not match its container metadata");
      } else {
        compressed.sizeKnown = true;
        compressed.expandedSizeBytes = measured.expandedSizeBytes;
        image.bootable = measured.prefixBytes >= 512U &&
                         measured.prefix[510] == 0x55U &&
                         measured.prefix[511] == 0xaaU;
      }
    }
    image.capabilities.validContainerMetadata = compressed.valid;
    image.capabilities.compressedSizeKnown =
        compressed.valid && compressed.sizeKnown;
    image.expandedSizeBytes = compressed.valid
                                  ? compressed.expandedSizeBytes
                                  : 0U;
    result.warnings.insert(result.warnings.end(), compressed.warnings.begin(),
                           compressed.warnings.end());
  }

  if (!image.compressed) {
    inspectPartitionTable(stream, fileSize, image, result.warnings);
  }

  std::vector<ImageContentEntry> imageContents;
  LinuxPersistenceInspection persistenceInspection;
  if (image.format == ImageFormat::Iso) {
    if (cancelled()) {
      return result;
    }
    auto isoContents = detail::readIso9660Contents(path);
    image.capabilities.iso9660 = isoContents.valid;
    image.capabilities.joliet = isoContents.joliet;
    image.capabilities.validBootCatalog = isoContents.bootCatalogValid;
    image.capabilities.biosBootable = isoContents.biosBootable;
    image.capabilities.uefiBootable = isoContents.uefiBootable;
    image.bootable = isoContents.biosBootable || isoContents.uefiBootable;
    if (!isoContents.volumeLabel.empty()) {
      image.volumeLabel = isoContents.volumeLabel;
    }
    image.capabilities.udf = udfContents.valid;
    if (image.volumeLabel.empty() && !udfContents.volumeLabel.empty()) {
      image.volumeLabel = udfContents.volumeLabel;
    }
    const bool preferUdf = udfContents.valid &&
                           (!isoContents.valid || udfContents.entries.size() > isoContents.entries.size());
    const auto& selectedFiles = preferUdf ? udfContents.files : isoContents.files;
    persistenceInspection =
        inspectLinuxPersistence(path, selectedFiles, isCancelled);
    if (persistenceInspection.cancelled) {
      result.cancelled = true;
      result.error = "Image analysis was cancelled";
      return result;
    }
    result.warnings.insert(
        result.warnings.end(),
        std::make_move_iterator(persistenceInspection.warnings.begin()),
        std::make_move_iterator(persistenceInspection.warnings.end()));
    const auto wim = detail::inspectWindowsImage(path, selectedFiles);
    image.capabilities.windowsImageMetadata = wim.valid;
    image.windowsImageCount = wim.imageCount;
    image.windowsBootIndex = wim.bootIndex;
    image.windowsVersionMajor = wim.versionMajor;
    image.windowsVersionMinor = wim.versionMinor;
    image.windowsBuild = wim.build;
    image.architecture = wim.architecture;
    image.windowsEditions = wim.editions;
    image.expandedSizeBytes = 0;
    for (const auto& entry : (preferUdf ? udfContents.entries : isoContents.entries)) {
      if (!entry.directory &&
          entry.sizeBytes <= std::numeric_limits<std::uint64_t>::max() -
                                 image.expandedSizeBytes) {
        image.expandedSizeBytes += entry.sizeBytes;
      }
    }
    result.warnings.insert(result.warnings.end(), wim.warnings.begin(), wim.warnings.end());
    imageContents = preferUdf ? std::move(udfContents.entries) : std::move(isoContents.entries);
    if (isoContents.valid || !udfContents.valid) {
      result.warnings.insert(result.warnings.end(),
                             std::make_move_iterator(isoContents.warnings.begin()),
                             std::make_move_iterator(isoContents.warnings.end()));
    }
    result.warnings.insert(result.warnings.end(),
                           std::make_move_iterator(udfContents.warnings.begin()),
                           std::make_move_iterator(udfContents.warnings.end()));
  }
  ImageProfileResolver::apply(imageContents, image,
                              persistenceInspection.style);
  if (image.format == ImageFormat::Iso && image.capabilities.isoExtraction &&
      !image.capabilities.standardWindowsInstallation &&
      image.capabilities.biosBootable && !image.capabilities.uefiBootable) {
    result.warnings.emplace_back(
        image.capabilities.rawWrite
            ? "Legacy-BIOS deployment for this non-Windows image is available only in DD mode"
            : "This non-Windows image is BIOS-only and is not safely deployable without a matching Syslinux or GRUB installer");
  }
  if (image.format == ImageFormat::Vhd &&
      image.capabilities.validContainerMetadata &&
      image.containerPayloadSizeBytes == 0U) {
    result.warnings.emplace_back(
        "Dynamic and differencing VHD containers are recognized but require conversion to a fixed VHD or raw image before deployment");
  } else if (((image.format == ImageFormat::Vhdx &&
               image.containerPayloadLayout == ContainerPayloadLayout::None) ||
              image.format == ImageFormat::Ffu) &&
             image.capabilities.validContainerMetadata) {
    result.warnings.emplace_back(
        image.format == ImageFormat::Ffu
            ? "FFU is a provider-applied container; Windows can deploy it through DISM when that backend is available"
            : "This container is recognized for diagnostics but does not expose a directly deployable raw payload");
  }
  if (image.format == ImageFormat::Unknown) {
    result.warnings.emplace_back("Image type is unknown; raw writing must remain disabled");
  }
  result.image = std::move(image);
  return result;
}

}  // namespace rufus::core
