/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "compressed_image_source.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include "rufus/core/raw_image_writer.hpp"

namespace rufus::core {

namespace {

constexpr std::size_t kDecoderInputBytes = 256U * 1024U;
constexpr std::uint64_t kXzMemoryLimit = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumZipMetadataBytes = 64ULL * 1024ULL * 1024ULL;

std::uint16_t littleEndian16(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(data[1]) << 8U;
}

std::uint32_t littleEndian32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) |
         static_cast<std::uint32_t>(data[1]) << 8U |
         static_cast<std::uint32_t>(data[2]) << 16U |
         static_cast<std::uint32_t>(data[3]) << 24U;
}

std::uint32_t bigEndian32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) << 24U |
         static_cast<std::uint32_t>(data[1]) << 16U |
         static_cast<std::uint32_t>(data[2]) << 8U |
         static_cast<std::uint32_t>(data[3]);
}

std::uint64_t bigEndian64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value = value << 8U | data[index];
  }
  return value;
}

bool readAt(std::ifstream& input, const std::uint64_t offset,
            unsigned char* data, const std::size_t size) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<std::uint64_t>(
                 std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  input.read(reinterpret_cast<char*>(data),
             static_cast<std::streamsize>(size));
  return input.gcount() == static_cast<std::streamsize>(size);
}

class Decoder {
 public:
  virtual ~Decoder() = default;
  virtual bool reset(std::string& error) = 0;
  virtual bool read(unsigned char* data, std::size_t capacity,
                    std::size_t& produced, bool& eof,
                    std::string& error) = 0;
};

class GzipDecoder final : public Decoder {
 public:
  explicit GzipDecoder(std::filesystem::path path) : path_(std::move(path)) {}
  ~GzipDecoder() override { close(); }

  bool reset(std::string& error) override {
    close();
#ifdef _WIN32
    file_ = gzopen_w(path_.c_str(), L"rb");
#else
    file_ = gzopen(path_.c_str(), "rb");
#endif
    if (file_ == nullptr) {
      error = "Unable to open the gzip image";
      return false;
    }
    if (gzrewind(file_) != 0) {
      error = "Unable to rewind the gzip image";
      close();
      return false;
    }
    return true;
  }

  bool read(unsigned char* data, const std::size_t capacity,
            std::size_t& produced, bool& eof,
            std::string& error) override {
    produced = 0;
    eof = false;
    if (file_ == nullptr) {
      error = "The gzip decoder is not initialized";
      return false;
    }
    const unsigned int requested = static_cast<unsigned int>(
        std::min<std::size_t>(capacity, static_cast<std::size_t>(INT_MAX)));
    const int count = gzread(file_, data, requested);
    if (count < 0) {
      int status = Z_OK;
      const char* message = gzerror(file_, &status);
      error = "gzip decompression failed";
      if (message != nullptr && *message != '\0') {
        error += ": " + std::string(message);
      }
      return false;
    }
    produced = static_cast<std::size_t>(count);
    if (count == 0) {
      int status = Z_OK;
      const char* message = gzerror(file_, &status);
      if (status != Z_OK && status != Z_STREAM_END) {
        error = "gzip decompression failed";
        if (message != nullptr && *message != '\0') {
          error += ": " + std::string(message);
        }
        return false;
      }
      eof = gzeof(file_) != 0;
      if (!eof) {
        error = "gzip decoder stopped before the end of the stream";
        return false;
      }
    }
    return true;
  }

 private:
  void close() {
    if (file_ != nullptr) {
      static_cast<void>(gzclose(file_));
      file_ = nullptr;
    }
  }

  std::filesystem::path path_;
  gzFile file_{};
};

class Bzip2Decoder final : public Decoder {
 public:
  explicit Bzip2Decoder(std::filesystem::path path) : path_(std::move(path)) {}
  ~Bzip2Decoder() override { close(); }

  bool reset(std::string& error) override {
    close();
#if defined(_WIN32)
    file_ = _wfopen(path_.c_str(), L"rb");
#else
    file_ = std::fopen(path_.c_str(), "rb");
#endif
    if (file_ == nullptr) {
      error = "Unable to open the bzip2 image";
      return false;
    }
    int status = BZ_OK;
    stream_ = BZ2_bzReadOpen(&status, file_, 0, 0, nullptr, 0);
    if (status != BZ_OK || stream_ == nullptr) {
      error = "Unable to initialize the bzip2 decoder";
      close();
      return false;
    }
    streamEnded_ = false;
    return true;
  }

  bool read(unsigned char* data, const std::size_t capacity,
            std::size_t& produced, bool& eof,
            std::string& error) override {
    produced = 0;
    eof = streamEnded_;
    if (stream_ == nullptr) {
      error = "The bzip2 decoder is not initialized";
      return false;
    }
    if (streamEnded_) {
      return true;
    }
    int status = BZ_OK;
    const int requested = static_cast<int>(
        std::min<std::size_t>(capacity, static_cast<std::size_t>(INT_MAX)));
    const int count = BZ2_bzRead(&status, stream_, data, requested);
    if (status != BZ_OK && status != BZ_STREAM_END) {
      error = "bzip2 decompression failed with status " +
              std::to_string(status);
      return false;
    }
    produced = count < 0 ? 0U : static_cast<std::size_t>(count);
    streamEnded_ = status == BZ_STREAM_END;
    eof = streamEnded_;
    return true;
  }

 private:
  void close() {
    if (stream_ != nullptr) {
      int status = BZ_OK;
      BZ2_bzReadClose(&status, stream_);
      stream_ = nullptr;
    }
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  std::filesystem::path path_;
  std::FILE* file_{};
  BZFILE* stream_{};
  bool streamEnded_{};
};

class XzDecoder final : public Decoder {
 public:
  explicit XzDecoder(std::filesystem::path path, const bool lzmaAlone = false)
      : path_(std::move(path)), lzmaAlone_(lzmaAlone) {}
  ~XzDecoder() override { endDecoder(); }

  bool reset(std::string& error) override {
    endDecoder();
    input_.close();
    input_.clear();
    input_.open(path_, std::ios::binary);
    if (!input_) {
      error = lzmaAlone_ ? "Unable to open the LZMA image"
                         : "Unable to open the XZ image";
      return false;
    }
    input_.seekg(0, std::ios::beg);
    if (!input_) {
      error = lzmaAlone_ ? "Unable to rewind the LZMA image"
                         : "Unable to rewind the XZ image";
      return false;
    }
    lzma_stream fresh = LZMA_STREAM_INIT;
    stream_ = fresh;
    const lzma_ret status = lzmaAlone_
                                ? lzma_alone_decoder(&stream_, kXzMemoryLimit)
                                : lzma_stream_decoder(
                                      &stream_, kXzMemoryLimit,
                                      LZMA_CONCATENATED);
    if (status != LZMA_OK) {
      error = status == LZMA_MEMLIMIT_ERROR
                  ? "The compressed image requires more than 1 GiB of decoder memory"
                  : lzmaAlone_ ? "Unable to initialize the LZMA decoder"
                               : "Unable to initialize the XZ decoder";
      return false;
    }
    initialized_ = true;
    inputEnded_ = false;
    streamEnded_ = false;
    return true;
  }

  bool read(unsigned char* data, const std::size_t capacity,
            std::size_t& produced, bool& eof,
            std::string& error) override {
    produced = 0;
    eof = streamEnded_;
    if (!initialized_) {
      error = "The XZ decoder is not initialized";
      return false;
    }
    if (streamEnded_) {
      return true;
    }
    stream_.next_out = data;
    stream_.avail_out = capacity;
    while (stream_.avail_out != 0U && !streamEnded_) {
      if (stream_.avail_in == 0U && !inputEnded_) {
        input_.read(reinterpret_cast<char*>(inputBuffer_.data()),
                    static_cast<std::streamsize>(inputBuffer_.size()));
        const auto count = input_.gcount();
        if (count < 0 || (input_.bad() && !input_.eof())) {
          error = lzmaAlone_ ? "Unable to read the LZMA image"
                             : "Unable to read the XZ image";
          return false;
        }
        stream_.next_in = inputBuffer_.data();
        stream_.avail_in = static_cast<std::size_t>(count);
        inputEnded_ = input_.eof();
      }
      const std::size_t beforeInput = stream_.avail_in;
      const std::size_t beforeOutput = stream_.avail_out;
      const lzma_ret status =
          lzma_code(&stream_, inputEnded_ ? LZMA_FINISH : LZMA_RUN);
      if (status == LZMA_STREAM_END) {
        streamEnded_ = true;
        break;
      }
      if (status != LZMA_OK) {
        error = status == LZMA_MEMLIMIT_ERROR
                    ? "The XZ image exceeds the 1 GiB decoder-memory limit"
                    : std::string(lzmaAlone_ ? "LZMA" : "XZ") +
                          " decompression failed with status " +
                          std::to_string(static_cast<int>(status));
        return false;
      }
      if (beforeInput == stream_.avail_in &&
          beforeOutput == stream_.avail_out && inputEnded_) {
        error = lzmaAlone_ ? "The LZMA stream is truncated"
                           : "The XZ stream is truncated";
        return false;
      }
    }
    produced = capacity - stream_.avail_out;
    eof = streamEnded_;
    return true;
  }

 private:
  void endDecoder() {
    if (initialized_) {
      lzma_end(&stream_);
      initialized_ = false;
    }
  }

  std::filesystem::path path_;
  std::ifstream input_;
  std::array<unsigned char, kDecoderInputBytes> inputBuffer_{};
  lzma_stream stream_ = LZMA_STREAM_INIT;
  bool initialized_{};
  bool inputEnded_{};
  bool streamEnded_{};
  bool lzmaAlone_{};
};

class ZstdDecoder final : public Decoder {
 public:
  explicit ZstdDecoder(std::filesystem::path path)
      : path_(std::move(path)), stream_(ZSTD_createDStream()) {}
  ~ZstdDecoder() override { ZSTD_freeDStream(stream_); }

  bool reset(std::string& error) override {
    input_.close();
    input_.clear();
    input_.open(path_, std::ios::binary);
    if (!input_) {
      error = "Unable to open the Zstandard image";
      return false;
    }
    input_.seekg(0, std::ios::beg);
    if (!input_) {
      error = "Unable to rewind the Zstandard image";
      return false;
    }
    if (stream_ == nullptr) {
      error = "Unable to allocate the Zstandard decoder";
      return false;
    }
    const std::size_t status = ZSTD_initDStream(stream_);
    if (ZSTD_isError(status) != 0U) {
      error = "Unable to initialize the Zstandard decoder: " +
              std::string(ZSTD_getErrorName(status));
      return false;
    }
    inputPosition_ = 0;
    inputSize_ = 0;
    inputEnded_ = false;
    streamEnded_ = false;
    sawFrame_ = false;
    remainingHint_ = 1;
    return true;
  }

  bool read(unsigned char* data, const std::size_t capacity,
            std::size_t& produced, bool& eof,
            std::string& error) override {
    produced = 0;
    eof = streamEnded_;
    if (stream_ == nullptr) {
      error = "The Zstandard decoder is not initialized";
      return false;
    }
    if (streamEnded_) {
      return true;
    }

    ZSTD_outBuffer output{data, capacity, 0};
    while (output.pos < output.size && !streamEnded_) {
      if (inputPosition_ == inputSize_ && !inputEnded_) {
        input_.read(reinterpret_cast<char*>(inputBuffer_.data()),
                    static_cast<std::streamsize>(inputBuffer_.size()));
        const auto count = input_.gcount();
        if (count < 0 || (input_.bad() && !input_.eof())) {
          error = "Unable to read the Zstandard image";
          return false;
        }
        inputPosition_ = 0;
        inputSize_ = static_cast<std::size_t>(count);
        inputEnded_ = input_.eof();
      }
      if (inputPosition_ == inputSize_ && inputEnded_) {
        if (!sawFrame_ || remainingHint_ != 0U) {
          error = "The Zstandard stream is truncated";
          return false;
        }
        streamEnded_ = true;
        break;
      }

      ZSTD_inBuffer inputBuffer{inputBuffer_.data(), inputSize_, inputPosition_};
      const std::size_t beforeInput = inputBuffer.pos;
      const std::size_t beforeOutput = output.pos;
      remainingHint_ = ZSTD_decompressStream(stream_, &output, &inputBuffer);
      inputPosition_ = inputBuffer.pos;
      if (ZSTD_isError(remainingHint_) != 0U) {
        error = "Zstandard decompression failed: " +
                std::string(ZSTD_getErrorName(remainingHint_));
        return false;
      }
      sawFrame_ = true;
      if (beforeInput == inputBuffer.pos && beforeOutput == output.pos) {
        error = "The Zstandard decoder made no progress";
        return false;
      }
    }
    produced = output.pos;
    eof = streamEnded_;
    return true;
  }

 private:
  std::filesystem::path path_;
  std::ifstream input_;
  std::array<unsigned char, kDecoderInputBytes> inputBuffer_{};
  ZSTD_DStream* stream_{};
  std::size_t inputPosition_{};
  std::size_t inputSize_{};
  std::size_t remainingHint_{1};
  bool inputEnded_{};
  bool streamEnded_{};
  bool sawFrame_{};
};

struct ZipEntry final {
  std::uint64_t dataOffset{};
  std::uint64_t compressedSize{};
  std::uint64_t uncompressedSize{};
  std::uint32_t checksum{};
  std::uint16_t method{};
};

bool parseZipEntry(const std::filesystem::path& path, ZipEntry& selected,
                   std::string& error) {
  std::error_code fileError;
  const std::uint64_t fileSize = std::filesystem::file_size(path, fileError);
  if (fileError || fileSize < 22U) {
    error = fileError ? "Unable to inspect the ZIP image: " + fileError.message()
                      : "The ZIP end record is missing";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  const std::size_t tailSize = static_cast<std::size_t>(
      std::min<std::uint64_t>(fileSize, 65557U));
  std::vector<unsigned char> tail(tailSize);
  if (!input || !readAt(input, fileSize - tailSize, tail.data(), tail.size())) {
    error = "Unable to read the ZIP end record";
    return false;
  }
  std::size_t endOffset = std::string::npos;
  for (std::size_t offset = tail.size() - 22U;; --offset) {
    if (littleEndian32(tail.data() + offset) == 0x06054b50U &&
        offset + 22U + littleEndian16(tail.data() + offset + 20U) ==
            tail.size()) {
      endOffset = offset;
      break;
    }
    if (offset == 0U) {
      break;
    }
  }
  if (endOffset == std::string::npos) {
    error = "The ZIP end record is invalid";
    return false;
  }
  const unsigned char* end = tail.data() + endOffset;
  const std::uint16_t entryCount = littleEndian16(end + 10U);
  const std::uint32_t directorySize = littleEndian32(end + 12U);
  const std::uint32_t directoryOffset = littleEndian32(end + 16U);
  const std::uint64_t absoluteEnd = fileSize - tailSize + endOffset;
  if (littleEndian16(end + 4U) != 0U || littleEndian16(end + 6U) != 0U ||
      littleEndian16(end + 8U) != entryCount || entryCount == 0xffffU ||
      directorySize == 0xffffffffU || directoryOffset == 0xffffffffU ||
      directorySize > kMaximumZipMetadataBytes || directoryOffset > fileSize ||
      directorySize > fileSize - directoryOffset ||
      static_cast<std::uint64_t>(directoryOffset) + directorySize !=
          absoluteEnd) {
    error = "ZIP64, split, or oversized archives are not supported";
    return false;
  }
  std::vector<unsigned char> directory(directorySize);
  if (!readAt(input, directoryOffset, directory.data(), directory.size())) {
    error = "Unable to read the ZIP central directory";
    return false;
  }

  std::size_t offset = 0;
  std::size_t parsed = 0;
  std::size_t regularFiles = 0;
  while (offset < directory.size()) {
    if (directory.size() - offset < 46U ||
        littleEndian32(directory.data() + offset) != 0x02014b50U) {
      error = "The ZIP central directory is malformed";
      return false;
    }
    const unsigned char* entry = directory.data() + offset;
    const std::uint16_t flags = littleEndian16(entry + 8U);
    const std::uint16_t method = littleEndian16(entry + 10U);
    const std::uint32_t checksum = littleEndian32(entry + 16U);
    const std::uint32_t compressedSize = littleEndian32(entry + 20U);
    const std::uint32_t uncompressedSize = littleEndian32(entry + 24U);
    const std::size_t nameLength = littleEndian16(entry + 28U);
    const std::size_t extraLength = littleEndian16(entry + 30U);
    const std::size_t commentLength = littleEndian16(entry + 32U);
    const std::uint16_t disk = littleEndian16(entry + 34U);
    const std::uint32_t localOffset = littleEndian32(entry + 42U);
    const std::size_t recordSize = 46U + nameLength + extraLength + commentLength;
    if (recordSize > directory.size() - offset || disk != 0U ||
        compressedSize == 0xffffffffU || uncompressedSize == 0xffffffffU ||
        localOffset == 0xffffffffU) {
      error = "A ZIP entry is truncated or requires ZIP64";
      return false;
    }
    const bool directoryEntry = nameLength != 0U &&
                                entry[46U + nameLength - 1U] == '/';
    if (!directoryEntry) {
      if (++regularFiles != 1U) {
        error = "A compressed disk-image ZIP must contain exactly one regular file";
        return false;
      }
      if ((flags & 0x0001U) != 0U) {
        error = "Encrypted ZIP images are not supported";
        return false;
      }
      if (method != 0U && method != 8U) {
        error = "The ZIP image uses an unsupported compression method";
        return false;
      }
      if (method == 0U && compressedSize != uncompressedSize) {
        error = "A stored ZIP entry has inconsistent sizes";
        return false;
      }
      std::array<unsigned char, 30> local{};
      if (!readAt(input, localOffset, local.data(), local.size()) ||
          littleEndian32(local.data()) != 0x04034b50U ||
          littleEndian16(local.data() + 6U) != flags ||
          littleEndian16(local.data() + 8U) != method) {
        error = "The ZIP local header does not match its central directory";
        return false;
      }
      const std::uint64_t dataOffset =
          static_cast<std::uint64_t>(localOffset) + local.size() +
          littleEndian16(local.data() + 26U) +
          littleEndian16(local.data() + 28U);
      if (dataOffset > directoryOffset ||
          compressedSize > directoryOffset - dataOffset) {
        error = "The ZIP payload overlaps or exceeds its central directory";
        return false;
      }
      selected = {dataOffset, compressedSize, uncompressedSize, checksum, method};
    }
    ++parsed;
    offset += recordSize;
  }
  if (parsed != entryCount || regularFiles != 1U) {
    error = regularFiles == 0U
                ? "The ZIP archive contains no disk image"
                : "The ZIP entry count does not match its end record";
    return false;
  }
  return true;
}

class ZipDecoder final : public Decoder {
 public:
  ZipDecoder(std::filesystem::path path, const ZipEntry entry)
      : path_(std::move(path)), entry_(entry) {}
  ~ZipDecoder() override { endInflater(); }

  bool reset(std::string& error) override {
    endInflater();
    input_.close();
    input_.clear();
    input_.open(path_, std::ios::binary);
    if (!input_) {
      error = "Unable to open the ZIP image";
      return false;
    }
    input_.seekg(static_cast<std::streamoff>(entry_.dataOffset), std::ios::beg);
    if (!input_) {
      error = "Unable to seek to the ZIP image payload";
      return false;
    }
    compressedRemaining_ = entry_.compressedSize;
    outputBytes_ = 0;
    checksum_ = ::crc32(0L, Z_NULL, 0);
    streamEnded_ = false;
    if (entry_.method == 8U) {
      stream_ = {};
      const int status = inflateInit2(&stream_, -MAX_WBITS);
      if (status != Z_OK) {
        error = "Unable to initialize the ZIP deflate decoder";
        return false;
      }
      initialized_ = true;
    }
    return true;
  }

  bool read(unsigned char* data, const std::size_t capacity,
            std::size_t& produced, bool& eof,
            std::string& error) override {
    produced = 0;
    eof = streamEnded_;
    if (streamEnded_) {
      return true;
    }
    if (entry_.method == 0U) {
      const std::size_t requested = static_cast<std::size_t>(
          std::min<std::uint64_t>(capacity, compressedRemaining_));
      input_.read(reinterpret_cast<char*>(data),
                  static_cast<std::streamsize>(requested));
      if (input_.gcount() != static_cast<std::streamsize>(requested)) {
        error = "The stored ZIP image payload is truncated";
        return false;
      }
      compressedRemaining_ -= requested;
      produced = requested;
      outputBytes_ += requested;
      checksum_ = ::crc32(checksum_, data, static_cast<uInt>(requested));
      if (compressedRemaining_ == 0U) {
        streamEnded_ = true;
      }
    } else {
      stream_.next_out = data;
      stream_.avail_out = static_cast<uInt>(std::min<std::size_t>(
          capacity, static_cast<std::size_t>(std::numeric_limits<uInt>::max())));
      const std::size_t outputCapacity = stream_.avail_out;
      while (stream_.avail_out != 0U && !streamEnded_) {
        if (stream_.avail_in == 0U && compressedRemaining_ != 0U) {
          const std::size_t requested = static_cast<std::size_t>(
              std::min<std::uint64_t>(inputBuffer_.size(), compressedRemaining_));
          input_.read(reinterpret_cast<char*>(inputBuffer_.data()),
                      static_cast<std::streamsize>(requested));
          if (input_.gcount() != static_cast<std::streamsize>(requested)) {
            error = "The deflated ZIP image payload is truncated";
            return false;
          }
          compressedRemaining_ -= requested;
          stream_.next_in = inputBuffer_.data();
          stream_.avail_in = static_cast<uInt>(requested);
        }
        const uInt beforeInput = stream_.avail_in;
        const uInt beforeOutput = stream_.avail_out;
        const int status = inflate(&stream_, Z_NO_FLUSH);
        if (status == Z_STREAM_END) {
          if (compressedRemaining_ != 0U || stream_.avail_in != 0U) {
            error = "The ZIP deflate stream ends before its declared payload";
            return false;
          }
          streamEnded_ = true;
          break;
        }
        if (status != Z_OK) {
          error = "ZIP deflate decompression failed";
          if (stream_.msg != nullptr) {
            error += ": " + std::string(stream_.msg);
          }
          return false;
        }
        if (beforeInput == stream_.avail_in && beforeOutput == stream_.avail_out) {
          error = "The ZIP deflate stream is truncated";
          return false;
        }
      }
      produced = outputCapacity - stream_.avail_out;
      outputBytes_ += produced;
      checksum_ = ::crc32(checksum_, data, static_cast<uInt>(produced));
    }
    if (streamEnded_) {
      if (outputBytes_ != entry_.uncompressedSize) {
        error = "The ZIP image expanded to a different size than declared";
        return false;
      }
      if (checksum_ != entry_.checksum) {
        error = "The ZIP image payload checksum is invalid";
        return false;
      }
      eof = true;
    }
    return true;
  }

 private:
  void endInflater() {
    if (initialized_) {
      inflateEnd(&stream_);
      initialized_ = false;
    }
  }

  std::filesystem::path path_;
  ZipEntry entry_;
  std::ifstream input_;
  std::array<unsigned char, kDecoderInputBytes> inputBuffer_{};
  z_stream stream_{};
  std::uint64_t compressedRemaining_{};
  std::uint64_t outputBytes_{};
  uLong checksum_{};
  bool initialized_{};
  bool streamEnded_{};
};

std::unique_ptr<Decoder> makeDecoder(const std::filesystem::path& path,
                                     const ImageFormat format,
                                     std::string& error) {
  std::unique_ptr<Decoder> decoder;
  switch (format) {
    case ImageFormat::Gzip:
      decoder = std::make_unique<GzipDecoder>(path);
      break;
    case ImageFormat::Bzip2:
      decoder = std::make_unique<Bzip2Decoder>(path);
      break;
    case ImageFormat::Lzma:
      decoder = std::make_unique<XzDecoder>(path, true);
      break;
    case ImageFormat::Xz:
      decoder = std::make_unique<XzDecoder>(path);
      break;
    case ImageFormat::Zstd:
      decoder = std::make_unique<ZstdDecoder>(path);
      break;
    case ImageFormat::Zip: {
      ZipEntry entry;
      if (!parseZipEntry(path, entry, error)) {
        return nullptr;
      }
      decoder = std::make_unique<ZipDecoder>(path, entry);
      break;
    }
    default:
      error = "The selected image is not a supported compressed format";
      return nullptr;
  }
  if (!decoder->reset(error)) {
    return nullptr;
  }
  return decoder;
}

class FileRawSource final : public RawSourceIo {
 public:
  FileRawSource(const std::filesystem::path& path,
                const std::uint64_t expectedFileSize,
                const std::uint64_t payloadOffset = 0U,
                const std::uint64_t payloadSize = 0U)
      : stream_(path, std::ios::binary | std::ios::ate) {
    if (stream_) {
      const auto end = stream_.tellg();
      const std::uint64_t viewSize = payloadSize == 0U
                                         ? expectedFileSize
                                         : payloadSize;
      if (end >= 0 && static_cast<std::uint64_t>(end) == expectedFileSize &&
          payloadOffset <= expectedFileSize &&
          viewSize <= expectedFileSize - payloadOffset) {
        offset_ = payloadOffset;
        size_ = viewSize;
        valid_ = true;
      }
    }
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override { return size_; }

  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    if (offset > size_ || size > size_ - offset) {
      error = "Source-image transfer is out of range";
      return false;
    }
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset_ + offset), std::ios::beg);
    if (!stream_) {
      error = "Unable to seek in the source image";
      return false;
    }
    stream_.read(reinterpret_cast<char*>(data),
                 static_cast<std::streamsize>(size));
    if (stream_.gcount() != static_cast<std::streamsize>(size)) {
      error = "The source image changed or could not be read completely";
      return false;
    }
    return true;
  }

 private:
  std::ifstream stream_;
  std::uint64_t offset_{};
  std::uint64_t size_{};
  bool valid_{};
};

class DynamicVhdRawSource final : public RawSourceIo {
 public:
  DynamicVhdRawSource(const std::filesystem::path& path,
                      const std::uint64_t expectedFileSize,
                      const std::uint64_t expectedVirtualSize)
      : stream_(path, std::ios::binary | std::ios::ate),
        fileSize_(expectedFileSize), size_(expectedVirtualSize) {
    if (!stream_ || expectedFileSize < 2048U || expectedVirtualSize == 0U ||
        stream_.tellg() < 0 ||
        static_cast<std::uint64_t>(stream_.tellg()) != expectedFileSize) {
      return;
    }
    std::array<unsigned char, 512> footer{};
    if (!::rufus::core::readAt(stream_, expectedFileSize - footer.size(),
                               footer.data(), footer.size()) ||
        !std::equal(footer.begin(), footer.begin() + 8U,
                    std::string_view("conectix").begin()) ||
        bigEndian32(footer.data() + 60U) != 3U ||
        bigEndian64(footer.data() + 48U) != expectedVirtualSize) {
      return;
    }
    const std::uint64_t headerOffset = bigEndian64(footer.data() + 16U);
    std::array<unsigned char, 1024> header{};
    if (!::rufus::core::readAt(stream_, headerOffset, header.data(),
                               header.size()) ||
        !std::equal(header.begin(), header.begin() + 8U,
                    std::string_view("cxsparse").begin())) {
      return;
    }
    const std::uint64_t tableOffset = bigEndian64(header.data() + 16U);
    const std::uint32_t tableEntries = bigEndian32(header.data() + 28U);
    blockSize_ = bigEndian32(header.data() + 32U);
    if (blockSize_ < 512U || blockSize_ % 512U != 0U ||
        (blockSize_ & (blockSize_ - 1U)) != 0U || tableEntries == 0U) {
      return;
    }
    const std::uint64_t requiredEntries =
        (size_ - 1U) / blockSize_ + 1U;
    const std::uint64_t tableBytes =
        static_cast<std::uint64_t>(tableEntries) * 4U;
    if (requiredEntries > tableEntries ||
        requiredEntries > std::numeric_limits<std::size_t>::max() ||
        tableBytes > 64ULL * 1024ULL * 1024ULL ||
        tableOffset > fileSize_ || tableBytes > fileSize_ - tableOffset) {
      return;
    }
    std::vector<unsigned char> table(static_cast<std::size_t>(tableBytes));
    if (!::rufus::core::readAt(stream_, tableOffset, table.data(), table.size())) {
      return;
    }
    bitmapBytes_ =
        (((static_cast<std::uint64_t>(blockSize_) / 512U + 7U) / 8U + 511U) /
         512U) * 512U;
    bat_.resize(static_cast<std::size_t>(requiredEntries));
    const std::uint64_t tableEnd =
        ((tableOffset + tableBytes + 511U) / 512U) * 512U;
    for (std::size_t index = 0; index < bat_.size(); ++index) {
      bat_[index] = bigEndian32(table.data() + index * 4U);
      if (bat_[index] == 0xffffffffU) {
        continue;
      }
      const std::uint64_t blockOffset =
          static_cast<std::uint64_t>(bat_[index]) * 512U;
      if (blockOffset < tableEnd || blockOffset >= fileSize_ - 512U ||
          bitmapBytes_ > fileSize_ - 512U - blockOffset ||
          blockSize_ > fileSize_ - 512U - blockOffset - bitmapBytes_) {
        bat_.clear();
        return;
      }
    }
    valid_ = true;
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override { return size_; }

  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    if (!valid_ || offset > size_ || size > size_ - offset) {
      error = "Dynamic VHD transfer is out of range";
      return false;
    }
    std::size_t completed = 0;
    while (completed < size) {
      const std::uint64_t virtualOffset = offset + completed;
      const std::size_t blockIndex =
          static_cast<std::size_t>(virtualOffset / blockSize_);
      const std::uint64_t withinBlock = virtualOffset % blockSize_;
      const std::uint64_t remainingInBlock = blockSize_ - withinBlock;
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uint64_t>(size - completed, remainingInBlock));
      if (bat_[blockIndex] == 0xffffffffU) {
        std::fill_n(data + completed, amount, 0);
        completed += amount;
        continue;
      }
      if (!loadBitmap(blockIndex, error)) {
        return false;
      }
      std::size_t withinAmount = 0;
      while (withinAmount < amount) {
        const std::uint64_t position = withinBlock + withinAmount;
        const std::uint64_t sector = position / 512U;
        const bool present = sectorPresent(sector);
        std::uint64_t runEnd = std::min<std::uint64_t>(
            blockSize_, ((sector + 1U) * 512U));
        while (runEnd < withinBlock + amount &&
               sectorPresent(runEnd / 512U) == present) {
          runEnd += 512U;
        }
        runEnd = std::min<std::uint64_t>(runEnd, withinBlock + amount);
        const std::size_t runBytes =
            static_cast<std::size_t>(runEnd - position);
        if (present) {
          const std::uint64_t physicalOffset =
              static_cast<std::uint64_t>(bat_[blockIndex]) * 512U +
              bitmapBytes_ + position;
          if (!::rufus::core::readAt(stream_, physicalOffset,
                                     data + completed + withinAmount,
                                     runBytes)) {
            error = "The dynamic VHD changed or could not be read completely";
            return false;
          }
        } else {
          std::fill_n(data + completed + withinAmount, runBytes, 0);
        }
        withinAmount += runBytes;
      }
      completed += amount;
    }
    return true;
  }

 private:
  bool loadBitmap(const std::size_t blockIndex, std::string& error) {
    if (cachedBlock_ == blockIndex) {
      return true;
    }
    if (bitmapBytes_ > std::numeric_limits<std::size_t>::max()) {
      error = "The dynamic VHD block bitmap is too large";
      return false;
    }
    bitmap_.resize(static_cast<std::size_t>(bitmapBytes_));
    const std::uint64_t offset =
        static_cast<std::uint64_t>(bat_[blockIndex]) * 512U;
    if (!::rufus::core::readAt(stream_, offset, bitmap_.data(), bitmap_.size())) {
      error = "Unable to read the dynamic VHD block bitmap";
      return false;
    }
    cachedBlock_ = blockIndex;
    return true;
  }

  [[nodiscard]] bool sectorPresent(const std::uint64_t sector) const {
    const std::size_t byte = static_cast<std::size_t>(sector / 8U);
    const unsigned char bit = static_cast<unsigned char>(0x80U >> (sector % 8U));
    return byte < bitmap_.size() && (bitmap_[byte] & bit) != 0U;
  }

  std::ifstream stream_;
  std::uint64_t fileSize_{};
  std::uint64_t size_{};
  std::uint64_t bitmapBytes_{};
  std::uint32_t blockSize_{};
  std::vector<std::uint32_t> bat_;
  std::vector<unsigned char> bitmap_;
  std::size_t cachedBlock_{std::numeric_limits<std::size_t>::max()};
  bool valid_{};
};

class DynamicVhdxRawSource final : public RawSourceIo {
 public:
  DynamicVhdxRawSource(const std::filesystem::path& path,
                       const ImageInfo& image)
      : stream_(path, std::ios::binary | std::ios::ate),
        fileSize_(image.sizeBytes),
        size_(image.containerPayloadSizeBytes),
        tableOffset_(image.containerAllocationTableOffsetBytes),
        blockSize_(image.containerBlockSizeBytes),
        logicalSectorSize_(image.containerLogicalSectorSize) {
    if (!stream_ || stream_.tellg() < 0 ||
        static_cast<std::uint64_t>(stream_.tellg()) != fileSize_ || size_ == 0U ||
        tableOffset_ == 0U || blockSize_ < 1024U * 1024U ||
        (blockSize_ & (blockSize_ - 1U)) != 0U ||
        (logicalSectorSize_ != 512U && logicalSectorSize_ != 4096U)) {
      return;
    }
    blockCount_ = (size_ - 1U) / blockSize_ + 1U;
    chunkRatio_ = (static_cast<std::uint64_t>(1U) << 23U) *
                  logicalSectorSize_ / blockSize_;
    if (chunkRatio_ == 0U) {
      return;
    }
    const std::uint64_t highestBatIndex =
        blockCount_ - 1U + (blockCount_ - 1U) / chunkRatio_;
    const std::uint64_t tableBytes = (highestBatIndex + 1U) * 8U;
    if (tableBytes > 64ULL * 1024ULL * 1024ULL ||
        tableOffset_ > fileSize_ || tableBytes > fileSize_ - tableOffset_) {
      return;
    }
    bat_.resize(static_cast<std::size_t>(tableBytes));
    if (!::rufus::core::readAt(stream_, tableOffset_, bat_.data(), bat_.size())) {
      bat_.clear();
      return;
    }
    for (std::uint64_t block = 0; block < blockCount_; ++block) {
      const std::uint64_t entry = batEntry(block);
      const unsigned int state = static_cast<unsigned int>(entry & 7U);
      if (state == 0U || state == 2U || state == 3U) {
        continue;
      }
      const std::uint64_t payloadOffset = entry & 0xfffffffffff00000ULL;
      if (state != 6U || (entry & 0x00000000000ffff8ULL) != 0U ||
          payloadOffset > fileSize_ || blockSize_ > fileSize_ - payloadOffset) {
        bat_.clear();
        return;
      }
    }
    valid_ = true;
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override { return size_; }

  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    if (!valid_ || offset > size_ || size > size_ - offset) {
      error = "Dynamic VHDX transfer is out of range";
      return false;
    }
    std::size_t completed = 0;
    while (completed < size) {
      const std::uint64_t virtualOffset = offset + completed;
      const std::uint64_t block = virtualOffset / blockSize_;
      const std::uint64_t withinBlock = virtualOffset % blockSize_;
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uint64_t>(size - completed, blockSize_ - withinBlock));
      const std::uint64_t entry = batEntry(block);
      const unsigned int state = static_cast<unsigned int>(entry & 7U);
      if (state == 6U) {
        const std::uint64_t physicalOffset =
            (entry & 0xfffffffffff00000ULL) + withinBlock;
        if (!::rufus::core::readAt(stream_, physicalOffset, data + completed,
                                   amount)) {
          error = "The dynamic VHDX changed or could not be read completely";
          return false;
        }
      } else if (state == 0U || state == 2U || state == 3U) {
        std::fill_n(data + completed, amount, 0);
      } else {
        error = "The dynamic VHDX contains a non-deployable payload block";
        return false;
      }
      completed += amount;
    }
    return true;
  }

 private:
  [[nodiscard]] std::uint64_t batEntry(const std::uint64_t block) const {
    const std::uint64_t index = block + block / chunkRatio_;
    return static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U]) |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 1U]) << 8U |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 2U]) << 16U |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 3U]) << 24U |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 4U]) << 32U |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 5U]) << 40U |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 6U]) << 48U |
           static_cast<std::uint64_t>(bat_[static_cast<std::size_t>(index) * 8U + 7U]) << 56U;
  }

  std::ifstream stream_;
  std::uint64_t fileSize_{};
  std::uint64_t size_{};
  std::uint64_t tableOffset_{};
  std::uint64_t blockCount_{};
  std::uint64_t chunkRatio_{};
  std::uint32_t blockSize_{};
  std::uint32_t logicalSectorSize_{};
  std::vector<unsigned char> bat_;
  bool valid_{};
};

class CompressedRawSource final : public RawSourceIo {
 public:
  CompressedRawSource(std::unique_ptr<Decoder> decoder,
                      const std::uint64_t expandedSize)
      : decoder_(std::move(decoder)), size_(expandedSize) {}

  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override { return size_; }

  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    if (offset > size_ || size > size_ - offset) {
      error = "Decompressed source-image transfer is out of range";
      return false;
    }
    if (offset != position_) {
      if (offset != 0U) {
        error = "Compressed images support only sequential reads and verification rewind";
        return false;
      }
      if (!decoder_->reset(error)) {
        return false;
      }
      position_ = 0;
      eof_ = false;
    }

    std::size_t completed = 0;
    while (completed < size) {
      std::size_t produced = 0;
      bool decoderEof = false;
      if (!decoder_->read(data + completed, size - completed, produced,
                          decoderEof, error)) {
        return false;
      }
      completed += produced;
      position_ += produced;
      eof_ = decoderEof;
      if (produced == 0U) {
        if (decoderEof) {
          error = "The compressed image ended before its measured size";
        } else {
          error = "The compressed image decoder made no progress";
        }
        return false;
      }
    }

    if (position_ == size_ && !eof_) {
      unsigned char extra = 0;
      std::size_t produced = 0;
      bool decoderEof = false;
      if (!decoder_->read(&extra, 1, produced, decoderEof, error)) {
        return false;
      }
      if (produced != 0U || !decoderEof) {
        error = "The compressed image expands beyond its measured size";
        return false;
      }
      eof_ = true;
    }
    return true;
  }

 private:
  std::unique_ptr<Decoder> decoder_;
  std::uint64_t size_{};
  std::uint64_t position_{};
  bool eof_{};
};

}  // namespace

RawSourceOpenResult openRawImageSource(
    const ImageInfo& image, const std::filesystem::path& pathOverride) {
  RawSourceOpenResult result;
  const std::filesystem::path path =
      pathOverride.empty() ? std::filesystem::u8path(image.path) : pathOverride;
  if (!image.compressed) {
    if ((image.format == ImageFormat::Vhd || image.format == ImageFormat::Vhdx ||
         image.format == ImageFormat::Ffu) &&
        image.containerPayloadLayout == ContainerPayloadLayout::None) {
      result.error =
          "The container does not expose a validated standalone raw-disk payload";
      return result;
    }
    if (image.containerPayloadLayout == ContainerPayloadLayout::DynamicVhd) {
      auto source = std::make_unique<DynamicVhdRawSource>(
          path, image.sizeBytes, image.containerPayloadSizeBytes);
      if (!source->valid()) {
        result.error = "Unable to open the validated dynamic VHD payload";
        return result;
      }
      result.source = std::move(source);
      return result;
    }
    if (image.containerPayloadLayout == ContainerPayloadLayout::DynamicVhdx) {
      auto source = std::make_unique<DynamicVhdxRawSource>(path, image);
      if (!source->valid()) {
        result.error = "Unable to open the validated dynamic VHDX payload";
        return result;
      }
      result.source = std::move(source);
      return result;
    }
    auto source = std::make_unique<FileRawSource>(
        path, image.sizeBytes, image.containerPayloadOffsetBytes,
        image.containerPayloadSizeBytes);
    if (!source->valid()) {
      result.error = "Unable to open the analyzed source image";
      return result;
    }
    result.source = std::move(source);
    return result;
  }
  if (!image.capabilities.validContainerMetadata ||
      !image.capabilities.compressedSizeKnown ||
      image.expandedSizeBytes == 0U) {
    result.error = "Compressed-image metadata was not fully validated";
    return result;
  }
  auto decoder = makeDecoder(path, image.format, result.error);
  if (decoder == nullptr) {
    return result;
  }
  result.source = std::make_unique<CompressedRawSource>(
      std::move(decoder), image.expandedSizeBytes);
  return result;
}

namespace detail {

CompressedMeasurement measureCompressedImage(
    const std::filesystem::path& path, const ImageFormat format,
    const std::function<bool()>& isCancelled) {
  CompressedMeasurement result;
  auto decoder = makeDecoder(path, format, result.error);
  if (decoder == nullptr) {
    return result;
  }
  std::vector<unsigned char> buffer(1024U * 1024U);
  bool eof = false;
  while (!eof) {
    if (isCancelled && isCancelled()) {
      result.error = "Compressed-image analysis was cancelled";
      return result;
    }
    std::size_t produced = 0;
    if (!decoder->read(buffer.data(), buffer.size(), produced, eof,
                       result.error)) {
      return result;
    }
    if (produced == 0U && !eof) {
      result.error = "The compressed image decoder made no progress";
      return result;
    }
    if (produced > std::numeric_limits<std::uint64_t>::max() -
                       result.expandedSizeBytes) {
      result.error = "The expanded compressed-image size overflows 64 bits";
      return result;
    }
    const std::size_t prefixBytes = std::min(
        produced, result.prefix.size() - result.prefixBytes);
    std::copy_n(buffer.begin(), prefixBytes,
                result.prefix.begin() +
                    static_cast<std::ptrdiff_t>(result.prefixBytes));
    result.prefixBytes += prefixBytes;
    result.expandedSizeBytes += produced;
  }
  if (result.expandedSizeBytes == 0U) {
    result.error = "The compressed image expands to an empty file";
    return result;
  }
  result.success = true;
  return result;
}

}  // namespace detail

}  // namespace rufus::core
