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

#include "main_window.hpp"
#include "rufus/core/application_info.hpp"
#include "secure_boot_assets.hpp"
#include "uefi_shell_assets.hpp"

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

  if (application.arguments().contains("--smoke-test")) {
    QTimer::singleShot(0, &application, &QApplication::quit);
  }

  return application.exec();
}
