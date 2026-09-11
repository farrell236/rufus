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
#include <QLabel>
#include <QStringList>
#include <QTimer>
#include <QToolButton>

#include <cstdio>
#include <memory>

#include "main_window.hpp"
#include "rufus/core/application_info.hpp"
#include "secure_boot_assets.hpp"
#include "uefi_shell_assets.hpp"

namespace {

constexpr int kAdaptiveWindowProbePollMilliseconds = 25;
constexpr int kAdaptiveWindowProbeTimeoutMilliseconds = 3000;

class AdaptiveWindowProbe final
    : public std::enable_shared_from_this<AdaptiveWindowProbe> {
 public:
  AdaptiveWindowProbe(QApplication& application,
                      rufus::qt::MainWindow& window)
      : application_(application), window_(window) {}

  void advance() {
    switch (stage_) {
      case 0:
        if (!waitFor(fixedAtCurrentSize(),
                     "Rufus++ must be fixed to its content-wrapped size")) {
          return;
        }
        if (!findControls()) {
          fail("Unable to find the adaptive-window controls");
          return;
        }
        baselineSize_ = window_.size();
        driveAdvanced_->setChecked(true);
        beginStage(1);
        return;
      case 1:
        if (!waitFor(fixedAtCurrentSize() &&
                         window_.width() == baselineSize_.width() &&
                         window_.height() > baselineSize_.height(),
                     "Expanding drive properties did not grow the window vertically")) {
          return;
        }
        driveAdvanced_->setChecked(false);
        beginStage(2);
        return;
      case 2:
        if (!waitFor(fixedAtCurrentSize() && window_.size() == baselineSize_,
                     "Collapsing drive properties did not restore the wrapped size")) {
          return;
        }
        formatAdvanced_->setChecked(false);
        beginStage(3);
        return;
      case 3:
        if (!waitFor(fixedAtCurrentSize() &&
                         window_.width() == baselineSize_.width() &&
                         window_.height() < baselineSize_.height(),
                     "Collapsing format options did not shrink the window vertically")) {
          return;
        }
        formatAdvanced_->setChecked(true);
        beginStage(4);
        return;
      case 4:
        if (!waitFor(fixedAtCurrentSize() && window_.size() == baselineSize_,
                     "Expanding format options did not restore the wrapped size")) {
          return;
        }
        logButton_->click();
        beginStage(5);
        return;
      case 5:
        if (!waitFor(fixedAtCurrentSize() &&
                         window_.width() == baselineSize_.width() &&
                         window_.height() > baselineSize_.height(),
                     "Showing the operation log did not grow the window vertically")) {
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

  void queueProbe() {
    const auto self = shared_from_this();
    QTimer::singleShot(kAdaptiveWindowProbePollMilliseconds, &application_,
                       [self] { self->advance(); });
  }

  void beginStage(const int stage) {
    stage_ = stage;
    stageWaitMilliseconds_ = 0;
    queueProbe();
  }

  [[nodiscard]] bool waitFor(const bool condition, const char* failureMessage) {
    if (condition) {
      return true;
    }
    stageWaitMilliseconds_ += kAdaptiveWindowProbePollMilliseconds;
    if (stageWaitMilliseconds_ >= kAdaptiveWindowProbeTimeoutMilliseconds) {
      fail(failureMessage);
    } else {
      queueProbe();
    }
    return false;
  }

  void fail(const char* message) {
    std::fprintf(
        stderr,
        "%s [stage=%d, current=%dx%d, minimum=%dx%d, maximum=%dx%d, "
        "baseline=%dx%d, drive-panel=%s, format-panel=%s]\n",
        message, stage_, window_.width(), window_.height(),
        window_.minimumWidth(), window_.minimumHeight(),
        window_.maximumWidth(), window_.maximumHeight(), baselineSize_.width(),
        baselineSize_.height(),
        driveAdvanced_ != nullptr && driveAdvanced_->isChecked() ? "open"
                                                                 : "closed",
        formatAdvanced_ != nullptr && formatAdvanced_->isChecked() ? "open"
                                                                   : "closed");
    std::fflush(stderr);
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
  int stageWaitMilliseconds_{};
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

  const qsizetype documentationArgument =
      application.arguments().indexOf("--generate-documentation-screenshots");
  if (documentationArgument >= 0) {
    const QStringList arguments = application.arguments();
    if (documentationArgument + 1 >= arguments.size()) {
      qCritical() << "--generate-documentation-screenshots requires an output directory";
      return 2;
    }
    QString error;
    if (!window.generateDocumentationScreenshots(
            arguments.at(documentationArgument + 1), error)) {
      qCritical().noquote() << error;
      return 1;
    }
    qInfo().noquote()
        << "Documentation screenshots written to"
        << arguments.at(documentationArgument + 1);
    return 0;
  }

  if (application.arguments().contains("--verify-privilege-indicator")) {
    const auto* indicator = window.findChild<QLabel*>("privilegeIndicator");
    const QStringList validLabels{
        "UNPRIVILEGED", "ELEVATED", "PRIVILEGED HELPER"};
    if (indicator == nullptr || !validLabels.contains(indicator->text()) ||
        indicator->toolTip().isEmpty()) {
      qCritical() << "The bottom-right privilege indicator is missing or invalid";
      return 1;
    }
    for (const QString& marker : validLabels) {
      if (window.windowTitle().contains(marker)) {
        qCritical() << "Privilege state must not appear in the window title";
        return 1;
      }
    }
    if (window.windowTitle().contains("UNSIGNED ROOT BUILD") ||
        window.windowTitle().contains("RESTRICTED BUILD")) {
      qCritical() << "A legacy build-state marker remains in the window title";
      return 1;
    }
    QTimer::singleShot(0, &application, &QApplication::quit);
  }

  std::shared_ptr<AdaptiveWindowProbe> sizingProbe;
  if (application.arguments().contains("--verify-window-sizing")) {
    sizingProbe = std::make_shared<AdaptiveWindowProbe>(application, window);
    QTimer::singleShot(kAdaptiveWindowProbePollMilliseconds, &application,
                       [sizingProbe] { sizingProbe->advance(); });
  }

  if (application.arguments().contains("--smoke-test")) {
    QTimer::singleShot(0, &application, &QApplication::quit);
  }

  return application.exec();
}
