/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rufus/core/raw_image_writer.hpp"

namespace rufus::core {

enum class BadBlockTestStage {
  WritingPattern,
  Flushing,
  VerifyingPattern,
  Complete,
};

struct BadBlockTestProgress final {
  BadBlockTestStage stage{BadBlockTestStage::WritingPattern};
  unsigned int pass{1};
  unsigned int passCount{1};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct BadBlockTestOptions final {
  unsigned int passes{1};
  std::size_t transferBytes{4U * 1024U * 1024U};
  std::size_t maximumReportedOffsets{256U};
};

struct BadBlockTestResult final {
  bool completed{};
  bool success{};
  bool cancelled{};
  bool destructiveWriteStarted{};
  std::uint64_t bytesTested{};
  std::uint64_t badSectorCount{};
  std::vector<std::uint64_t> firstBadSectorOffsets;
  std::string error;
};

using BadBlockTestProgressCallback =
    std::function<void(const BadBlockTestProgress&)>;
using BadBlockTestCancelCallback = std::function<bool()>;

// Destructively writes location-dependent patterns over the complete target,
// then reads them back. Location dependence detects capacity-faking devices
// whose advertised address ranges alias the same flash cells.
class BadBlockTester final {
 public:
  [[nodiscard]] BadBlockTestResult test(
      RawTargetIo& target, const BadBlockTestOptions& options = {},
      const BadBlockTestProgressCallback& onProgress = {},
      const BadBlockTestCancelCallback& isCancelled = {}) const;
};

[[nodiscard]] const char* badBlockTestStageName(BadBlockTestStage stage) noexcept;

}  // namespace rufus::core
