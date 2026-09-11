/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QTimer>
#include <QToolButton>

#include <memory>

#include "main_window.hpp"
#include "rufus/core/application_info.hpp"
#include "secure_boot_assets.hpp"
#include "uefi_shell_assets.hpp"

namespace {

class AdaptiveWindowProbe final
    : public std::enable_shared_from_this<AdaptiveWindowProbe> {
 public:
  AdaptiveWindowProbe(QApplication& application,
                      rufus::qt::MainWindow& window)
      : application_(application), window_(window) {}

  void advance() {
    if (!fixedAtCurrentSize()) {
      fail("Rufus++ must be fixed to its content-wrapped size");
      return;
    }

    switch (stage_++) {
      case 0:
        if (!findControls()) {
          fail("Unable to find the adaptive-window controls");
          return;
        }
        baselineSize_ = window_.size();
        driveAdvanced_->setChecked(true);
        queueNext();
        return;
      case 1:
        if (window_.width() != baselineSize_.width() ||
            window_.height() <= baselineSize_.height()) {
          fail("Expanding drive properties did not grow the window vertically");
          return;
        }
        driveAdvanced_->setChecked(false);
        queueNext();
        return;
      case 2:
        if (window_.size() != baselineSize_) {
          fail("Collapsing drive properties did not restore the wrapped size");
          return;
        }
        formatAdvanced_->setChecked(false);
        queueNext();
        return;
      case 3:
        if (window_.width() != baselineSize_.width() ||
            window_.height() >= baselineSize_.height()) {
          fail("Collapsing format options did not shrink the window vertically");
          return;
        }
        formatAdvanced_->setChecked(true);
        queueNext();
        return;
      case 4:
        if (window_.size() != baselineSize_) {
          fail("Expanding format options did not restore the wrapped size");
          return;
        }
        logButton_->click();
        queueNext();
        return;
      case 5:
        if (window_.width() != baselineSize_.width() ||
            window_.height() <= baselineSize_.height()) {
          fail("Showing the operation log did not grow the window vertically");
          return;
        }
        application_.exit(0);
        return;
      default:
        fail("Adaptive-window test entered an invalid state");
    }
  }

 private:
  [[nodiscard]] bool fixedAtCurrentSize() const {
    return window_.minimumSize() == window_.maximumSize() &&
           window_.size() == window_.minimumSize();
  }

  [[nodiscard]] bool findControls() {
    const auto buttons = window_.findChildren<QToolButton*>();
    for (QToolButton* button : buttons) {
      if (button->text().contains("advanced drive properties")) {
        driveAdvanced_ = button;
      } else if (button->text().contains("advanced format options")) {
        formatAdvanced_ = button;
      } else if (button->toolTip() == "Show or hide log") {
        logButton_ = button;
      }
    }
    return driveAdvanced_ != nullptr && formatAdvanced_ != nullptr &&
           logButton_ != nullptr;
  }

  void queueNext() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, &application_, [self] { self->advance(); });
  }

  void fail(const char* message) {
    qCritical().noquote() << message;
    application_.exit(1);
  }

  QApplication& application_;
  rufus::qt::MainWindow& window_;
  QToolButton* driveAdvanced_{};
  QToolButton* formatAdvanced_{};
  QToolButton* logButton_{};
  QSize baselineSize_;
  int stage_{};
};

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(
      QString::fromUtf8(rufus::core::ApplicationInfo::name.data()));
  application.setApplicationDisplayName(
      QString::fromUtf8(rufus::core::ApplicationInfo::displayName.data()));
  application.setApplicationVersion(
      QString::fromUtf8(rufus::core::ApplicationInfo::version.data()));
  application.setOrganizationName(
      QString::fromUtf8(rufus::core::ApplicationInfo::organization.data()));
  application.setWindowIcon(QIcon(":/rufus/icons/rufus-128.png"));

  if (application.arguments().contains("--verify-embedded-assets")) {
    QString shellReport;
    QString secureBootReport;
    if (!rufus::qt::verifyBundledUefiShellAssets(shellReport)) {
      qCritical().noquote() << shellReport;
      return 1;
    }
    if (!rufus::qt::verifyBundledSecureBootAssets(secureBootReport)) {
      qCritical().noquote() << secureBootReport;
      return 1;
    }
    qInfo().noquote() << shellReport;
    qInfo().noquote() << secureBootReport;
    return 0;
  }

  rufus::qt::MainWindow window;
  window.show();

  std::shared_ptr<AdaptiveWindowProbe> sizingProbe;
  if (application.arguments().contains("--verify-window-sizing")) {
    sizingProbe = std::make_shared<AdaptiveWindowProbe>(application, window);
    QTimer::singleShot(0, &application,
                       [sizingProbe] { sizingProbe->advance(); });
  }

  if (application.arguments().contains("--smoke-test")) {
    QTimer::singleShot(0, &application, &QApplication::quit);
  }

  return application.exec();
}
