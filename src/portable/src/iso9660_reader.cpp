/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "iso9660_reader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rufus::core::detail {

namespace {

constexpr std::uint64_t kSectorSize = 2048;
constexpr std::uint64_t kDescriptorStart = 16;
constexpr std::uint64_t kDescriptorLimit = 64;
constexpr std::size_t kMaximumEntries = 100000;
constexpr std::size_t kMaximumDepth = 32;
constexpr std::uint64_t kMaximumDirectoryBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumTotalDirectoryBytes = 128ULL * 1024ULL * 1024ULL;

enum class NameEncoding {
  Iso9660,
  Joliet,
};

struct Directory final {
  std::uint32_t extent{};
  std::uint32_t size{};
  std::string path;
  std::size_t depth{};
};

struct VolumeRoot final {
  Directory directory;
  std::string label;
  bool present{};
};

template <std::size_t Size>
bool readAt(std::ifstream& stream, const std::uint64_t offset,
            std::array<unsigned char, Size>& data) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    return false;
  }
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

std::uint16_t littleEndian16(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint16_t bigEndian16(const unsigned char* data) {
  return (static_cast<std::uint16_t>(data[0]) << 8U) |
         static_cast<std::uint16_t>(data[1]);
}

std::uint32_t littleEndian32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint32_t bigEndian32(const unsigned char* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) |
         static_cast<std::uint32_t>(data[3]);
}

bool bothEndian16(const unsigned char* data, std::uint16_t& value) {
  value = littleEndian16(data);
  return value == bigEndian16(data + 2);
}

bool bothEndian32(const unsigned char* data, std::uint32_t& value) {
  value = littleEndian32(data);
  return value == bigEndian32(data + 4);
}

bool isDescriptor(const std::array<unsigned char, kSectorSize>& descriptor) {
  constexpr std::array<unsigned char, 5> identifier{'C', 'D', '0', '0', '1'};
  return std::equal(identifier.begin(), identifier.end(), descriptor.begin() + 1) &&
         descriptor[6] == 1;
}

bool isJolietDescriptor(const std::array<unsigned char, kSectorSize>& descriptor) {
  return descriptor[0] == 2 && descriptor[88] == '%' && descriptor[89] == '/' &&
         (descriptor[90] == '@' || descriptor[90] == 'C' || descriptor[90] == 'E');
}

bool extentIsReadable(const std::uint32_t extent, const std::uint32_t size,
                      const std::uint64_t fileSize) {
  const std::uint64_t offset = static_cast<std::uint64_t>(extent) * kSectorSize;
  return offset <= fileSize && static_cast<std::uint64_t>(size) <= fileSize - offset;
}

void appendUtf8(std::string& output, const std::uint32_t codePoint) {
  if (codePoint <= 0x7fU) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else if (codePoint <= 0xffffU && !(codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
    output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else {
    appendUtf8(output, 0xfffdU);
  }
}

std::string jolietText(const unsigned char* data, const std::size_t size) {
  std::string result;
  result.reserve(size / 2U);
  for (std::size_t index = 0; index + 1U < size; index += 2U) {
    const std::uint16_t value = bigEndian16(data + index);
    if (value == 0) {
      break;
    }
    appendUtf8(result, value);
  }
  while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
    result.pop_back();
  }
  return result;
}

std::string isoText(const unsigned char* data, const std::size_t size) {
  std::string result(reinterpret_cast<const char*>(data), size);
  while (!result.empty() && (result.back() == ' ' || result.back() == '\0')) {
    result.pop_back();
  }
  const auto first = result.find_first_not_of(' ');
  return first == std::string::npos ? std::string{} : result.substr(first);
}

std::string entryName(const unsigned char* data, const std::size_t size,
                      const NameEncoding encoding) {
  std::string name = encoding == NameEncoding::Joliet ? jolietText(data, size)
                                                       : isoText(data, size);
  const auto version = name.find(';');
  if (version != std::string::npos) {
    name.erase(version);
  }
  while (!name.empty() && name.back() == '.') {
    name.pop_back();
  }
  return name;
}

std::string rockRidgeName(const unsigned char* record, const std::size_t recordLength,
                          const std::size_t nameLength) {
  std::size_t offset = 33U + nameLength + (nameLength % 2U == 0 ? 1U : 0U);
  std::string name;
  while (offset + 4U <= recordLength) {
    const unsigned char* entry = record + offset;
    const std::size_t length = entry[2];
    if (length < 4U || length > recordLength - offset || entry[3] != 1U) {
      break;
    }
    if (entry[0] == 'N' && entry[1] == 'M' && length >= 5U) {
      const unsigned char flags = entry[4];
      if ((flags & 0x06U) != 0) {
        return {};
      }
      name.append(reinterpret_cast<const char*>(entry + 5U), length - 5U);
      if ((flags & 0x01U) == 0) {
        return name;
      }
    }
    offset += length;
  }
  return {};
}

bool descriptorRoot(const std::array<unsigned char, kSectorSize>& descriptor,
                    VolumeRoot& root, const NameEncoding encoding) {
  constexpr std::size_t rootOffset = 156;
  constexpr std::size_t minimumRecordSize = 34;
  if (descriptor[rootOffset] < minimumRecordSize) {
    return false;
  }
  std::uint16_t logicalBlockSize = 0;
  if (!bothEndian16(descriptor.data() + 128, logicalBlockSize) ||
      logicalBlockSize != kSectorSize ||
      !bothEndian32(descriptor.data() + rootOffset + 2, root.directory.extent) ||
      !bothEndian32(descriptor.data() + rootOffset + 10, root.directory.size)) {
    return false;
  }
  root.label = encoding == NameEncoding::Joliet
                   ? jolietText(descriptor.data() + 40, 32)
                   : isoText(descriptor.data() + 40, 32);
  root.present = true;
  return true;
}

void inspectBootEntry(const unsigned char* entry, const unsigned char platform,
                      const std::uint64_t fileSize, Iso9660ReadResult& result) {
  if (entry[0] != 0x88U) {
    return;
  }
  const std::uint32_t imageLba = littleEndian32(entry + 8);
  if (imageLba == 0 || static_cast<std::uint64_t>(imageLba) * kSectorSize >= fileSize) {
    result.warnings.emplace_back("El Torito boot entry points outside the image");
    return;
  }
  if (platform == 0xefU) {
    result.uefiBootable = true;
  } else if (platform == 0x00U) {
    result.biosBootable = true;
  }
}

void inspectBootCatalog(std::ifstream& stream, const std::uint64_t fileSize,
                        const std::uint32_t catalogLba, Iso9660ReadResult& result) {
  result.bootCatalogPresent = true;
  std::array<unsigned char, kSectorSize> catalog{};
  if (catalogLba == 0 || !readAt(stream, static_cast<std::uint64_t>(catalogLba) * kSectorSize,
                                 catalog)) {
    result.warnings.emplace_back("El Torito boot catalog is outside the image");
    return;
  }

  std::uint32_t checksum = 0;
  for (std::size_t index = 0; index < 32; index += 2) {
    checksum += littleEndian16(catalog.data() + index);
  }
  if (catalog[0] != 1 || catalog[30] != 0x55U || catalog[31] != 0xaaU ||
      (checksum & 0xffffU) != 0) {
    result.warnings.emplace_back("El Torito validation entry is malformed");
    return;
  }

  result.bootCatalogValid = true;
  inspectBootEntry(catalog.data() + 32, catalog[1], fileSize, result);
  std::size_t offset = 64;
  while (offset + 32 <= catalog.size()) {
    const unsigned char header = catalog[offset];
    if (header == 0x00U) {
      break;
    }
    if (header != 0x90U && header != 0x91U) {
      result.warnings.emplace_back("El Torito catalog contains an unknown section header");
      break;
    }
    const unsigned char platform = catalog[offset + 1];
    const std::uint16_t count = littleEndian16(catalog.data() + offset + 2);
    offset += 32;
    if (count > (catalog.size() - offset) / 32U) {
      result.warnings.emplace_back("El Torito catalog section exceeds its containing sector");
      break;
    }
    for (std::uint16_t index = 0; index < count; ++index, offset += 32) {
      inspectBootEntry(catalog.data() + offset, platform, fileSize, result);
    }
    if (header == 0x91U) {
      break;
    }
  }
}

void readDirectoryTree(std::ifstream& stream, const std::uint64_t fileSize,
                       const Directory& root, const NameEncoding encoding,
                       Iso9660ReadResult& result) {
  std::deque<Directory> pending;
  pending.push_back(root);
  std::set<std::pair<std::uint32_t, std::uint32_t>> visited;
  std::set<std::string> directoryPaths;
  std::unordered_map<std::string, std::size_t> entryIndexes;
  std::unordered_map<std::string, std::size_t> fileIndexes;
  std::uint64_t totalDirectoryBytes = 0;

  while (!pending.empty()) {
    Directory directory = std::move(pending.front());
    pending.pop_front();
    if (!visited.emplace(directory.extent, directory.size).second) {
      continue;
    }
    if (directory.depth > kMaximumDepth) {
      result.warnings.emplace_back("ISO directory depth exceeds the analysis safety limit");
      continue;
    }
    if (directory.size > kMaximumDirectoryBytes ||
        totalDirectoryBytes > kMaximumTotalDirectoryBytes - directory.size) {
      result.warnings.emplace_back("ISO directory data exceeds the analysis safety limit");
      break;
    }
    if (!extentIsReadable(directory.extent, directory.size, fileSize)) {
      result.warnings.emplace_back("ISO directory entry points outside the image");
      continue;
    }

    totalDirectoryBytes += directory.size;
    std::vector<unsigned char> bytes(directory.size);
    if (!readAt(stream, static_cast<std::uint64_t>(directory.extent) * kSectorSize,
                bytes.data(), bytes.size())) {
      result.warnings.emplace_back("Unable to read an ISO directory");
      continue;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t recordLength = bytes[offset];
      if (recordLength == 0) {
        const std::size_t nextSector = ((offset / kSectorSize) + 1U) * kSectorSize;
        if (nextSector <= offset) {
          break;
        }
        offset = std::min(nextSector, bytes.size());
        continue;
      }
      if (recordLength < 34 || recordLength > bytes.size() - offset) {
        result.warnings.emplace_back("ISO contains a malformed directory record");
        break;
      }
      if (recordLength > kSectorSize - (offset % kSectorSize)) {
        result.warnings.emplace_back("ISO directory record crosses a logical-sector boundary");
        break;
      }

      const unsigned char* record = bytes.data() + offset;
      const std::size_t nameLength = record[32];
      if (nameLength == 0 || 33U + nameLength > recordLength) {
        result.warnings.emplace_back("ISO contains a malformed file identifier");
        break;
      }

      const bool dotEntry = nameLength == 1 && (record[33] == 0 || record[33] == 1);
      if (!dotEntry) {
        std::string name = entryName(record + 33, nameLength, encoding);
        if (encoding == NameEncoding::Iso9660) {
          const std::string alternate = rockRidgeName(record, recordLength, nameLength);
          if (!alternate.empty()) {
            name = alternate;
          }
        }
        if (!name.empty()) {
          const bool isDirectory = (record[25] & 0x02U) != 0;
          std::uint32_t extent = 0;
          std::uint32_t size = 0;
          if (!bothEndian32(record + 2, extent) || !bothEndian32(record + 10, size)) {
            result.warnings.emplace_back("ISO directory record has inconsistent byte order");
            offset += recordLength;
            continue;
          }
          const std::string fullPath = directory.path.empty() ? name : directory.path + '/' + name;
          if (!extentIsReadable(extent, size, fileSize)) {
            result.warnings.emplace_back("ISO file extent points outside the image: " + fullPath);
            offset += recordLength;
            continue;
          }

          if (isDirectory) {
            if (directoryPaths.insert(fullPath).second) {
              result.entries.push_back({fullPath, size, true});
            }
            if (size != 0) {
              pending.push_back({extent, size, fullPath, directory.depth + 1U});
            }
          } else {
            auto file = fileIndexes.find(fullPath);
            if (file == fileIndexes.end()) {
              const std::size_t index = result.files.size();
              fileIndexes.emplace(fullPath, index);
              entryIndexes.emplace(fullPath, result.entries.size());
              result.entries.push_back({fullPath, size, false});
              result.files.push_back(
                  {fullPath, size,
                   {{static_cast<std::uint64_t>(extent) * kSectorSize, 0, size}}, {}, {}});
            } else {
              auto& existing = result.files[file->second];
              existing.extents.push_back(
                  {static_cast<std::uint64_t>(extent) * kSectorSize,
                   existing.sizeBytes, size});
              existing.sizeBytes += size;
              result.entries[entryIndexes.at(fullPath)].sizeBytes = existing.sizeBytes;
            }
          }
          if (result.entries.size() >= kMaximumEntries) {
            result.warnings.emplace_back("ISO entry count exceeds the analysis safety limit");
            return;
          }
        }
      }
      offset += recordLength;
    }
  }
}

}  // namespace

Iso9660ReadResult readIso9660Contents(const std::filesystem::path& path) {
  Iso9660ReadResult result;
  std::error_code error;
  const std::uint64_t fileSize = std::filesystem::file_size(path, error);
  if (error) {
    result.warnings.emplace_back("Unable to determine ISO size while reading its directory tree");
    return result;
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    result.warnings.emplace_back("Unable to open the ISO directory tree");
    return result;
  }

  VolumeRoot primary;
  VolumeRoot joliet;
  std::uint32_t bootCatalogLba = 0;
  std::array<unsigned char, kSectorSize> descriptor{};
  for (std::uint64_t sector = kDescriptorStart; sector < kDescriptorLimit; ++sector) {
    if (!readAt(stream, sector * kSectorSize, descriptor) || !isDescriptor(descriptor)) {
      break;
    }
    if (descriptor[0] == 0) {
      constexpr char elTorito[] = "EL TORITO SPECIFICATION";
      if (std::equal(std::begin(elTorito), std::end(elTorito) - 1, descriptor.begin() + 7)) {
        bootCatalogLba = littleEndian32(descriptor.data() + 71);
      }
    } else if (descriptor[0] == 1) {
      if (!descriptorRoot(descriptor, primary, NameEncoding::Iso9660)) {
        result.warnings.emplace_back("ISO primary volume descriptor is inconsistent");
      }
    } else if (isJolietDescriptor(descriptor)) {
      if (!descriptorRoot(descriptor, joliet, NameEncoding::Joliet)) {
        result.warnings.emplace_back("ISO Joliet volume descriptor is inconsistent");
      }
    } else if (descriptor[0] == 255) {
      break;
    }
  }

  const VolumeRoot& selected = joliet.present ? joliet : primary;
  if (!selected.present) {
    result.warnings.emplace_back("ISO has no supported ISO-9660 or Joliet volume descriptor");
    return result;
  }
  if (selected.directory.size == 0 ||
      !extentIsReadable(selected.directory.extent, selected.directory.size, fileSize)) {
    result.warnings.emplace_back("ISO root directory points outside the image");
    return result;
  }

  result.valid = true;
  result.joliet = joliet.present;
  result.volumeLabel = selected.label;
  if (bootCatalogLba != 0) {
    inspectBootCatalog(stream, fileSize, bootCatalogLba, result);
  }
  readDirectoryTree(stream, fileSize, selected.directory,
                    joliet.present ? NameEncoding::Joliet : NameEncoding::Iso9660,
                    result);
  return result;
}

bool readImageFile(std::ifstream& image, const ImageFileRecord& file,
                   const std::uint64_t offset, unsigned char* data,
                   const std::size_t size, std::string& error) {
  if (data == nullptr || offset > file.sizeBytes || size > file.sizeBytes - offset) {
    error = "Requested file data is outside the optical image entry";
    return false;
  }
  if (!file.externalPath.empty()) {
    std::error_code fileError;
    const std::uint64_t externalSize = std::filesystem::file_size(file.externalPath, fileError);
    if (fileError || externalSize != file.sizeBytes) {
      error = fileError ? "Unable to read a prepared deployment file: " +
                              fileError.message()
                        : "A prepared deployment file changed size";
      return false;
    }
    if (size == 0) {
      return true;
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
      error = "Prepared deployment file offset exceeds the host file API limit";
      return false;
    }
    std::ifstream stream(file.externalPath, std::ios::binary);
    if (!stream) {
      error = "Unable to open a prepared deployment file";
      return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!stream) {
      error = "Unable to read a prepared deployment file";
      return false;
    }
    return true;
  }
  if (!file.embeddedData.empty()) {
    if (file.embeddedData.size() != file.sizeBytes) {
      error = "Embedded UDF file data has an inconsistent size";
      return false;
    }
    std::copy_n(file.embeddedData.data() + static_cast<std::size_t>(offset), size, data);
    return true;
  }
  if (size == 0) {
    return true;
  }
  if (!image.is_open()) {
    error = "Unable to open the optical image";
    return false;
  }

  std::uint64_t position = offset;
  std::size_t completed = 0;
  for (const auto& extent : file.extents) {
    if (position >= extent.fileOffset + extent.length) {
      continue;
    }
    if (position < extent.fileOffset) {
      error = "Optical image file extents contain a gap";
      return false;
    }
    const std::uint64_t withinExtent = position - extent.fileOffset;
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(
        extent.length - withinExtent, static_cast<std::uint64_t>(size - completed)));
    if (!readAt(image, extent.imageOffset + withinExtent, data + completed, available)) {
      error = "Unable to read file data from the optical image";
      return false;
    }
    position += available;
    completed += available;
    if (completed == size) {
      return true;
    }
  }
  error = "Optical image file extents ended before the requested data";
  return false;
}

bool readImageFile(const std::filesystem::path& imagePath,
                   const ImageFileRecord& file, const std::uint64_t offset,
                   unsigned char* data, const std::size_t size,
                   std::string& error) {
  if (!file.externalPath.empty() || !file.embeddedData.empty() || size == 0) {
    std::ifstream unused;
    return readImageFile(unused, file, offset, data, size, error);
  }
  std::ifstream image(imagePath, std::ios::binary);
  return readImageFile(image, file, offset, data, size, error);
}

}  // namespace rufus::core::detail
