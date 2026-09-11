/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "udf_reader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rufus::core::detail {

namespace {

constexpr std::uint64_t kBlockSize = 2048;
constexpr std::size_t kMaximumEntries = 100000;
constexpr std::size_t kMaximumDepth = 32;
constexpr std::uint64_t kMaximumDirectoryBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumDescriptorBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kExtentLengthMask = 0x3fffffffU;

std::uint16_t littleEndian16(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8U);
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

bool readAt(std::ifstream& stream, const std::uint64_t fileSize,
            const std::uint64_t offset, unsigned char* data, const std::size_t size) {
  if (offset > fileSize || size > fileSize - offset ||
      offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
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

std::uint16_t crc16(const unsigned char* data, const std::size_t size) {
  std::uint16_t crc = 0;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= static_cast<std::uint16_t>(data[index]) << 8U;
    for (unsigned int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0 ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                                 : static_cast<std::uint16_t>(crc << 1U);
    }
  }
  return crc;
}

bool validTag(const unsigned char* descriptor, const std::size_t available,
              const std::uint16_t expectedId) {
  if (available < 16 || littleEndian16(descriptor) != expectedId) {
    return false;
  }
  unsigned int checksum = 0;
  for (std::size_t index = 0; index < 16; ++index) {
    if (index != 4) {
      checksum += descriptor[index];
    }
  }
  const std::uint16_t crcLength = littleEndian16(descriptor + 10);
  return static_cast<unsigned char>(checksum) == descriptor[4] &&
         crcLength <= available - 16 &&
         (crcLength == 0 || crc16(descriptor + 16, crcLength) == littleEndian16(descriptor + 8));
}

void appendUtf8(std::string& output, const std::uint32_t codePoint) {
  if (codePoint <= 0x7fU) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else if (codePoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  }
}

std::string decodeCompressedUnicode(const unsigned char* data, const std::size_t size) {
  if (size < 1) {
    return {};
  }
  std::string output;
  if (data[0] == 8) {
    output.assign(reinterpret_cast<const char*>(data + 1), size - 1U);
  } else if (data[0] == 16) {
    for (std::size_t index = 1; index + 1U < size; index += 2U) {
      appendUtf8(output, (static_cast<std::uint16_t>(data[index]) << 8U) | data[index + 1U]);
    }
  }
  return output;
}

std::string decodeDString(const unsigned char* data, const std::size_t fieldSize) {
  if (fieldSize < 2) {
    return {};
  }
  const std::size_t encodedSize = data[fieldSize - 1U];
  if (encodedSize == 0 || encodedSize > fieldSize - 1U) {
    return {};
  }
  return decodeCompressedUnicode(data, encodedSize);
}

struct Partition final {
  std::uint32_t start{};
  std::uint32_t length{};
};

struct LogicalVolume final {
  std::uint32_t fileSetLba{};
  std::uint16_t fileSetPartitionReference{};
  std::map<std::uint16_t, std::uint16_t> referenceToPartition;
  std::string label;
  bool present{};
};

struct Allocation final {
  std::vector<ImageFileExtent> extents;
  std::vector<unsigned char> embedded;
  bool readable{true};
};

struct FileEntry final {
  std::uint64_t informationLength{};
  unsigned char type{};
  Allocation allocation;
};

bool resolvePartition(const std::map<std::uint16_t, Partition>& partitions,
                      const LogicalVolume& volume, const std::uint16_t reference,
                      Partition& partition) {
  const auto mapping = volume.referenceToPartition.find(reference);
  if (mapping == volume.referenceToPartition.end()) {
    return false;
  }
  const auto found = partitions.find(mapping->second);
  if (found == partitions.end()) {
    return false;
  }
  partition = found->second;
  return true;
}

bool extentInImage(const std::uint64_t offset, const std::uint64_t length,
                   const std::uint64_t fileSize) {
  return offset <= fileSize && length <= fileSize - offset;
}

bool parseFileEntry(std::ifstream& stream, const std::uint64_t fileSize,
                    const std::map<std::uint16_t, Partition>& partitions,
                    const LogicalVolume& volume, const std::uint16_t partitionReference,
                    const std::uint32_t relativeLba, FileEntry& entry) {
  Partition containingPartition;
  if (!resolvePartition(partitions, volume, partitionReference, containingPartition) ||
      relativeLba >= containingPartition.length) {
    return false;
  }
  const std::uint64_t absoluteLba =
      static_cast<std::uint64_t>(containingPartition.start) + relativeLba;
  std::array<unsigned char, kBlockSize> bytes{};
  if (!readAt(stream, fileSize, absoluteLba * kBlockSize, bytes.data(), bytes.size())) {
    return false;
  }
  const std::uint16_t tagId = littleEndian16(bytes.data());
  const bool extended = tagId == 266;
  if ((!extended && tagId != 261) || !validTag(bytes.data(), bytes.size(), tagId)) {
    return false;
  }
  entry.type = bytes[27];
  entry.informationLength = littleEndian64(bytes.data() + 56);
  const std::uint16_t allocationType = littleEndian16(bytes.data() + 34) & 0x0007U;
  const std::size_t attributesOffset = extended ? 208U : 168U;
  const std::size_t allocationLengthOffset = extended ? 212U : 172U;
  const std::size_t dataOffset = extended ? 216U : 176U;
  const std::uint32_t attributeLength = littleEndian32(bytes.data() + attributesOffset);
  const std::uint32_t allocationLength = littleEndian32(bytes.data() + allocationLengthOffset);
  if (attributeLength > bytes.size() - dataOffset ||
      allocationLength > bytes.size() - dataOffset - attributeLength) {
    return false;
  }
  const unsigned char* allocation = bytes.data() + dataOffset + attributeLength;

  if (allocationType == 3) {
    if (entry.informationLength > allocationLength) {
      return false;
    }
    entry.allocation.embedded.assign(allocation,
                                     allocation + static_cast<std::size_t>(entry.informationLength));
    return true;
  }
  const std::size_t descriptorSize = allocationType == 0 ? 8U : allocationType == 1 ? 16U : 0U;
  if (descriptorSize == 0 || allocationLength % descriptorSize != 0) {
    return false;
  }
  std::uint64_t fileOffset = 0;
  for (std::size_t offset = 0; offset < allocationLength; offset += descriptorSize) {
    const std::uint32_t encodedLength = littleEndian32(allocation + offset);
    const std::uint32_t extentType = encodedLength >> 30U;
    const std::uint32_t length = encodedLength & kExtentLengthMask;
    if (length == 0) {
      continue;
    }
    if (extentType != 0) {
      entry.allocation.readable = false;
      fileOffset += length;
      continue;
    }
    const std::uint32_t lba = littleEndian32(allocation + offset + 4);
    const std::uint16_t reference = allocationType == 0
                                        ? partitionReference
                                        : littleEndian16(allocation + offset + 8);
    Partition extentPartition;
    if (!resolvePartition(partitions, volume, reference, extentPartition) ||
        lba >= extentPartition.length) {
      return false;
    }
    const std::uint64_t imageOffset =
        (static_cast<std::uint64_t>(extentPartition.start) + lba) * kBlockSize;
    if (!extentInImage(imageOffset, length, fileSize)) {
      return false;
    }
    entry.allocation.extents.push_back({imageOffset, fileOffset, length});
    fileOffset += length;
  }
  return fileOffset >= entry.informationLength;
}

bool materializeData(std::ifstream& stream, const std::uint64_t fileSize,
                     const FileEntry& entry, std::vector<unsigned char>& data) {
  if (!entry.allocation.readable || entry.informationLength > kMaximumDirectoryBytes ||
      entry.informationLength > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  if (!entry.allocation.embedded.empty() || entry.informationLength == 0) {
    data = entry.allocation.embedded;
    return data.size() == entry.informationLength;
  }
  data.assign(static_cast<std::size_t>(entry.informationLength), 0);
  std::size_t completed = 0;
  for (const auto& extent : entry.allocation.extents) {
    const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        extent.length, entry.informationLength - completed));
    if (!readAt(stream, fileSize, extent.imageOffset, data.data() + completed, amount)) {
      return false;
    }
    completed += amount;
    if (completed == data.size()) {
      return true;
    }
  }
  return completed == data.size();
}

struct PendingDirectory final {
  std::uint16_t partitionReference{};
  std::uint32_t lba{};
  std::string path;
  std::size_t depth{};
};

void walkDirectories(std::ifstream& stream, const std::uint64_t fileSize,
                     const std::map<std::uint16_t, Partition>& partitions,
                     const LogicalVolume& volume, const PendingDirectory& root,
                     UdfReadResult& result) {
  std::deque<PendingDirectory> pending;
  pending.push_back(root);
  std::set<std::pair<std::uint16_t, std::uint32_t>> visited;
  while (!pending.empty()) {
    const PendingDirectory directory = std::move(pending.front());
    pending.pop_front();
    if (!visited.emplace(directory.partitionReference, directory.lba).second) {
      continue;
    }
    if (directory.depth > kMaximumDepth) {
      result.warnings.emplace_back("UDF directory depth exceeds the analysis safety limit");
      continue;
    }
    FileEntry directoryEntry;
    if (!parseFileEntry(stream, fileSize, partitions, volume,
                        directory.partitionReference, directory.lba, directoryEntry) ||
        directoryEntry.type != 4) {
      result.warnings.emplace_back("UDF directory has an invalid file entry");
      continue;
    }
    std::vector<unsigned char> data;
    if (!materializeData(stream, fileSize, directoryEntry, data)) {
      result.warnings.emplace_back("UDF directory data is not readable");
      continue;
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
      if (data.size() - offset < 38) {
        result.warnings.emplace_back("UDF contains a malformed file identifier");
        break;
      }
      const unsigned char* identifier = data.data() + offset;
      const unsigned char characteristics = identifier[18];
      const std::size_t nameLength = identifier[19];
      const std::size_t implementationUseLength = littleEndian16(identifier + 36);
      const std::size_t recordLength =
          (38U + implementationUseLength + nameLength + 3U) & ~std::size_t{3U};
      if (recordLength > data.size() - offset ||
          implementationUseLength + nameLength > recordLength - 38U ||
          !validTag(identifier, recordLength, 257)) {
        result.warnings.emplace_back("UDF file identifier exceeds its directory data");
        break;
      }
      if ((characteristics & (0x04U | 0x08U)) == 0) {
        const std::string name = decodeCompressedUnicode(
            identifier + 38U + implementationUseLength, nameLength);
        if (!name.empty()) {
          const std::uint32_t extentLength = littleEndian32(identifier + 20) & kExtentLengthMask;
          const std::uint32_t lba = littleEndian32(identifier + 24);
          const std::uint16_t reference = littleEndian16(identifier + 28);
          if (extentLength == 0) {
            result.warnings.emplace_back("UDF file identifier has an empty ICB extent");
          } else {
            FileEntry child;
            if (!parseFileEntry(stream, fileSize, partitions, volume, reference, lba, child)) {
              result.warnings.emplace_back("UDF child has an invalid file entry: " + name);
            } else {
              const bool isDirectory = (characteristics & 0x02U) != 0 || child.type == 4;
              const std::string fullPath =
                  directory.path.empty() ? name : directory.path + '/' + name;
              result.entries.push_back({fullPath, child.informationLength, isDirectory});
              if (isDirectory) {
                pending.push_back({reference, lba, fullPath, directory.depth + 1U});
              } else if (child.allocation.readable) {
                result.files.push_back({fullPath, child.informationLength,
                                        std::move(child.allocation.extents),
                                        std::move(child.allocation.embedded), {}});
              }
              if (result.entries.size() >= kMaximumEntries) {
                result.warnings.emplace_back("UDF entry count exceeds the analysis safety limit");
                return;
              }
            }
          }
        }
      }
      offset += recordLength;
    }
  }
}

bool hasUdfRecognitionSequence(std::ifstream& stream, const std::uint64_t fileSize) {
  bool began = false;
  bool foundNsr = false;
  std::array<unsigned char, kBlockSize> block{};
  for (std::uint64_t sector = 16; sector < 32; ++sector) {
    if (!readAt(stream, fileSize, sector * kBlockSize, block.data(), block.size())) {
      break;
    }
    const std::string identifier(reinterpret_cast<const char*>(block.data() + 1), 5);
    if (identifier == "BEA01") {
      began = true;
    } else if (began && (identifier == "NSR02" || identifier == "NSR03")) {
      foundNsr = true;
    } else if (identifier == "TEA01") {
      return began && foundNsr;
    }
  }
  return false;
}

}  // namespace

UdfReadResult readUdfContents(const std::filesystem::path& path) {
  UdfReadResult result;
  std::error_code error;
  const std::uint64_t fileSize = std::filesystem::file_size(path, error);
  if (error || fileSize < 257U * kBlockSize) {
    return result;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream || !hasUdfRecognitionSequence(stream, fileSize)) {
    return result;
  }

  std::array<unsigned char, kBlockSize> block{};
  if (!readAt(stream, fileSize, 256U * kBlockSize, block.data(), block.size()) ||
      !validTag(block.data(), block.size(), 2)) {
    result.warnings.emplace_back("UDF recognition sequence has no valid anchor descriptor");
    return result;
  }
  const std::uint32_t sequenceLength = littleEndian32(block.data() + 16);
  const std::uint32_t sequenceLba = littleEndian32(block.data() + 20);
  if (sequenceLength == 0 || sequenceLength > kMaximumDescriptorBytes ||
      sequenceLba >= fileSize / kBlockSize ||
      sequenceLength > fileSize - static_cast<std::uint64_t>(sequenceLba) * kBlockSize) {
    result.warnings.emplace_back("UDF main volume descriptor sequence is outside the image");
    return result;
  }

  std::map<std::uint16_t, Partition> partitions;
  LogicalVolume volume;
  const std::uint64_t descriptorCount = (sequenceLength + kBlockSize - 1U) / kBlockSize;
  for (std::uint64_t index = 0; index < descriptorCount; ++index) {
    if (!readAt(stream, fileSize, (static_cast<std::uint64_t>(sequenceLba) + index) * kBlockSize,
                block.data(), block.size())) {
      break;
    }
    const std::uint16_t tag = littleEndian16(block.data());
    if (tag == 8) {
      break;
    }
    if (!validTag(block.data(), block.size(), tag)) {
      continue;
    }
    if (tag == 5) {
      const std::uint16_t number = littleEndian16(block.data() + 22);
      const std::uint32_t start = littleEndian32(block.data() + 188);
      const std::uint32_t length = littleEndian32(block.data() + 192);
      if (start < fileSize / kBlockSize && length <= fileSize / kBlockSize - start) {
        partitions[number] = {start, length};
      }
    } else if (tag == 6 && littleEndian32(block.data() + 212) == kBlockSize) {
      const std::uint32_t mapLength = littleEndian32(block.data() + 264);
      const std::uint32_t mapCount = littleEndian32(block.data() + 268);
      if (mapLength > block.size() - 440U || mapCount > 256U) {
        continue;
      }
      LogicalVolume candidate;
      candidate.fileSetLba = littleEndian32(block.data() + 252);
      candidate.fileSetPartitionReference = littleEndian16(block.data() + 256);
      candidate.label = decodeDString(block.data() + 84, 128);
      std::size_t mapOffset = 440;
      for (std::uint32_t mapIndex = 0;
           mapIndex < mapCount && mapOffset + 2U <= 440U + mapLength; ++mapIndex) {
        const unsigned char type = block[mapOffset];
        const std::size_t length = block[mapOffset + 1U];
        if (length < 2U || length > 440U + mapLength - mapOffset) {
          candidate.referenceToPartition.clear();
          break;
        }
        if (type == 1 && length == 6U) {
          candidate.referenceToPartition[static_cast<std::uint16_t>(mapIndex)] =
              littleEndian16(block.data() + mapOffset + 4U);
        }
        mapOffset += length;
      }
      candidate.present = !candidate.referenceToPartition.empty();
      if (candidate.present) {
        volume = std::move(candidate);
      }
    }
  }

  Partition fileSetPartition;
  if (!volume.present ||
      !resolvePartition(partitions, volume, volume.fileSetPartitionReference,
                        fileSetPartition) ||
      volume.fileSetLba >= fileSetPartition.length ||
      !readAt(stream, fileSize,
              (static_cast<std::uint64_t>(fileSetPartition.start) + volume.fileSetLba) * kBlockSize,
              block.data(), block.size()) ||
      !validTag(block.data(), block.size(), 256)) {
    result.warnings.emplace_back("UDF volume descriptors do not resolve to a valid file set");
    return result;
  }
  const std::uint32_t rootLba = littleEndian32(block.data() + 404);
  const std::uint16_t rootReference = littleEndian16(block.data() + 408);
  FileEntry root;
  if (!parseFileEntry(stream, fileSize, partitions, volume, rootReference, rootLba, root) ||
      root.type != 4) {
    result.warnings.emplace_back("UDF file set has no valid root directory");
    return result;
  }

  result.valid = true;
  result.volumeLabel = volume.label;
  walkDirectories(stream, fileSize, partitions, volume,
                  {rootReference, rootLba, {}, 0}, result);
  return result;
}

}  // namespace rufus::core::detail
