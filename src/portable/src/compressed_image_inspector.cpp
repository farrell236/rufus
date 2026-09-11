/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "compressed_image_inspector.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace rufus::core::detail {

namespace {

constexpr std::uint64_t kMaximumMetadataBytes = 64ULL * 1024ULL * 1024ULL;

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

std::uint64_t littleEndian(const unsigned char* data, const std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < size; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
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

bool checkedAdd(std::uint64_t& total, const std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) {
    return false;
  }
  total += value;
  return true;
}

class FileCursor final {
 public:
  FileCursor(const std::filesystem::path& path, const std::uint64_t size)
      : stream_(path, std::ios::binary), size_(size) {}

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(stream_); }
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
  [[nodiscard]] std::uint64_t remaining() const noexcept { return size_ - position_; }

  bool read(unsigned char* data, const std::size_t size) {
    if (size > remaining() || size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
      return false;
    }
    stream_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    if (stream_.gcount() != static_cast<std::streamsize>(size)) {
      return false;
    }
    position_ += size;
    return true;
  }

  bool skip(const std::uint64_t size) {
    if (size > remaining() || position_ + size >
                                  static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      return false;
    }
    position_ += size;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(position_), std::ios::beg);
    return static_cast<bool>(stream_);
  }

 private:
  std::ifstream stream_;
  std::uint64_t size_{};
  std::uint64_t position_{};
};

bool readAt(std::ifstream& stream, const std::uint64_t fileSize,
            const std::uint64_t offset, unsigned char* data, const std::size_t size) {
  if (offset > fileSize || size > fileSize - offset ||
      offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
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

CompressedInspectionResult inspectGzip(const std::filesystem::path& path,
                                       const std::uint64_t fileSize) {
  CompressedInspectionResult result;
  FileCursor cursor(path, fileSize);
  std::array<unsigned char, 10> header{};
  if (!cursor.valid() || !cursor.read(header.data(), header.size()) || header[0] != 0x1fU ||
      header[1] != 0x8bU || header[2] != 8U || (header[3] & 0xe0U) != 0) {
    result.warnings.emplace_back("gzip header is invalid");
    return result;
  }
  const unsigned char flags = header[3];
  if ((flags & 0x04U) != 0) {
    std::array<unsigned char, 2> length{};
    if (!cursor.read(length.data(), length.size()) || !cursor.skip(littleEndian16(length.data()))) {
      result.warnings.emplace_back("gzip extra field is truncated");
      return result;
    }
  }
  auto skipTerminated = [&cursor]() {
    unsigned char value = 0;
    std::size_t count = 0;
    do {
      if (!cursor.read(&value, 1) || ++count > 1024U * 1024U) {
        return false;
      }
    } while (value != 0);
    return true;
  };
  if (((flags & 0x08U) != 0 && !skipTerminated()) ||
      ((flags & 0x10U) != 0 && !skipTerminated()) ||
      ((flags & 0x02U) != 0 && !cursor.skip(2)) || cursor.remaining() < 8) {
    result.warnings.emplace_back("gzip optional header or trailer is truncated");
    return result;
  }
  result.valid = true;
  return result;
}

CompressedInspectionResult inspectBzip2(const std::filesystem::path& path,
                                        const std::uint64_t fileSize) {
  CompressedInspectionResult result;
  std::array<unsigned char, 4> header{};
  std::ifstream stream(path, std::ios::binary);
  if (fileSize < header.size() ||
      !readAt(stream, fileSize, 0, header.data(), header.size()) ||
      header[0] != 'B' || header[1] != 'Z' || header[2] != 'h' ||
      header[3] < '1' || header[3] > '9') {
    result.warnings.emplace_back("bzip2 header is invalid");
    return result;
  }
  result.valid = true;
  return result;
}

CompressedInspectionResult inspectLzma(const std::filesystem::path& path,
                                       const std::uint64_t fileSize) {
  CompressedInspectionResult result;
  std::array<unsigned char, 13> header{};
  std::ifstream stream(path, std::ios::binary);
  if (fileSize < header.size() ||
      !readAt(stream, fileSize, 0, header.data(), header.size()) ||
      header[0] > 224U) {
    result.warnings.emplace_back("LZMA-alone header is invalid");
    return result;
  }
  result.valid = true;
  return result;
}

CompressedInspectionResult inspectZstd(const std::filesystem::path& path,
                                       const std::uint64_t fileSize) {
  CompressedInspectionResult result;
  FileCursor cursor(path, fileSize);
  bool allSizesKnown = true;
  std::uint64_t totalSize = 0;
  std::size_t frames = 0;
  while (cursor.remaining() != 0) {
    std::array<unsigned char, 4> magicBytes{};
    if (!cursor.read(magicBytes.data(), magicBytes.size())) {
      break;
    }
    const std::uint32_t magic = littleEndian32(magicBytes.data());
    if (magic >= 0x184d2a50U && magic <= 0x184d2a5fU) {
      std::array<unsigned char, 4> sizeBytes{};
      if (!cursor.read(sizeBytes.data(), sizeBytes.size()) ||
          !cursor.skip(littleEndian32(sizeBytes.data()))) {
        result.warnings.emplace_back("Zstandard skippable frame is truncated");
        return result;
      }
      continue;
    }
    if (magic != 0xfd2fb528U) {
      result.warnings.emplace_back("Zstandard frame magic is invalid");
      return result;
    }
    ++frames;
    unsigned char descriptor = 0;
    if (!cursor.read(&descriptor, 1) || (descriptor & 0x18U) != 0) {
      result.warnings.emplace_back("Zstandard frame descriptor is invalid");
      return result;
    }
    const unsigned int contentSizeFlag = descriptor >> 6U;
    const bool singleSegment = (descriptor & 0x20U) != 0;
    const bool checksum = (descriptor & 0x04U) != 0;
    const unsigned int dictionaryFlag = descriptor & 0x03U;
    if (!singleSegment && !cursor.skip(1)) {
      result.warnings.emplace_back("Zstandard window descriptor is truncated");
      return result;
    }
    constexpr std::array<std::size_t, 4> dictionarySizes{0, 1, 2, 4};
    if (!cursor.skip(dictionarySizes[dictionaryFlag])) {
      result.warnings.emplace_back("Zstandard dictionary identifier is truncated");
      return result;
    }
    const std::size_t contentSizeBytes =
        contentSizeFlag == 0 ? (singleSegment ? 1U : 0U) : (1U << contentSizeFlag);
    if (contentSizeBytes != 0) {
      std::array<unsigned char, 8> sizeBytes{};
      if (!cursor.read(sizeBytes.data(), contentSizeBytes)) {
        result.warnings.emplace_back("Zstandard frame content size is truncated");
        return result;
      }
      std::uint64_t size = littleEndian(sizeBytes.data(), contentSizeBytes);
      if (contentSizeBytes == 2) {
        size += 256U;
      }
      if (!checkedAdd(totalSize, size)) {
        result.warnings.emplace_back("Zstandard expanded size overflows 64 bits");
        return result;
      }
    } else {
      allSizesKnown = false;
    }

    bool lastBlock = false;
    while (!lastBlock) {
      std::array<unsigned char, 3> blockHeader{};
      if (!cursor.read(blockHeader.data(), blockHeader.size())) {
        result.warnings.emplace_back("Zstandard block header is truncated");
        return result;
      }
      const std::uint32_t encoded = static_cast<std::uint32_t>(blockHeader[0]) |
                                    (static_cast<std::uint32_t>(blockHeader[1]) << 8U) |
                                    (static_cast<std::uint32_t>(blockHeader[2]) << 16U);
      lastBlock = (encoded & 1U) != 0;
      const unsigned int type = (encoded >> 1U) & 3U;
      const std::uint32_t blockSize = encoded >> 3U;
      if (type == 3U || blockSize > 128U * 1024U ||
          !cursor.skip(type == 1U ? 1U : blockSize)) {
        result.warnings.emplace_back("Zstandard block is invalid or truncated");
        return result;
      }
    }
    if (checksum && !cursor.skip(4)) {
      result.warnings.emplace_back("Zstandard content checksum is truncated");
      return result;
    }
  }
  result.valid = frames != 0 && cursor.remaining() == 0;
  result.sizeKnown = result.valid && allSizesKnown;
  result.expandedSizeBytes = result.sizeKnown ? totalSize : 0;
  if (result.valid && !result.sizeKnown) {
    result.warnings.emplace_back("Zstandard frame does not declare its expanded size");
  }
  return result;
}

bool readVli(const unsigned char* data, const std::size_t end, std::size_t& offset,
             std::uint64_t& value) {
  value = 0;
  for (unsigned int byteIndex = 0; byteIndex < 9 && offset < end; ++byteIndex) {
    const unsigned char byte = data[offset++];
    if (byteIndex == 8 && (byte & 0xfeU) != 0) {
      return false;
    }
    value |= static_cast<std::uint64_t>(byte & 0x7fU) << (byteIndex * 7U);
    if ((byte & 0x80U) == 0) {
      return byteIndex == 0 || byte != 0;
    }
  }
  return false;
}

CompressedInspectionResult inspectXz(const std::filesystem::path& path,
                                     const std::uint64_t fileSize) {
  CompressedInspectionResult result;
  std::ifstream stream(path, std::ios::binary);
  std::uint64_t end = fileSize;
  std::uint64_t totalSize = 0;
  std::size_t streamCount = 0;
  while (end != 0) {
    unsigned char padding = 0;
    while (end != 0 && readAt(stream, fileSize, end - 1U, &padding, 1) && padding == 0) {
      --end;
    }
    if (end == 0 || (fileSize - end) % 4U != 0 || end < 24U) {
      result.warnings.emplace_back("XZ stream padding or footer is invalid");
      return result;
    }
    std::array<unsigned char, 12> footer{};
    if (!readAt(stream, fileSize, end - footer.size(), footer.data(), footer.size()) ||
        footer[10] != 'Y' || footer[11] != 'Z' ||
        crc32(footer.data() + 4, 6) != littleEndian32(footer.data())) {
      result.warnings.emplace_back("XZ stream footer checksum is invalid");
      return result;
    }
    const std::uint64_t indexSize =
        (static_cast<std::uint64_t>(littleEndian32(footer.data() + 4)) + 1U) * 4U;
    if (indexSize < 8U || indexSize > kMaximumMetadataBytes || indexSize > end - 12U ||
        indexSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      result.warnings.emplace_back("XZ index size is invalid");
      return result;
    }
    const std::uint64_t indexOffset = end - 12U - indexSize;
    std::vector<unsigned char> index(static_cast<std::size_t>(indexSize));
    if (!readAt(stream, fileSize, indexOffset, index.data(), index.size()) || index[0] != 0 ||
        crc32(index.data(), index.size() - 4U) !=
            littleEndian32(index.data() + index.size() - 4U)) {
      result.warnings.emplace_back("XZ index checksum is invalid");
      return result;
    }
    std::size_t offset = 1;
    std::uint64_t records = 0;
    if (!readVli(index.data(), index.size() - 4U, offset, records) || records > 1048576U) {
      result.warnings.emplace_back("XZ index record count is invalid");
      return result;
    }
    std::uint64_t paddedBlocksSize = 0;
    for (std::uint64_t record = 0; record < records; ++record) {
      std::uint64_t unpadded = 0;
      std::uint64_t uncompressed = 0;
      if (!readVli(index.data(), index.size() - 4U, offset, unpadded) || unpadded == 0 ||
          !readVli(index.data(), index.size() - 4U, offset, uncompressed) ||
          unpadded > std::numeric_limits<std::uint64_t>::max() - 3U ||
          !checkedAdd(paddedBlocksSize, (unpadded + 3U) & ~std::uint64_t{3U}) ||
          !checkedAdd(totalSize, uncompressed)) {
        result.warnings.emplace_back("XZ index record is invalid");
        return result;
      }
    }
    while (offset < index.size() - 4U && index[offset] == 0) {
      ++offset;
    }
    const std::uint64_t streamSize = 12U + paddedBlocksSize + indexSize + 12U;
    if (offset != index.size() - 4U || streamSize > end) {
      result.warnings.emplace_back("XZ index padding or stream size is invalid");
      return result;
    }
    const std::uint64_t streamStart = end - streamSize;
    std::array<unsigned char, 12> header{};
    constexpr std::array<unsigned char, 6> magic{0xfd, '7', 'z', 'X', 'Z', 0};
    if (!readAt(stream, fileSize, streamStart, header.data(), header.size()) ||
        !std::equal(magic.begin(), magic.end(), header.begin()) ||
        header[6] != footer[8] || header[7] != footer[9] ||
        crc32(header.data() + 6, 2) != littleEndian32(header.data() + 8)) {
      result.warnings.emplace_back("XZ stream header checksum is invalid");
      return result;
    }
    ++streamCount;
    end = streamStart;
  }
  result.valid = streamCount != 0;
  result.sizeKnown = result.valid;
  result.expandedSizeBytes = totalSize;
  return result;
}

CompressedInspectionResult inspectZip(const std::filesystem::path& path,
                                      const std::uint64_t fileSize) {
  CompressedInspectionResult result;
  if (fileSize < 22U) {
    result.warnings.emplace_back("ZIP end record is missing");
    return result;
  }
  std::ifstream stream(path, std::ios::binary);
  const std::size_t tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(fileSize, 65557U));
  std::vector<unsigned char> tail(tailSize);
  if (!readAt(stream, fileSize, fileSize - tailSize, tail.data(), tail.size())) {
    return result;
  }
  std::size_t endOffset = std::string::npos;
  for (std::size_t offset = tail.size() - 22U;; --offset) {
    if (littleEndian32(tail.data() + offset) == 0x06054b50U &&
        offset + 22U + littleEndian16(tail.data() + offset + 20U) == tail.size()) {
      endOffset = offset;
      break;
    }
    if (offset == 0) {
      break;
    }
  }
  if (endOffset == std::string::npos) {
    result.warnings.emplace_back("ZIP end record is invalid");
    return result;
  }
  const unsigned char* endRecord = tail.data() + endOffset;
  const std::uint16_t entries = littleEndian16(endRecord + 10);
  const std::uint32_t directorySize = littleEndian32(endRecord + 12);
  const std::uint32_t directoryOffset = littleEndian32(endRecord + 16);
  const std::uint64_t endRecordOffset = fileSize - tailSize + endOffset;
  if (littleEndian16(endRecord + 4) != 0 || littleEndian16(endRecord + 6) != 0 ||
      littleEndian16(endRecord + 8) != entries || entries == 0xffffU ||
      directorySize == 0xffffffffU || directoryOffset == 0xffffffffU ||
      directorySize > kMaximumMetadataBytes || directoryOffset > fileSize ||
      directorySize > fileSize - directoryOffset ||
      static_cast<std::uint64_t>(directoryOffset) + directorySize != endRecordOffset) {
    result.warnings.emplace_back("ZIP64, split, or oversized archives are not supported for image sizing");
    return result;
  }
  std::vector<unsigned char> directory(directorySize);
  if (!readAt(stream, fileSize, directoryOffset, directory.data(), directory.size())) {
    return result;
  }
  std::size_t offset = 0;
  std::uint64_t expandedSize = 0;
  std::size_t parsedEntries = 0;
  while (offset < directory.size()) {
    if (directory.size() - offset < 46U ||
        littleEndian32(directory.data() + offset) != 0x02014b50U) {
      result.warnings.emplace_back("ZIP central directory is malformed");
      return result;
    }
    const unsigned char* entry = directory.data() + offset;
    const std::size_t nameLength = littleEndian16(entry + 28);
    const std::size_t extraLength = littleEndian16(entry + 30);
    const std::size_t commentLength = littleEndian16(entry + 32);
    const std::size_t recordSize = 46U + nameLength + extraLength + commentLength;
    const std::uint32_t compressedSize = littleEndian32(entry + 20);
    const std::uint32_t uncompressedSize = littleEndian32(entry + 24);
    const std::uint32_t localOffset = littleEndian32(entry + 42);
    if (recordSize > directory.size() - offset || compressedSize == 0xffffffffU ||
        uncompressedSize == 0xffffffffU || localOffset == 0xffffffffU) {
      result.warnings.emplace_back("ZIP entry metadata is truncated or requires ZIP64");
      return result;
    }
    std::array<unsigned char, 30> local{};
    if (!readAt(stream, fileSize, localOffset, local.data(), local.size()) ||
        littleEndian32(local.data()) != 0x04034b50U ||
        littleEndian16(local.data() + 6) != littleEndian16(entry + 8) ||
        littleEndian16(local.data() + 8) != littleEndian16(entry + 10)) {
      result.warnings.emplace_back("ZIP local file header does not match its directory entry");
      return result;
    }
    const std::uint64_t localDataOffset =
        static_cast<std::uint64_t>(localOffset) + local.size() +
        littleEndian16(local.data() + 26) + littleEndian16(local.data() + 28);
    if (localDataOffset > directoryOffset || compressedSize > directoryOffset - localDataOffset) {
      result.warnings.emplace_back("ZIP file payload overlaps or exceeds its central directory");
      return result;
    }
    const bool directoryEntry = nameLength != 0 && entry[46U + nameLength - 1U] == '/';
    if (!directoryEntry && !checkedAdd(expandedSize, uncompressedSize)) {
      result.warnings.emplace_back("ZIP expanded size overflows 64 bits");
      return result;
    }
    ++parsedEntries;
    offset += recordSize;
  }
  if (parsedEntries != entries) {
    result.warnings.emplace_back("ZIP entry count does not match its end record");
    return result;
  }
  result.valid = true;
  result.sizeKnown = true;
  result.expandedSizeBytes = expandedSize;
  return result;
}

}  // namespace

CompressedInspectionResult inspectCompressedImage(const std::filesystem::path& path,
                                                   const ImageFormat format) {
  std::error_code error;
  const std::uint64_t fileSize = std::filesystem::file_size(path, error);
  if (error) {
    return {};
  }
  switch (format) {
    case ImageFormat::Gzip:
      return inspectGzip(path, fileSize);
    case ImageFormat::Bzip2:
      return inspectBzip2(path, fileSize);
    case ImageFormat::Zip:
      return inspectZip(path, fileSize);
    case ImageFormat::Xz:
      return inspectXz(path, fileSize);
    case ImageFormat::Lzma:
      return inspectLzma(path, fileSize);
    case ImageFormat::Zstd:
      return inspectZstd(path, fileSize);
    default:
      return {};
  }
}

}  // namespace rufus::core::detail
