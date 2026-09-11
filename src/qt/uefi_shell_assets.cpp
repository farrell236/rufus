/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "uefi_shell_assets.hpp"

#include <QCryptographicHash>
#include <QFile>

namespace rufus::qt {

namespace {

constexpr qsizetype kMaximumShellBytes = 4 * 1024 * 1024;
constexpr std::uint16_t kEfiApplicationSubsystem = 10U;

std::uint16_t littleEndian16(const QByteArray& bytes, const qsizetype offset) {
  const auto low = static_cast<unsigned char>(bytes.at(offset));
  const auto high = static_cast<unsigned char>(bytes.at(offset + 1));
  return static_cast<std::uint16_t>(low |
                                    (static_cast<std::uint16_t>(high) << 8U));
}

std::uint32_t littleEndian32(const QByteArray& bytes, const qsizetype offset) {
  std::uint32_t value = 0U;
  for (qsizetype index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes.at(offset + index)))
             << (static_cast<unsigned int>(index) * 8U);
  }
  return value;
}

bool validateEfiApplication(const BundledUefiShellAsset& asset,
                            const QByteArray& bytes, QString& error) {
  if (bytes.size() < 64 || bytes.at(0) != 'M' || bytes.at(1) != 'Z') {
    error = asset.architectureLabel + " shell has no valid DOS/PE header";
    return false;
  }
  const std::uint32_t peOffset = littleEndian32(bytes, 60);
  if (peOffset > static_cast<std::uint32_t>(bytes.size() - 24)) {
    error = asset.architectureLabel + " shell has an invalid PE offset";
    return false;
  }
  const qsizetype pe = static_cast<qsizetype>(peOffset);
  if (bytes.at(pe) != 'P' || bytes.at(pe + 1) != 'E' ||
      bytes.at(pe + 2) != '\0' || bytes.at(pe + 3) != '\0') {
    error = asset.architectureLabel + " shell has an invalid PE signature";
    return false;
  }
  if (littleEndian16(bytes, pe + 4) != asset.peMachine) {
    error = asset.architectureLabel + " shell has the wrong PE architecture";
    return false;
  }
  const std::uint16_t optionalHeaderBytes = littleEndian16(bytes, pe + 20);
  const qsizetype optionalHeader = pe + 24;
  if (optionalHeaderBytes < 70U ||
      optionalHeader > bytes.size() - optionalHeaderBytes) {
    error = asset.architectureLabel + " shell has a truncated PE optional header";
    return false;
  }
  const std::uint16_t magic = littleEndian16(bytes, optionalHeader);
  if ((magic != 0x010bU && magic != 0x020bU) ||
      littleEndian16(bytes, optionalHeader + 68) != kEfiApplicationSubsystem) {
    error = asset.architectureLabel + " payload is not a UEFI application";
    return false;
  }
  return true;
}

}  // namespace

QString BundledUefiShellAsset::displayLabel() const {
  return architectureLabel + " — " + releaseLabel +
         (legacy ? " (last supported release)" : QString{});
}

const std::vector<BundledUefiShellAsset>& bundledUefiShellAssets() {
  // These digests are published with the corresponding pbatard/UEFI-Shell
  // releases and are deliberately checked again whenever an asset is used.
  static const std::vector<BundledUefiShellAsset> assets{
      {"x86-64", "UEFI Shell 2.2 26H1", "BOOTX64.EFI",
       ":/rufus/uefi-shell/shellx64.efi",
       "4ea080ddd576117cd04f5c02d16712ea5d9249c0752214d8e4055e460d7b11e0",
       0x8664U, false},
      {"x86-32", "UEFI Shell 2.2 26H1", "BOOTIA32.EFI",
       ":/rufus/uefi-shell/shellia32.efi",
       "54ae3a8f58b6fe7123fd948d0773c88e8c26834e39acd3874732c96cbe7c0dd5",
       0x014cU, false},
      {"ARM64", "UEFI Shell 2.2 26H1", "BOOTAA64.EFI",
       ":/rufus/uefi-shell/shellaa64.efi",
       "1569b6db4e391c3c59194aa3319a3945efb800fb25349eb9d36ff3d258517ea6",
       0xaa64U, false},
      {"ARM32", "UEFI Shell 2.2 25H1", "BOOTARM.EFI",
       ":/rufus/uefi-shell/shellarm.efi",
       "eef9c4908b634d9fe0c853c75c284666319058b07545f03f9a7b8303a390a83f",
       0x01c2U, true},
      {"RISC-V 64", "UEFI Shell 2.2 26H1", "BOOTRISCV64.EFI",
       ":/rufus/uefi-shell/shellriscv64.efi",
       "ccdb9523276d470277f7676d6534916534cd70218ea5c4cc5ac302e149f65196",
       0x5064U, false},
      {"LoongArch64", "UEFI Shell 2.2 26H1", "BOOTLOONGARCH64.EFI",
       ":/rufus/uefi-shell/shellloongarch64.efi",
       "d6c97ae52707ebbad4eda063cb0aefc467ec942b07461a6d6d1119cad0ac3e9c",
       0x6264U, false},
  };
  return assets;
}

bool loadBundledUefiShellAsset(const BundledUefiShellAsset& asset,
                               QByteArray& contents, QString& error) {
  contents.clear();
  error.clear();
  QFile resource(asset.resourcePath);
  if (!resource.open(QIODevice::ReadOnly)) {
    error = "Unable to open the bundled " + asset.architectureLabel +
            " UEFI Shell";
    return false;
  }
  if (resource.size() <= 0 || resource.size() > kMaximumShellBytes) {
    error = "The bundled " + asset.architectureLabel +
            " UEFI Shell has an invalid size";
    return false;
  }
  contents = resource.readAll();
  if (contents.size() != resource.size()) {
    contents.clear();
    error = "Unable to read the complete bundled " + asset.architectureLabel +
            " UEFI Shell";
    return false;
  }
  const QByteArray digest =
      QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
  if (digest != asset.sha256) {
    contents.clear();
    error = "The bundled " + asset.architectureLabel +
            " UEFI Shell failed its SHA-256 integrity check";
    return false;
  }
  if (!validateEfiApplication(asset, contents, error)) {
    contents.clear();
    return false;
  }
  return true;
}

bool verifyBundledUefiShellAssets(QString& report) {
  qsizetype totalBytes = 0;
  for (const auto& asset : bundledUefiShellAssets()) {
    QByteArray contents;
    QString error;
    if (!loadBundledUefiShellAsset(asset, contents, error)) {
      report = error;
      return false;
    }
    totalBytes += contents.size();
  }
  report = QString("Verified %1 bundled UEFI Shell architectures (%2 bytes)")
               .arg(bundledUefiShellAssets().size())
               .arg(totalBytes);
  return true;
}

}  // namespace rufus::qt
