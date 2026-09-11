/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "windows_user_experience_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace rufus::qt {

WindowsUserExperienceDialog::WindowsUserExperienceDialog(
    const core::ImageArchitecture architecture,
    const std::uint32_t windowsBuild,
    core::WindowsUserExperienceOptions options,
    const core::WindowsDeploymentMode mode, QWidget* parent)
    : QDialog(parent), architecture_(architecture), mode_(mode) {
  setWindowTitle("Windows User Experience");
  setModal(true);
  setMinimumWidth(470);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 13, 16, 13);
  layout->setSpacing(8);

  auto* heading = new QLabel("Customize the Windows installation?", this);
  QFont headingFont = heading->font();
  headingFont.setPointSize(14);
  headingFont.setWeight(QFont::DemiBold);
  heading->setFont(headingFont);
  layout->addWidget(heading);

  auto* explanation = new QLabel(
      mode == core::WindowsDeploymentMode::WindowsToGo
          ? "These settings are applied to the portable Windows installation at first boot."
          : "These settings are embedded in the Windows Setup media and applied during installation.",
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  preventInternalDisks_ = new QCheckBox(
      "Prevent Windows To Go from accessing this computer's internal disks", this);
  preventInternalDisks_->setChecked(options.preventInternalDiskAccess);
  preventInternalDisks_->setToolTip(
      "Applies Windows SAN policy 4 so internal disks remain offline by default.");
  preventInternalDisks_->setVisible(mode == core::WindowsDeploymentMode::WindowsToGo);
  layout->addWidget(preventInternalDisks_);

  bypassHardwareRequirements_ = new QCheckBox(
      "Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0", this);
  bypassHardwareRequirements_->setChecked(options.bypassHardwareRequirements);
  bypassHardwareRequirements_->setVisible(
      mode == core::WindowsDeploymentMode::StandardInstallation && windowsBuild >= 22000U);
  layout->addWidget(bypassHardwareRequirements_);

  bypassOnlineAccount_ = new QCheckBox(
      "Remove requirement for an online Microsoft account", this);
  bypassOnlineAccount_->setChecked(options.bypassOnlineAccountRequirement);
  bypassOnlineAccount_->setVisible(windowsBuild >= 22500U);
  layout->addWidget(bypassOnlineAccount_);

  createLocalAccount_ = new QCheckBox("Create a local account with username", this);
  createLocalAccount_->setChecked(options.createLocalAccount);
  layout->addWidget(createLocalAccount_);

  auto* accountForm = new QFormLayout();
  accountForm->setContentsMargins(24, 0, 0, 0);
  localAccountName_ = new QLineEdit(QString::fromStdString(options.localAccountName), this);
  localAccountName_->setMaxLength(20);
  localAccountName_->setClearButtonEnabled(true);
  localAccountName_->setPlaceholderText("Username");
  accountForm->addRow("Username", localAccountName_);
  layout->addLayout(accountForm);

  auto* passwordNote = new QLabel(
      "No password is stored. Windows will require this account to choose one at first sign-in.",
      this);
  passwordNote->setWordWrap(true);
  passwordNote->setContentsMargins(24, 0, 0, 2);
  layout->addWidget(passwordNote);

  useRegionalOptions_ = new QCheckBox(
      "Set regional options to the same values as this user", this);
  useRegionalOptions_->setChecked(options.useRegionalOptions);
  layout->addWidget(useRegionalOptions_);

  auto* localeForm = new QFormLayout();
  localeForm->setContentsMargins(24, 0, 0, 0);
  localeName_ = new QLineEdit(this);
  const QString systemLocale = QLocale::system().name().replace('_', '-');
  localeName_->setText(options.localeName.empty()
                           ? systemLocale
                           : QString::fromStdString(options.localeName));
  localeName_->setPlaceholderText("en-US");
  localeForm->addRow("Windows locale", localeName_);
  layout->addLayout(localeForm);

  disableDataCollection_ = new QCheckBox("Disable data collection (skip privacy questions)", this);
  disableDataCollection_->setChecked(options.disableDataCollection);
  layout->addWidget(disableDataCollection_);

  disableAutomaticDeviceEncryption_ = new QCheckBox(
      "Disable automatic BitLocker device encryption", this);
  disableAutomaticDeviceEncryption_->setChecked(
      options.disableAutomaticDeviceEncryption);
  disableAutomaticDeviceEncryption_->setVisible(windowsBuild >= 22000U);
  layout->addWidget(disableAutomaticDeviceEncryption_);

  qualityOfLife_ = new QCheckBox("Disable Windows consumer experience suggestions", this);
  qualityOfLife_->setChecked(options.applyQualityOfLifeOptions);
  qualityOfLife_->setVisible(windowsBuild >= 22000U);
  layout->addWidget(qualityOfLife_);

  buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons_, &QDialogButtonBox::accepted,
          this, &WindowsUserExperienceDialog::acceptOptions);
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons_);

  connect(createLocalAccount_, &QCheckBox::toggled,
          this, &WindowsUserExperienceDialog::updateEnabledFields);
  connect(useRegionalOptions_, &QCheckBox::toggled,
          this, &WindowsUserExperienceDialog::updateEnabledFields);
  updateEnabledFields();
}

core::WindowsUserExperienceOptions WindowsUserExperienceDialog::options() const {
  core::WindowsUserExperienceOptions result;
  result.preventInternalDiskAccess = preventInternalDisks_->isChecked();
  if (preventInternalDisks_->isHidden()) {
    result.preventInternalDiskAccess = false;
  }
  result.bypassHardwareRequirements = bypassHardwareRequirements_->isChecked();
  if (bypassHardwareRequirements_->isHidden()) {
    result.bypassHardwareRequirements = false;
  }
  result.bypassOnlineAccountRequirement = bypassOnlineAccount_->isChecked();
  if (bypassOnlineAccount_->isHidden()) {
    result.bypassOnlineAccountRequirement = false;
  }
  result.createLocalAccount = createLocalAccount_->isChecked();
  result.localAccountName = localAccountName_->text().trimmed().toStdString();
  result.useRegionalOptions = useRegionalOptions_->isChecked();
  result.localeName = localeName_->text().trimmed().toStdString();
  result.disableDataCollection = disableDataCollection_->isChecked();
  result.disableAutomaticDeviceEncryption =
      disableAutomaticDeviceEncryption_->isChecked();
  if (disableAutomaticDeviceEncryption_->isHidden()) {
    result.disableAutomaticDeviceEncryption = false;
  }
  result.applyQualityOfLifeOptions = qualityOfLife_->isChecked();
  if (qualityOfLife_->isHidden()) {
    result.applyQualityOfLifeOptions = false;
  }
  return result;
}

void WindowsUserExperienceDialog::acceptOptions() {
  const core::WindowsUnattendResult validation =
      core::createWindowsUnattend(architecture_, options(), mode_);
  if (!validation.succeeded()) {
    QMessageBox::warning(this, "Invalid Windows options",
                         QString::fromStdString(validation.error));
    return;
  }
  accept();
}

void WindowsUserExperienceDialog::updateEnabledFields() {
  localAccountName_->setEnabled(createLocalAccount_->isChecked());
  localeName_->setEnabled(useRegionalOptions_->isChecked());
}

}  // namespace rufus::qt
