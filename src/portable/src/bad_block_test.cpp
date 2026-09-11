/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/bad_block_test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace rufus::core {
namespace {

constexpr std::size_t kMinimumTransferBytes = 4096U;
constexpr std::size_t kMaximumTransferBytes = 64U * 1024U * 1024U;
constexpr std::array<std::uint64_t, 4> kPassSeeds{
    0x243f6a8885a308d3ULL,
    0x13198a2e03707344ULL,
    0xa4093822299f31d0ULL,
    0x082efa98ec4e6c89ULL,
};

std::uint64_t mix(std::uint64_t value) {
  value ^= value >> 30U;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

void fillPattern(unsigned char* destination, const std::size_t size,
                 const std::uint64_t absoluteOffset,
                 const unsigned int pass) {
  std::size_t index = 0;
  while (index < size) {
    const std::uint64_t byteOffset = absoluteOffset + index;
    const std::uint64_t word = mix((byteOffset / 8U) ^ kPassSeeds[pass]);
    const auto withinWord = static_cast<unsigned int>(byteOffset % 8U);
    const auto count = std::min<std::size_t>(8U - withinWord, size - index);
    for (std::size_t byte = 0; byte < count; ++byte) {
      destination[index + byte] = static_cast<unsigned char>(
          word >> ((withinWord + byte) * 8U));
    }
    index += count;
  }
}

bool cancelled(const BadBlockTestCancelCallback& callback) {
  return callback && callback();
}

void report(const BadBlockTestProgressCallback& callback,
            const BadBlockTestStage stage, const unsigned int pass,
            const unsigned int passCount, const std::uint64_t processed,
            const std::uint64_t total) {
  if (callback) {
    callback({stage, pass, passCount, processed, total});
  }
}

}  // namespace

BadBlockTestResult BadBlockTester::test(
    RawTargetIo& target, const BadBlockTestOptions& options,
    const BadBlockTestProgressCallback& onProgress,
    const BadBlockTestCancelCallback& isCancelled) const {
  BadBlockTestResult result;
  const std::uint64_t capacity = target.capacityBytes();
  const std::uint32_t sectorSize = target.logicalSectorSize();
  if (capacity == 0U || sectorSize == 0U || capacity % sectorSize != 0U) {
    result.error = "Bad-block testing requires sector-aligned target geometry";
    return result;
  }
  if (options.passes == 0U || options.passes > kPassSeeds.size()) {
    result.error = "Bad-block testing supports between one and four passes";
    return result;
  }
  if (options.transferBytes < kMinimumTransferBytes ||
      options.transferBytes > kMaximumTransferBytes ||
      options.transferBytes % sectorSize != 0U) {
    result.error =
        "Bad-block transfer size must be sector-aligned and between 4 KiB and 64 MiB";
    return result;
  }

  std::vector<unsigned char> expected(options.transferBytes);
  std::vector<unsigned char> observed(options.transferBytes);
  const std::uint64_t totalWork =
      capacity > std::numeric_limits<std::uint64_t>::max() /
                     (2U * options.passes)
          ? std::numeric_limits<std::uint64_t>::max()
          : capacity * 2U * options.passes;
  std::uint64_t completedWork = 0;
  for (unsigned int pass = 0; pass < options.passes; ++pass) {
    for (std::uint64_t offset = 0; offset < capacity;) {
      if (cancelled(isCancelled)) {
        result.cancelled = true;
        result.error = "Bad-block test cancelled";
        return result;
      }
      const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
          expected.size(), capacity - offset));
      fillPattern(expected.data(), amount, offset, pass);
      if (!target.writeAt(offset, expected.data(), amount, result.error)) {
        return result;
      }
      result.destructiveWriteStarted = true;
      offset += amount;
      completedWork = std::min<std::uint64_t>(totalWork,
                                              completedWork + amount);
      report(onProgress, BadBlockTestStage::WritingPattern, pass + 1U,
             options.passes, completedWork, totalWork);
    }
    report(onProgress, BadBlockTestStage::Flushing, pass + 1U, options.passes,
           completedWork, totalWork);
    if (!target.flush(result.error)) {
      return result;
    }
    for (std::uint64_t offset = 0; offset < capacity;) {
      if (cancelled(isCancelled)) {
        result.cancelled = true;
        result.error = "Bad-block test cancelled";
        return result;
      }
      const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
          expected.size(), capacity - offset));
      fillPattern(expected.data(), amount, offset, pass);
      if (!target.readAt(offset, observed.data(), amount, result.error)) {
        return result;
      }
      for (std::size_t sectorOffset = 0; sectorOffset < amount;
           sectorOffset += sectorSize) {
        const auto sectorBytes = std::min<std::size_t>(sectorSize,
                                                       amount - sectorOffset);
        if (!std::equal(expected.begin() +
                            static_cast<std::ptrdiff_t>(sectorOffset),
                        expected.begin() + static_cast<std::ptrdiff_t>(
                                               sectorOffset + sectorBytes),
                        observed.begin() +
                            static_cast<std::ptrdiff_t>(sectorOffset))) {
          ++result.badSectorCount;
          if (result.firstBadSectorOffsets.size() <
              options.maximumReportedOffsets) {
            result.firstBadSectorOffsets.push_back(offset + sectorOffset);
          }
        }
      }
      offset += amount;
      completedWork = std::min<std::uint64_t>(totalWork,
                                              completedWork + amount);
      result.bytesTested += amount;
      report(onProgress, BadBlockTestStage::VerifyingPattern, pass + 1U,
             options.passes, completedWork, totalWork);
    }
  }
  result.completed = true;
  result.success = result.badSectorCount == 0U;
  if (!result.success) {
    result.error = "The device returned mismatched data for " +
                   std::to_string(result.badSectorCount) +
                   " sector(s), indicating bad blocks or aliased fake capacity";
    if (!result.firstBadSectorOffsets.empty()) {
      result.error += "; first byte offset(s): ";
      for (std::size_t index = 0;
           index < result.firstBadSectorOffsets.size(); ++index) {
        if (index != 0U) {
          result.error += ", ";
        }
        result.error += std::to_string(result.firstBadSectorOffsets[index]);
      }
    }
  }
  report(onProgress, BadBlockTestStage::Complete, options.passes,
         options.passes, totalWork, totalWork);
  return result;
}

const char* badBlockTestStageName(const BadBlockTestStage stage) noexcept {
  switch (stage) {
    case BadBlockTestStage::WritingPattern:
      return "Writing test pattern";
    case BadBlockTestStage::Flushing:
      return "Flushing test pattern";
    case BadBlockTestStage::VerifyingPattern:
      return "Verifying test pattern";
    case BadBlockTestStage::Complete:
      return "Bad-block test complete";
  }
  return "Bad-block test";
}

}  // namespace rufus::core
