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

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace rufus::qt {

struct BundledUefiShellAsset final {
  QString architectureLabel;
  QString releaseLabel;
  QString fallbackFileName;
  QString resourcePath;
  QByteArray sha256;
  std::uint16_t peMachine{};
  bool legacy{};

  [[nodiscard]] QString displayLabel() const;
};

[[nodiscard]] const std::vector<BundledUefiShellAsset>&
bundledUefiShellAssets();

[[nodiscard]] bool loadBundledUefiShellAsset(
    const BundledUefiShellAsset& asset, QByteArray& contents, QString& error);

[[nodiscard]] bool verifyBundledUefiShellAssets(QString& report);

}  // namespace rufus::qt
