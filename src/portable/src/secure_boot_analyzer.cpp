/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/secure_boot_analyzer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include <zlib.h>

#include "iso9660_reader.hpp"
#include "udf_reader.hpp"

namespace rufus::core {
namespace {

constexpr std::uint64_t kMaximumEfiImageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumSecureBootDatabaseBytes =
    64ULL * 1024ULL * 1024ULL;
constexpr std::array<unsigned char, 16> kEfiCertSha256Guid{
    0x26U, 0x16U, 0xc4U, 0xc1U, 0x4cU, 0x50U, 0x92U, 0x40U,
    0xacU, 0xa9U, 0x41U, 0xf9U, 0x36U, 0x93U, 0x43U, 0x28U};

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

std::uint32_t rotateRight(const std::uint32_t value, const unsigned int bits) {
  return (value >> bits) | (value << (32U - bits));
}

class Sha256 final {
 public:
  void update(const unsigned char* data, std::size_t size) {
    totalBytes_ += size;
    while (size != 0U) {
      const std::size_t copied = std::min(size, block_.size() - blockBytes_);
      std::copy_n(data, copied, block_.begin() +
                                    static_cast<std::ptrdiff_t>(blockBytes_));
      blockBytes_ += copied;
      data += copied;
      size -= copied;
      if (blockBytes_ == block_.size()) {
        transform(block_.data());
        blockBytes_ = 0U;
      }
    }
  }

  std::array<unsigned char, 32> finish() {
    const std::uint64_t bitCount = totalBytes_ * 8U;
    block_[blockBytes_++] = 0x80U;
    if (blockBytes_ > 56U) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockBytes_),
                block_.end(), 0U);
      transform(block_.data());
      blockBytes_ = 0U;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockBytes_),
              block_.begin() + 56, 0U);
    for (unsigned int index = 0; index < 8U; ++index) {
      block_[63U - index] =
          static_cast<unsigned char>(bitCount >> (index * 8U));
    }
    transform(block_.data());
    std::array<unsigned char, 32> digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
      digest[index * 4U] = static_cast<unsigned char>(state_[index] >> 24U);
      digest[index * 4U + 1U] =
          static_cast<unsigned char>(state_[index] >> 16U);
      digest[index * 4U + 2U] =
          static_cast<unsigned char>(state_[index] >> 8U);
      digest[index * 4U + 3U] = static_cast<unsigned char>(state_[index]);
    }
    return digest;
  }

 private:
  void transform(const unsigned char* block) {
    static constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      words[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                     (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                     static_cast<std::uint32_t>(block[index * 4U + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const std::uint32_t s0 = rotateRight(words[index - 15U], 7U) ^
                               rotateRight(words[index - 15U], 18U) ^
                               (words[index - 15U] >> 3U);
      const std::uint32_t s1 = rotateRight(words[index - 2U], 17U) ^
                               rotateRight(words[index - 2U], 19U) ^
                               (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t upper = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^
                                  rotateRight(e, 25U);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 = h + upper + choose + k[index] + words[index];
      const std::uint32_t lower = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^
                                  rotateRight(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = lower + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<unsigned char, 64> block_{};
  std::size_t blockBytes_{};
  std::uint64_t totalBytes_{};
};

std::string sha256Hex(const std::vector<unsigned char>& bytes) {
  Sha256 hash;
  hash.update(bytes.data(), bytes.size());
  const auto digest = hash.finish();
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const unsigned char byte : digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::string finishSha256Hex(Sha256& hash) {
  const auto digest = hash.finish();
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const unsigned char byte : digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::string authenticodeSha256Hex(
    const std::vector<unsigned char>& bytes, const std::uint32_t peOffset,
    const std::size_t optionalOffset, const std::uint16_t optionalSize,
    const std::size_t directoryOffset, const std::uint32_t certificateOffset,
    const std::uint32_t certificateSize) {
  const std::size_t checksumOffset = optionalOffset + 64U;
  const std::size_t securityDirectoryOffset = directoryOffset + 4U * 8U;
  if (checksumOffset > bytes.size() || bytes.size() - checksumOffset < 4U ||
      securityDirectoryOffset < checksumOffset + 4U ||
      securityDirectoryOffset > bytes.size() ||
      bytes.size() - securityDirectoryOffset < 8U ||
      optionalOffset > bytes.size() || bytes.size() - optionalOffset < 64U) {
    return {};
  }
  const std::uint32_t sizeOfHeaders =
      little32(bytes.data() + optionalOffset + 60U);
  const std::size_t sectionTable = optionalOffset + optionalSize;
  const std::uint16_t sectionCount = little16(bytes.data() + peOffset + 6U);
  if (sizeOfHeaders < securityDirectoryOffset + 8U ||
      sizeOfHeaders > bytes.size() || sectionTable > sizeOfHeaders ||
      static_cast<std::uint64_t>(sectionCount) * 40U >
          sizeOfHeaders - sectionTable) {
    return {};
  }

  Sha256 hash;
  hash.update(bytes.data(), checksumOffset);
  hash.update(bytes.data() + checksumOffset + 4U,
              securityDirectoryOffset - checksumOffset - 4U);
  hash.update(bytes.data() + securityDirectoryOffset + 8U,
              sizeOfHeaders - securityDirectoryOffset - 8U);

  std::vector<std::pair<std::uint32_t, std::uint32_t>> sections;
  sections.reserve(sectionCount);
  for (std::uint16_t index = 0U; index < sectionCount; ++index) {
    const unsigned char* section =
        bytes.data() + sectionTable + static_cast<std::size_t>(index) * 40U;
    const std::uint32_t rawBytes = little32(section + 16U);
    const std::uint32_t rawOffset = little32(section + 20U);
    if (rawBytes == 0U) {
      continue;
    }
    if (rawOffset > bytes.size() || rawBytes > bytes.size() - rawOffset) {
      return {};
    }
    sections.emplace_back(rawOffset, rawBytes);
  }
  std::sort(sections.begin(), sections.end());
  std::uint64_t hashedBytes = sizeOfHeaders;
  for (const auto& [offset, size] : sections) {
    hash.update(bytes.data() + offset, size);
    hashedBytes += size;
    if (hashedBytes > bytes.size()) {
      return {};
    }
  }
  if (hashedBytes < bytes.size()) {
    const std::size_t overlay = static_cast<std::size_t>(hashedBytes);
    const bool boundedCertificate =
        certificateOffset >= overlay && certificateOffset <= bytes.size() &&
        certificateSize <= bytes.size() - certificateOffset;
    if (boundedCertificate) {
      hash.update(bytes.data() + overlay, certificateOffset - overlay);
      const std::size_t afterCertificate = certificateOffset + certificateSize;
      if (afterCertificate < bytes.size()) {
        hash.update(bytes.data() + afterCertificate,
                    bytes.size() - afterCertificate);
      }
    } else {
      hash.update(bytes.data() + overlay, bytes.size() - overlay);
    }
  }
  return finishSha256Hex(hash);
}

std::string hexBytes(const unsigned char* bytes, const std::size_t size) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return output.str();
}

struct EfiSignatureListImport final {
  bool recognized{};
  std::set<std::string> sha256Hashes;
  std::string error;
};

struct ZipEntry final {
  std::string name;
  std::uint16_t flags{};
  std::uint16_t method{};
  std::uint32_t crc{};
  std::uint32_t compressedBytes{};
  std::uint32_t uncompressedBytes{};
  std::uint32_t localHeaderOffset{};
};

bool extractZipEntry(const std::vector<unsigned char>& archive,
                     const ZipEntry& entry,
                     std::vector<unsigned char>& contents,
                     std::string& error) {
  if ((entry.flags & 1U) != 0U) {
    error = "Encrypted Secure Boot update archives are not supported";
    return false;
  }
  if (entry.method != 0U && entry.method != 8U) {
    error = "The Secure Boot update archive uses an unsupported compression method";
    return false;
  }
  const std::size_t local = entry.localHeaderOffset;
  if (local > archive.size() || archive.size() - local < 30U ||
      little32(archive.data() + local) != 0x04034b50U) {
    error = "The Secure Boot update archive has an invalid local header";
    return false;
  }
  const std::uint16_t nameBytes = little16(archive.data() + local + 26U);
  const std::uint16_t extraBytes = little16(archive.data() + local + 28U);
  const std::uint64_t dataOffset =
      static_cast<std::uint64_t>(local) + 30U + nameBytes + extraBytes;
  if (dataOffset > archive.size() ||
      entry.compressedBytes > archive.size() - dataOffset ||
      entry.uncompressedBytes > kMaximumSecureBootDatabaseBytes) {
    error = "The Secure Boot update archive entry is out of bounds";
    return false;
  }
  contents.assign(entry.uncompressedBytes, 0U);
  const unsigned char* compressed =
      archive.data() + static_cast<std::size_t>(dataOffset);
  if (entry.method == 0U) {
    if (entry.compressedBytes != entry.uncompressedBytes) {
      error = "A stored Secure Boot update entry has inconsistent sizes";
      return false;
    }
    std::copy_n(compressed, entry.compressedBytes, contents.begin());
  } else {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(compressed);
    stream.avail_in = entry.compressedBytes;
    stream.next_out = contents.data();
    stream.avail_out = entry.uncompressedBytes;
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
      error = "Unable to initialize the Secure Boot update decompressor";
      return false;
    }
    const int status = inflate(&stream, Z_FINISH);
    const bool complete = status == Z_STREAM_END &&
                          stream.total_in == entry.compressedBytes &&
                          stream.total_out == entry.uncompressedBytes;
    inflateEnd(&stream);
    if (!complete) {
      error = "A Secure Boot update archive entry could not be decompressed completely";
      return false;
    }
  }
  const std::uint32_t observedCrc = static_cast<std::uint32_t>(
      crc32(0U, contents.data(), static_cast<uInt>(contents.size())));
  if (observedCrc != entry.crc) {
    contents.clear();
    error = "A Secure Boot update archive entry failed its CRC check";
    return false;
  }
  return true;
}

bool parseZipDirectory(const std::vector<unsigned char>& archive,
                       std::vector<ZipEntry>& entries, std::string& error) {
  if (archive.size() < 22U) {
    error = "The Secure Boot update is not a ZIP archive";
    return false;
  }
  const std::size_t earliest = archive.size() > 65557U
                                   ? archive.size() - 65557U
                                   : 0U;
  std::size_t end = std::numeric_limits<std::size_t>::max();
  for (std::size_t candidate = archive.size() - 22U;; --candidate) {
    if (little32(archive.data() + candidate) == 0x06054b50U) {
      end = candidate;
      break;
    }
    if (candidate == earliest) {
      break;
    }
  }
  if (end == std::numeric_limits<std::size_t>::max() ||
      archive.size() - end < 22U || little16(archive.data() + end + 4U) != 0U ||
      little16(archive.data() + end + 6U) != 0U) {
    error = "The Secure Boot update ZIP directory is missing or unsupported";
    return false;
  }
  const std::uint16_t diskEntryCount = little16(archive.data() + end + 8U);
  const std::uint16_t entryCount = little16(archive.data() + end + 10U);
  const std::uint32_t directoryBytes = little32(archive.data() + end + 12U);
  const std::uint32_t directoryOffset = little32(archive.data() + end + 16U);
  if (entryCount == 0U || entryCount != diskEntryCount || entryCount > 128U ||
      directoryOffset > archive.size() ||
      directoryBytes > archive.size() - directoryOffset ||
      static_cast<std::uint64_t>(directoryOffset) + directoryBytes > end) {
    error = "The Secure Boot update ZIP directory is invalid";
    return false;
  }
  std::size_t cursor = directoryOffset;
  entries.clear();
  entries.reserve(entryCount);
  for (std::uint16_t index = 0U; index < entryCount; ++index) {
    if (cursor > archive.size() || archive.size() - cursor < 46U ||
        little32(archive.data() + cursor) != 0x02014b50U) {
      error = "The Secure Boot update ZIP entry header is invalid";
      return false;
    }
    const std::uint16_t nameBytes = little16(archive.data() + cursor + 28U);
    const std::uint16_t extraBytes = little16(archive.data() + cursor + 30U);
    const std::uint16_t commentBytes = little16(archive.data() + cursor + 32U);
    const std::uint64_t next = static_cast<std::uint64_t>(cursor) + 46U +
                               nameBytes + extraBytes + commentBytes;
    if (next > archive.size()) {
      error = "The Secure Boot update ZIP entry is truncated";
      return false;
    }
    ZipEntry entry;
    entry.flags = little16(archive.data() + cursor + 8U);
    entry.method = little16(archive.data() + cursor + 10U);
    entry.crc = little32(archive.data() + cursor + 16U);
    entry.compressedBytes = little32(archive.data() + cursor + 20U);
    entry.uncompressedBytes = little32(archive.data() + cursor + 24U);
    entry.localHeaderOffset = little32(archive.data() + cursor + 42U);
    entry.name.assign(
        reinterpret_cast<const char*>(archive.data() + cursor + 46U),
        nameBytes);
    entries.push_back(std::move(entry));
    cursor = static_cast<std::size_t>(next);
  }
  return true;
}

EfiSignatureListImport parseEfiSignatureLists(
    const std::vector<unsigned char>& bytes) {
  EfiSignatureListImport result;
  std::size_t cursor = 0U;
  // EFI_VARIABLE_AUTHENTICATION_2 places an EFI_TIME followed by a
  // WIN_CERTIFICATE_UEFI_GUID in front of the ordinary signature lists.
  if (bytes.size() >= 40U) {
    const std::uint32_t certificateBytes = little32(bytes.data() + 16U);
    if (certificateBytes >= 24U && certificateBytes <= bytes.size() - 16U) {
      cursor = 16U + certificateBytes;
    }
  }
  while (cursor < bytes.size()) {
    if (bytes.size() - cursor < 28U) {
      result.error = "The EFI signature-list header is truncated";
      return result;
    }
    const unsigned char* list = bytes.data() + cursor;
    const std::uint32_t listBytes = little32(list + 16U);
    const std::uint32_t headerBytes = little32(list + 20U);
    const std::uint32_t signatureBytes = little32(list + 24U);
    if (listBytes < 28U || listBytes > bytes.size() - cursor ||
        headerBytes > listBytes - 28U || signatureBytes < 16U) {
      result.error = "The EFI signature-list geometry is invalid";
      return result;
    }
    const std::uint64_t signaturesOffset = 28ULL + headerBytes;
    const std::uint64_t signaturesBytes = listBytes - signaturesOffset;
    if (signaturesBytes % signatureBytes != 0U) {
      result.error = "The EFI signature-list payload is misaligned";
      return result;
    }
    result.recognized = true;
    if (std::equal(kEfiCertSha256Guid.begin(), kEfiCertSha256Guid.end(), list)) {
      if (signatureBytes != 48U) {
        result.error = "An EFI SHA-256 signature entry has an invalid size";
        return result;
      }
      for (std::uint64_t offset = signaturesOffset;
           offset < listBytes; offset += signatureBytes) {
        result.sha256Hashes.insert(
            hexBytes(list + offset + 16U, 32U));
      }
    }
    cursor += listBytes;
  }
  return result;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

std::string trim(std::string value) {
  const auto isSpace = [](const unsigned char value) {
    return std::isspace(value) != 0;
  };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), isSpace));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
              value.end());
  return value;
}

std::vector<std::string> split(const std::string& text, const char delimiter) {
  std::vector<std::string> fields;
  std::istringstream input(text);
  std::string field;
  while (std::getline(input, field, delimiter)) {
    fields.push_back(trim(std::move(field)));
  }
  return fields;
}

bool validHexDigest(const std::string& value, const std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](const unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

std::map<std::string, std::uint64_t> readSbat(const std::vector<unsigned char>& bytes) {
  std::map<std::string, std::uint64_t> values;
  const std::string body(bytes.begin(), bytes.end());
  std::size_t cursor = 0;
  while (cursor < body.size()) {
    const std::size_t end = body.find_first_of("\r\n\0", cursor);
    const std::string line = body.substr(
        cursor, end == std::string::npos ? std::string::npos : end - cursor);
    const auto fields = split(line, ',');
    if (fields.size() >= 2U && !fields[0].empty() &&
        std::all_of(fields[1].begin(), fields[1].end(), [](const unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      try {
        values[lower(fields[0])] = std::stoull(fields[1]);
      } catch (const std::exception&) {
      }
    }
    if (end == std::string::npos) {
      break;
    }
    cursor = end + 1U;
  }
  return values;
}

std::map<std::string, std::uint64_t> readPeSbatSection(
    const std::vector<unsigned char>& bytes, const std::uint32_t peOffset,
    const std::size_t optionalOffset, const std::uint16_t optionalSize) {
  const std::size_t sectionTable = optionalOffset + optionalSize;
  const std::uint16_t sectionCount = little16(bytes.data() + peOffset + 6U);
  if (sectionTable > bytes.size() ||
      static_cast<std::uint64_t>(sectionCount) * 40U >
          bytes.size() - sectionTable) {
    return {};
  }
  constexpr std::array<unsigned char, 8> sbatName{
      '.', 's', 'b', 'a', 't', 0U, 0U, 0U};
  for (std::uint16_t index = 0U; index < sectionCount; ++index) {
    const unsigned char* section =
        bytes.data() + sectionTable + static_cast<std::size_t>(index) * 40U;
    if (!std::equal(sbatName.begin(), sbatName.end(), section)) {
      continue;
    }
    const std::uint32_t rawBytes = little32(section + 16U);
    const std::uint32_t rawOffset = little32(section + 20U);
    if (rawOffset > bytes.size() || rawBytes > bytes.size() - rawOffset) {
      return {};
    }
    return readSbat(std::vector<unsigned char>(
        bytes.begin() + static_cast<std::ptrdiff_t>(rawOffset),
        bytes.begin() + static_cast<std::ptrdiff_t>(rawOffset + rawBytes)));
  }
  return {};
}

bool isEfiPath(const std::string& path) {
  const std::string folded = lower(path);
  return folded.size() >= 4U &&
         folded.compare(folded.size() - 4U, 4U, ".efi") == 0;
}

}  // namespace

SecureBootDatabase packagedSecureBootBaseline() {
  SecureBootDatabase database;
  database.source = "Rufus++ packaged SBAT baseline";
  database.sbatVersion = "2026-06";
  database.trustedBaseline = true;
  database.minimumSbatGeneration = {
      {"grub", 5U}, {"grub.proxmox", 2U}, {"shim", 4U}};
  return database;
}

SecureBootDatabaseResult loadSecureBootDatabaseBytes(
    const std::vector<unsigned char>& bytes, std::string source,
    const SecureBootDatabase& baseline) {
  SecureBootDatabaseResult result;
  result.database = baseline;
  if (bytes.empty() || bytes.size() > kMaximumSecureBootDatabaseBytes) {
    result.error =
        "The offline Secure Boot database is empty or exceeds 64 MiB";
    return result;
  }
  if (source.empty()) {
    source = "Imported offline Secure Boot data";
  }
  const bool binary = std::find(bytes.begin(), bytes.end(), 0U) != bytes.end();
  if (binary) {
    const EfiSignatureListImport imported = parseEfiSignatureLists(bytes);
    if (!imported.recognized || !imported.error.empty()) {
      result.error = imported.error.empty()
                         ? "No EFI signature list was found in the database"
                         : imported.error;
      return result;
    }
    result.database.revokedSha256.insert(imported.sha256Hashes.begin(),
                                         imported.sha256Hashes.end());
    result.database.source = std::move(source);
    result.database.trustedBaseline = false;
    result.database.customOverlay = true;
    result.success = true;
    return result;
  }

  std::istringstream text(
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  std::string line;
  std::size_t lineNumber = 0U;
  while (std::getline(text, line)) {
    ++lineNumber;
    line = trim(std::move(line));
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto fields = split(line, ' ');
    if (fields.size() == 2U && lower(fields[0]) == "sha256" &&
        validHexDigest(fields[1], 64U)) {
      result.database.revokedSha256.insert(lower(fields[1]));
      continue;
    }
    if (fields.size() == 3U && lower(fields[0]) == "sbat") {
      try {
        result.database.minimumSbatGeneration[lower(fields[1])] =
            std::stoull(fields[2]);
        continue;
      } catch (const std::exception&) {
      }
    }
    result.error = "Invalid offline Secure Boot database entry at line " +
                   std::to_string(lineNumber);
    return result;
  }
  if (!text.eof()) {
    result.error = "Unable to read the complete offline Secure Boot database";
    return result;
  }
  result.database.source = std::move(source);
  result.database.trustedBaseline = false;
  result.database.customOverlay = true;
  result.success = true;
  return result;
}

SecureBootDatabaseResult loadSecureBootDatabase(
    const std::filesystem::path& path, const SecureBootDatabase& baseline) {
  SecureBootDatabaseResult result;
  result.database = baseline;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.error = "Unable to open the offline Secure Boot database";
    return result;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff length = input.tellg();
  if (length <= 0 || static_cast<std::uint64_t>(length) >
                         kMaximumSecureBootDatabaseBytes) {
    result.error =
        "The offline Secure Boot database is empty or exceeds 64 MiB";
    return result;
  }
  input.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
  input.read(reinterpret_cast<char*>(bytes.data()), length);
  if (!input) {
    result.error = "Unable to read the complete offline Secure Boot database";
    return result;
  }
  return loadSecureBootDatabaseBytes(bytes, path.u8string(), baseline);
}

SecureBootDatabaseResult loadSecureBootDatabaseArchive(
    const std::vector<unsigned char>& archive, std::string source,
    std::string publishedDate, const SecureBootDatabase& baseline) {
  SecureBootDatabaseResult result;
  result.database = baseline;
  if (archive.empty() || archive.size() > kMaximumSecureBootDatabaseBytes) {
    result.error = "The Secure Boot update archive is empty or exceeds 64 MiB";
    return result;
  }
  std::vector<ZipEntry> entries;
  if (!parseZipDirectory(archive, entries, result.error)) {
    return result;
  }

  const std::set<std::string> requiredDatabases{
      "dbx_x64.efiauth2", "dbx_ia32.efiauth2", "dbx_arm.efiauth2",
      "dbx_aarch64.efiauth2"};
  std::set<std::string> importedDatabases;
  std::string version;
  for (const ZipEntry& entry : entries) {
    if (entry.name != "version" &&
        requiredDatabases.count(entry.name) == 0U) {
      continue;
    }
    std::vector<unsigned char> contents;
    if (!extractZipEntry(archive, entry, contents, result.error)) {
      return result;
    }
    if (entry.name == "version") {
      if (!version.empty()) {
        result.error =
            "The Secure Boot update contains duplicate version markers";
        return result;
      }
      version = trim(std::string(contents.begin(), contents.end()));
      if (version.empty() || version.size() > 64U ||
          !std::all_of(version.begin(), version.end(),
                       [](const unsigned char character) {
                         return std::isalnum(character) != 0 ||
                                character == '.' || character == '-';
                       })) {
        result.error = "The Secure Boot update contains an invalid version";
        return result;
      }
      continue;
    }
    if (importedDatabases.count(entry.name) != 0U) {
      result.error = "The Secure Boot update contains a duplicate DBX entry";
      return result;
    }
    const EfiSignatureListImport imported = parseEfiSignatureLists(contents);
    if (!imported.recognized || !imported.error.empty()) {
      result.error = imported.error.empty()
                         ? "A DBX update contains no EFI signature list"
                         : imported.error;
      return result;
    }
    result.database.revokedSha256.insert(imported.sha256Hashes.begin(),
                                         imported.sha256Hashes.end());
    importedDatabases.insert(entry.name);
  }
  if (importedDatabases != requiredDatabases) {
    result.error =
        "The Secure Boot update does not contain all supported DBX architectures";
    return result;
  }
  if (version.empty()) {
    result.error = "The Secure Boot update does not contain a version marker";
    return result;
  }
  result.database.source = source.empty()
                               ? "Imported Secure Boot update archive"
                               : std::move(source);
  result.database.version = std::move(version);
  result.database.publishedDate = std::move(publishedDate);
  result.database.trustedBaseline = false;
  result.database.customOverlay = true;
  result.success = true;
  return result;
}

EfiImageTrust analyzeEfiImage(std::string path,
                              const std::vector<unsigned char>& bytes,
                              const SecureBootDatabase& database) {
  EfiImageTrust result;
  result.path = std::move(path);
  result.sha256 = sha256Hex(bytes);
  if (database.revokedSha256.count(result.sha256) != 0U) {
    result.disposition = SecureBootDisposition::RevokedHash;
    result.findings.emplace_back(
        "The complete EFI image hash appears in the offline revocation database");
  }
  if (bytes.size() < 0x40U || bytes[0] != 'M' || bytes[1] != 'Z') {
    result.disposition = SecureBootDisposition::Invalid;
    result.findings.emplace_back("The file does not contain a valid DOS/PE header");
    return result;
  }
  const std::uint32_t peOffset = little32(bytes.data() + 0x3cU);
  if (peOffset > bytes.size() || bytes.size() - peOffset < 24U ||
      std::memcmp(bytes.data() + peOffset, "PE\0\0", 4U) != 0) {
    result.disposition = SecureBootDisposition::Invalid;
    result.findings.emplace_back("The PE header offset or signature is invalid");
    return result;
  }
  const std::size_t optionalOffset = peOffset + 24U;
  const std::uint16_t optionalSize = little16(bytes.data() + peOffset + 20U);
  if (optionalOffset > bytes.size() || bytes.size() - optionalOffset < optionalSize ||
      optionalSize < 112U) {
    result.disposition = SecureBootDisposition::Invalid;
    result.findings.emplace_back("The PE optional header is truncated");
    return result;
  }
  const std::uint16_t magic = little16(bytes.data() + optionalOffset);
  const std::size_t directoryOffset =
      magic == 0x10bU ? optionalOffset + 96U
      : magic == 0x20bU ? optionalOffset + 112U
                        : 0U;
  if (directoryOffset == 0U || directoryOffset > bytes.size() ||
      bytes.size() - directoryOffset < 5U * 8U ||
      directoryOffset + 5U * 8U > optionalOffset + optionalSize) {
    result.disposition = SecureBootDisposition::Invalid;
    result.findings.emplace_back("The PE data-directory table is invalid");
    return result;
  }
  result.validPe = true;
  const std::uint32_t certificateOffset =
      little32(bytes.data() + directoryOffset + 4U * 8U);
  const std::uint32_t certificateSize =
      little32(bytes.data() + directoryOffset + 4U * 8U + 4U);
  result.authenticodeSha256 = authenticodeSha256Hex(
      bytes, peOffset, optionalOffset, optionalSize, directoryOffset,
      certificateOffset, certificateSize);
  if (!result.authenticodeSha256.empty() &&
      database.revokedSha256.count(result.authenticodeSha256) != 0U) {
    result.disposition = SecureBootDisposition::RevokedHash;
    result.findings.emplace_back(
        "The UEFI/Authenticode image hash appears in the offline revocation database");
  }
  result.authenticodePresent = certificateOffset != 0U && certificateSize >= 8U &&
                               certificateOffset <= bytes.size() &&
                               certificateSize <= bytes.size() - certificateOffset;
  if (!result.authenticodePresent) {
    if (result.disposition != SecureBootDisposition::RevokedHash) {
      result.disposition = SecureBootDisposition::Unsigned;
    }
    result.findings.emplace_back(
        "No bounded Authenticode certificate table is present");
  } else {
    result.findings.emplace_back(
        "An Authenticode certificate table is present; its chain and signature are not validated by the offline structural check");
  }

  const auto sbat =
      readPeSbatSection(bytes, peOffset, optionalOffset, optionalSize);
  for (const auto& [component, minimum] : database.minimumSbatGeneration) {
    const auto observed = sbat.find(component);
    if (observed != sbat.end() && observed->second < minimum) {
      result.disposition = SecureBootDisposition::RevokedSbat;
      result.findings.push_back(component + " SBAT generation " +
                                std::to_string(observed->second) +
                                " is below offline minimum " +
                                std::to_string(minimum));
    }
  }
  if (!sbat.empty()) {
    result.findings.emplace_back("Embedded SBAT component generations were checked");
  }
  if (result.disposition == SecureBootDisposition::Unknown) {
    result.disposition = result.authenticodePresent
                             ? SecureBootDisposition::Compatible
                             : SecureBootDisposition::Unsigned;
  }
  return result;
}

SecureBootAnalysisResult SecureBootAnalyzer::analyze(
    const ImageInfo& image, const SecureBootDatabase& database,
    const SecureBootAnalysisCancelCallback& isCancelled) const {
  SecureBootAnalysisResult result;
  result.databaseSource = database.source;
  result.databaseVersion = database.version;
  result.databasePublishedDate = database.publishedDate;
  result.sbatVersion = database.sbatVersion;
  if (image.format != ImageFormat::Iso || !image.capabilities.isoExtraction) {
    result.error =
        "Secure Boot analysis requires a readable ISO-9660, Joliet, or UDF image";
    return result;
  }
  const auto path = std::filesystem::u8path(image.path);
  auto contents = detail::readIso9660Contents(path);
  if (!contents.valid && image.capabilities.udf) {
    const auto udf = detail::readUdfContents(path);
    contents.files = udf.files;
    contents.warnings.insert(contents.warnings.end(), udf.warnings.begin(),
                             udf.warnings.end());
    contents.valid = udf.valid;
  }
  if (!contents.valid) {
    result.error = "Unable to enumerate EFI files from the optical image";
    return result;
  }
  result.warnings = contents.warnings;
  for (const auto& file : contents.files) {
    if (!isEfiPath(file.path)) {
      continue;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Secure Boot analysis was cancelled";
      return result;
    }
    if (file.sizeBytes > kMaximumEfiImageBytes ||
        file.sizeBytes > std::numeric_limits<std::size_t>::max()) {
      result.warnings.push_back(file.path +
                                " exceeds the bounded EFI inspection limit");
      continue;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(file.sizeBytes));
    std::string readError;
    if (!bytes.empty() &&
        !detail::readImageFile(path, file, 0U, bytes.data(), bytes.size(),
                               readError)) {
      result.warnings.push_back("Unable to read " + file.path + ": " +
                                readError);
      continue;
    }
    result.images.push_back(analyzeEfiImage(file.path, bytes, database));
  }
  if (result.images.empty()) {
    result.warnings.emplace_back("No EFI executable was found in the image");
  }
  result.success = true;
  return result;
}

bool SecureBootAnalysisResult::hasRevokedImage() const noexcept {
  return std::any_of(images.begin(), images.end(), [](const auto& image) {
    return image.disposition == SecureBootDisposition::RevokedHash ||
           image.disposition == SecureBootDisposition::RevokedSbat;
  });
}

std::string SecureBootAnalysisResult::toText() const {
  std::ostringstream output;
  output << "Offline Secure Boot analysis\n\nDatabase: " << databaseSource
         << '\n';
  if (!databaseVersion.empty()) {
    output << "DBX version: " << databaseVersion;
    if (!databasePublishedDate.empty()) {
      output << " (published " << databasePublishedDate << ')';
    }
    output << '\n';
  }
  if (!sbatVersion.empty()) {
    output << "SBAT policy: " << sbatVersion << '\n';
  }
  if (!success) {
    output << "Result: " << (cancelled ? "cancelled" : "failed") << "\n"
           << error << '\n';
    return output.str();
  }
  output << "Result: "
         << (hasRevokedImage() ? "revoked loader detected" : "no known revocation detected")
         << "\n";
  for (const auto& image : images) {
    output << "\n" << image.path << "\n  "
           << secureBootDispositionName(image.disposition)
           << "\n  SHA-256: " << image.sha256 << '\n';
    if (!image.authenticodeSha256.empty()) {
      output << "  UEFI/Authenticode SHA-256: " << image.authenticodeSha256
             << '\n';
    }
    for (const auto& finding : image.findings) {
      output << "  - " << finding << '\n';
    }
  }
  for (const auto& warning : warnings) {
    output << "\nWarning: " << warning;
  }
  output << '\n';
  return output.str();
}

const char* secureBootDispositionName(
    const SecureBootDisposition disposition) noexcept {
  switch (disposition) {
    case SecureBootDisposition::Compatible:
      return "Certificate table present; no offline hash/SBAT revocation detected";
    case SecureBootDisposition::Unsigned:
      return "Unsigned";
    case SecureBootDisposition::RevokedHash:
      return "Revoked exact hash";
    case SecureBootDisposition::RevokedSbat:
      return "Revoked by SBAT generation";
    case SecureBootDisposition::Unknown:
      return "Unknown";
    case SecureBootDisposition::Invalid:
      return "Invalid EFI PE image";
  }
  return "Unknown";
}

}  // namespace rufus::core
