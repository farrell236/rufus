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

#include <QString>

#include "rufus/core/secure_boot_analyzer.hpp"

class QWidget;

namespace rufus::qt {

struct SecureBootDataLoadResult final {
  core::SecureBootDatabase database;
  bool usedCachedUpdate{};
  QString warning;
};

struct SecureBootDataRefreshResult final {
  core::SecureBootDatabase database;
  bool success{};
  bool cancelled{};
  bool alreadyCurrent{};
  QString message;
  QString error;
};

[[nodiscard]] QString packagedSecureBootDataVersion();
[[nodiscard]] SecureBootDataLoadResult loadSecureBootData();
[[nodiscard]] bool verifyBundledSecureBootAssets(QString& report);

// Network access occurs only when this user-initiated function is called.
// A newer signed Microsoft release is presented for confirmation before its
// archive is downloaded or cached.
[[nodiscard]] SecureBootDataRefreshResult refreshSecureBootData(
    QWidget* parent, const core::SecureBootDatabase& current);

}  // namespace rufus::qt
