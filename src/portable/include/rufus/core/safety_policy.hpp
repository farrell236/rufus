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

#include <string>
#include <vector>

#include "rufus/core/media.hpp"

namespace rufus::core {

enum class SafetyIssueCode {
  ImageMissing,
  ImageEmpty,
  ImageUnsupported,
  ExpandedSizeUnknown,
  DeviceIneligible,
  DeviceTooSmall,
  SourceIsTarget,
  IdentityChanged,
  ModeUnsupported,
  InvalidSectorSize,
  ImageNotSectorAligned,
  InvalidBlockSize,
  SourceChanged,
};

struct SafetyIssue final {
  SafetyIssueCode code;
  std::string message;
};

struct SafetyCheckResult final {
  std::vector<SafetyIssue> issues;

  [[nodiscard]] bool safe() const noexcept { return issues.empty(); }
};

class SafetyPolicy final {
 public:
  [[nodiscard]] SafetyCheckResult validateWrite(const ImageInfo& image,
                                                const BlockDeviceInfo& target) const;
  [[nodiscard]] SafetyCheckResult validateIdentity(const BlockDeviceInfo& selected,
                                                   const BlockDeviceInfo& observed) const;
};

}  // namespace rufus::core
