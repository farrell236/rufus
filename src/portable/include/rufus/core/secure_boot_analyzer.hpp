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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rufus/core/media.hpp"

namespace rufus::core {

struct SecureBootDatabase final {
  std::string source;
  std::string version;
  std::string publishedDate;
  std::string sbatVersion;
  bool trustedBaseline{};
  bool customOverlay{};
  std::set<std::string> revokedSha256;
  std::map<std::string, std::uint64_t> minimumSbatGeneration;
};

struct SecureBootDatabaseResult final {
  SecureBootDatabase database;
  bool success{};
  std::string error;
};

enum class SecureBootDisposition {
  Compatible,
  Unsigned,
  RevokedHash,
  RevokedSbat,
  Unknown,
  Invalid,
};

struct EfiImageTrust final {
  std::string path;
  std::string sha256;
  std::string authenticodeSha256;
  bool validPe{};
  bool authenticodePresent{};
  SecureBootDisposition disposition{SecureBootDisposition::Unknown};
  std::vector<std::string> findings;
};

struct SecureBootAnalysisResult final {
  bool success{};
  bool cancelled{};
  std::string databaseSource;
  std::string databaseVersion;
  std::string databasePublishedDate;
  std::string sbatVersion;
  std::vector<EfiImageTrust> images;
  std::vector<std::string> warnings;
  std::string error;

  [[nodiscard]] bool hasRevokedImage() const noexcept;
  [[nodiscard]] std::string toText() const;
};

using SecureBootAnalysisCancelCallback = std::function<bool()>;

[[nodiscard]] SecureBootDatabase packagedSecureBootBaseline();
[[nodiscard]] SecureBootDatabaseResult loadSecureBootDatabase(
    const std::filesystem::path& path,
    const SecureBootDatabase& baseline = packagedSecureBootBaseline());
[[nodiscard]] SecureBootDatabaseResult loadSecureBootDatabaseBytes(
    const std::vector<unsigned char>& bytes, std::string source,
    const SecureBootDatabase& baseline = packagedSecureBootBaseline());
[[nodiscard]] SecureBootDatabaseResult loadSecureBootDatabaseArchive(
    const std::vector<unsigned char>& archive, std::string source,
    std::string publishedDate,
    const SecureBootDatabase& baseline = packagedSecureBootBaseline());

// Exposed so offline database and PE/SBAT behavior can be validated without
// constructing a complete optical image.
[[nodiscard]] EfiImageTrust analyzeEfiImage(
    std::string path, const std::vector<unsigned char>& bytes,
    const SecureBootDatabase& database);

class SecureBootAnalyzer final {
 public:
  [[nodiscard]] SecureBootAnalysisResult analyze(
      const ImageInfo& image,
      const SecureBootDatabase& database = packagedSecureBootBaseline(),
      const SecureBootAnalysisCancelCallback& isCancelled = {}) const;
};

[[nodiscard]] const char* secureBootDispositionName(
    SecureBootDisposition disposition) noexcept;

}  // namespace rufus::core
