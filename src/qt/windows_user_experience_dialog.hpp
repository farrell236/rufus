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

#include <QDialog>

#include "rufus/core/windows_to_go.hpp"

class QCheckBox;
class QDialogButtonBox;
class QLineEdit;

namespace rufus::qt {

class WindowsUserExperienceDialog final : public QDialog {
  Q_OBJECT

 public:
  explicit WindowsUserExperienceDialog(
      core::ImageArchitecture architecture,
      std::uint32_t windowsBuild,
      core::WindowsUserExperienceOptions options,
      core::WindowsDeploymentMode mode,
      QWidget* parent = nullptr);

  [[nodiscard]] core::WindowsUserExperienceOptions options() const;

 private slots:
  void acceptOptions();
  void updateEnabledFields();

 private:
  core::ImageArchitecture architecture_{core::ImageArchitecture::Unknown};
  core::WindowsDeploymentMode mode_{core::WindowsDeploymentMode::StandardInstallation};
  QCheckBox* preventInternalDisks_{};
  QCheckBox* bypassHardwareRequirements_{};
  QCheckBox* bypassOnlineAccount_{};
  QCheckBox* createLocalAccount_{};
  QLineEdit* localAccountName_{};
  QCheckBox* useRegionalOptions_{};
  QLineEdit* localeName_{};
  QCheckBox* disableDataCollection_{};
  QCheckBox* disableAutomaticDeviceEncryption_{};
  QCheckBox* qualityOfLife_{};
  QDialogButtonBox* buttons_{};
};

}  // namespace rufus::qt
