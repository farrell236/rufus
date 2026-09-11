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

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rufus/core/media.hpp"

namespace rufus::core {

struct ImageAnalysisResult final {
  std::optional<ImageInfo> image;
  std::vector<std::string> warnings;
  std::string error;
  bool cancelled{};

  [[nodiscard]] bool succeeded() const noexcept { return image.has_value(); }
};

using ImageAnalysisCancelCallback = std::function<bool()>;

class ImageAnalyzer final {
 public:
  [[nodiscard]] ImageAnalysisResult analyze(const std::filesystem::path& path) const;
  [[nodiscard]] ImageAnalysisResult analyze(const std::filesystem::path& path,
                                            std::string_view filenameHint) const;
  [[nodiscard]] ImageAnalysisResult analyze(
      const std::filesystem::path& path, std::string_view filenameHint,
      const ImageAnalysisCancelCallback& isCancelled) const;
};

}  // namespace rufus::core
