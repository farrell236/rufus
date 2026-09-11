/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "secure_boot_assets.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <algorithm>
#include <vector>

namespace rufus::qt {
namespace {

constexpr auto kPackagedVersion = "v1.7.0-signed";
constexpr auto kPackagedPublishedDate = "2026-09-03";
constexpr auto kReleaseApi =
    "https://api.github.com/repos/microsoft/secureboot_objects/releases/latest";
constexpr auto kReleaseAssetName =
    "edk2-2023-signed-secureboot-binaries.zip";
constexpr auto kPackagedArchive =
    ":/rufus/secure-boot/dbx-v1.7.0-signed.zip";
constexpr auto kPackagedArchiveSha256 =
    "0c73bd3e244c92da8bf4d836ab2abf509cfaffff2e6ac151be93d56ddd056125";
constexpr qsizetype kMaximumMetadataBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumArchiveBytes = 64 * 1024 * 1024;

QString officialSource(const QString& version, const QString& suffix) {
  return "Microsoft secureboot_objects " + version + " (" + suffix + ')';
}

QVersionNumber semanticVersion(QString version) {
  if (version.startsWith('v')) {
    version.remove(0, 1);
  }
  const qsizetype suffix = version.indexOf('-');
  if (suffix >= 0) {
    version.truncate(suffix);
  }
  return QVersionNumber::fromString(version);
}

bool isNewer(const QString& candidate, const QString& current) {
  const QVersionNumber candidateVersion = semanticVersion(candidate);
  const QVersionNumber currentVersion = semanticVersion(current);
  return !candidateVersion.isNull() &&
         (currentVersion.isNull() ||
          QVersionNumber::compare(candidateVersion, currentVersion) > 0);
}

std::vector<unsigned char> toBytes(const QByteArray& bytes) {
  const auto* begin = reinterpret_cast<const unsigned char*>(bytes.constData());
  return {begin, begin + bytes.size()};
}

QString cacheDirectory() {
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::AppLocalDataLocation))
      .filePath("secure-boot");
}

bool readBoundedFile(const QString& path, const qsizetype maximum,
                     QByteArray& bytes, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = "Unable to open " + path;
    return false;
  }
  if (file.size() <= 0 || file.size() > maximum) {
    error = "The file has an invalid size: " + path;
    return false;
  }
  bytes = file.readAll();
  if (bytes.size() != file.size()) {
    bytes.clear();
    error = "Unable to read the complete file: " + path;
    return false;
  }
  return true;
}

SecureBootDataLoadResult loadPackagedSecureBootData() {
  SecureBootDataLoadResult result;
  result.database = core::packagedSecureBootBaseline();
  QByteArray archive;
  QString error;
  if (!readBoundedFile(kPackagedArchive, kMaximumArchiveBytes, archive,
                       error)) {
    result.warning = error;
    return result;
  }
  const QByteArray digest =
      QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex();
  if (digest != kPackagedArchiveSha256) {
    result.warning =
        "The bundled Microsoft Secure Boot DBX archive failed its SHA-256 check";
    return result;
  }
  auto loaded = core::loadSecureBootDatabaseArchive(
      toBytes(archive),
      officialSource(kPackagedVersion, "packaged signed release").toStdString(),
      kPackagedPublishedDate, result.database);
  if (!loaded.success || loaded.database.version != kPackagedVersion) {
    result.warning = "The bundled Microsoft Secure Boot DBX archive is invalid: " +
                     QString::fromStdString(loaded.success
                                                 ? "version mismatch"
                                                 : loaded.error);
    return result;
  }
  result.database = std::move(loaded.database);
  result.database.trustedBaseline = true;
  result.database.customOverlay = false;
  return result;
}

bool saveFile(const QString& path, const QByteArray& bytes, QString& error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    error = "Unable to save " + path;
    return false;
  }
  return true;
}

void loadCachedUpdate(SecureBootDataLoadResult& result) {
  const QString directory = cacheDirectory();
  const QString manifestPath = QDir(directory).filePath("current.json");
  if (!QFile::exists(manifestPath)) {
    return;
  }
  QByteArray manifestBytes;
  QString error;
  if (!readBoundedFile(manifestPath, kMaximumMetadataBytes, manifestBytes,
                       error)) {
    result.warning = "Cached Secure Boot update ignored: " + error;
    return;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(manifestBytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    result.warning = "Cached Secure Boot update ignored: invalid manifest";
    return;
  }
  const QJsonObject manifest = document.object();
  const QString version = manifest.value("version").toString();
  const QString publishedDate = manifest.value("publishedDate").toString();
  const QString expectedSha256 = manifest.value("sha256").toString().toLower();
  const QString sourceUrl = manifest.value("sourceUrl").toString();
  static const QRegularExpression versionPattern(
      R"(^v[0-9]+\.[0-9]+\.[0-9]+-signed$)");
  static const QRegularExpression digestPattern(R"(^[0-9a-f]{64}$)");
  if (manifest.value("schema").toInt() != 1 ||
      !versionPattern.match(version).hasMatch() ||
      !digestPattern.match(expectedSha256).hasMatch() ||
      !sourceUrl.startsWith(
          "https://github.com/microsoft/secureboot_objects/releases/download/") ||
      !isNewer(version, QString::fromStdString(result.database.version))) {
    return;
  }
  QByteArray archive;
  if (!readBoundedFile(QDir(directory).filePath("current.zip"),
                       kMaximumArchiveBytes, archive, error)) {
    result.warning = "Cached Secure Boot update ignored: " + error;
    return;
  }
  const QString observedSha256 = QString::fromLatin1(
      QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex());
  if (observedSha256 != expectedSha256) {
    result.warning =
        "Cached Secure Boot update ignored: SHA-256 verification failed";
    return;
  }
  auto loaded = core::loadSecureBootDatabaseArchive(
      toBytes(archive), sourceUrl.toStdString(), publishedDate.toStdString(),
      result.database);
  if (!loaded.success || QString::fromStdString(loaded.database.version) != version) {
    result.warning = "Cached Secure Boot update ignored: " +
                     QString::fromStdString(loaded.success
                                                 ? "version mismatch"
                                                 : loaded.error);
    return;
  }
  loaded.database.source =
      officialSource(version, "verified cached signed release").toStdString();
  loaded.database.trustedBaseline = true;
  loaded.database.customOverlay = false;
  result.database = std::move(loaded.database);
  result.usedCachedUpdate = true;
}

struct DownloadResult final {
  QByteArray bytes;
  bool success{};
  bool cancelled{};
  QString error;
};

DownloadResult download(QWidget* parent, const QUrl& url, const QString& label,
                        const qsizetype maximumBytes) {
  DownloadResult result;
  if (!url.isValid() || url.scheme() != "https") {
    result.error = "The update service returned an invalid HTTPS URL";
    return result;
  }

  QNetworkAccessManager manager;
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("User-Agent", "RufusPlusPlus-Secure-Boot-Updater");
  QNetworkReply* const reply = manager.get(request);

  QProgressDialog progress(label, "Cancel", 0, 0, parent);
  progress.setWindowTitle("Secure Boot data");
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(0);
  progress.setAutoClose(false);

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  bool timedOut = false;
  bool tooLarge = false;
  QObject::connect(&timeout, &QTimer::timeout, reply, [&] {
    timedOut = true;
    reply->abort();
  });
  QObject::connect(&progress, &QProgressDialog::canceled, reply, [&] {
    result.cancelled = true;
    reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::downloadProgress, &progress,
                   [&](const qint64 received, const qint64 total) {
                     if (total > maximumBytes) {
                       tooLarge = true;
                       reply->abort();
                       return;
                     }
                     if (total > 0 && total <= maximumBytes) {
                       progress.setRange(0, 1000);
                       progress.setValue(static_cast<int>(
                           (received * 1000) / std::max<qint64>(1, total)));
                     }
                   });
  QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
    result.bytes += reply->readAll();
    if (result.bytes.size() > maximumBytes) {
      tooLarge = true;
      reply->abort();
    }
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  timeout.start(60000);
  loop.exec();
  timeout.stop();
  result.bytes += reply->readAll();
  progress.hide();

  if (result.cancelled) {
    result.bytes.clear();
    reply->deleteLater();
    return result;
  }
  if (timedOut) {
    result.error = "The Secure Boot update request timed out";
  } else if (tooLarge || result.bytes.size() > maximumBytes) {
    result.error = "The Secure Boot update response exceeds its safety limit";
  } else if (reply->error() != QNetworkReply::NoError) {
    result.error = reply->errorString();
  } else {
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
      result.error = QString("The update service returned HTTP %1").arg(status);
    } else {
      result.success = true;
    }
  }
  if (!result.success) {
    result.bytes.clear();
  }
  reply->deleteLater();
  return result;
}

}  // namespace

QString packagedSecureBootDataVersion() {
  return kPackagedVersion;
}

SecureBootDataLoadResult loadSecureBootData() {
  SecureBootDataLoadResult result = loadPackagedSecureBootData();
  if (!result.warning.isEmpty()) {
    return result;
  }
  loadCachedUpdate(result);
  return result;
}

bool verifyBundledSecureBootAssets(QString& report) {
  const SecureBootDataLoadResult loaded = loadPackagedSecureBootData();
  if (!loaded.warning.isEmpty()) {
    report = loaded.warning;
    return false;
  }
  if (loaded.database.version != kPackagedVersion ||
      loaded.database.revokedSha256.empty()) {
    report = "The bundled Secure Boot baseline did not load any DBX hashes";
    return false;
  }
  report = QString("Verified Microsoft Secure Boot DBX %1 for %2 architectures (%3 revoked hashes)")
               .arg(kPackagedVersion)
               .arg(4)
               .arg(loaded.database.revokedSha256.size());
  return true;
}

SecureBootDataRefreshResult refreshSecureBootData(
    QWidget* parent, const core::SecureBootDatabase& current) {
  SecureBootDataRefreshResult result;
  result.database = current;
  const DownloadResult metadata =
      download(parent, QUrl(kReleaseApi),
               "Checking Microsoft for signed Secure Boot DBX updates...",
               kMaximumMetadataBytes);
  if (metadata.cancelled) {
    result.cancelled = true;
    return result;
  }
  if (!metadata.success) {
    result.error = metadata.error;
    return result;
  }

  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(metadata.bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    result.error = "Microsoft's release response was not valid JSON";
    return result;
  }
  const QJsonObject release = document.object();
  const QString version = release.value("tag_name").toString();
  static const QRegularExpression versionPattern(
      R"(^v[0-9]+\.[0-9]+\.[0-9]+-signed$)");
  if (!versionPattern.match(version).hasMatch() ||
      release.value("draft").toBool() || release.value("prerelease").toBool()) {
    result.error = "Microsoft's latest release is not a supported signed DBX release";
    return result;
  }
  QString publishedDate = release.value("published_at").toString();
  if (publishedDate.size() >= 10) {
    publishedDate.truncate(10);
  }

  QString assetUrl;
  QString expectedSha256;
  qint64 assetBytes = 0;
  for (const QJsonValue& value : release.value("assets").toArray()) {
    const QJsonObject asset = value.toObject();
    if (asset.value("name").toString() != kReleaseAssetName) {
      continue;
    }
    assetUrl = asset.value("browser_download_url").toString();
    expectedSha256 = asset.value("digest").toString().toLower();
    assetBytes = asset.value("size").toInteger();
    break;
  }
  if (expectedSha256.startsWith("sha256:")) {
    expectedSha256.remove(0, 7);
  }
  static const QRegularExpression digestPattern(R"(^[0-9a-f]{64}$)");
  const QUrl downloadUrl(assetUrl);
  if (!downloadUrl.isValid() || downloadUrl.scheme() != "https" ||
      downloadUrl.host() != "github.com" ||
      !downloadUrl.path().startsWith(
          "/microsoft/secureboot_objects/releases/download/") ||
      !digestPattern.match(expectedSha256).hasMatch() || assetBytes <= 0 ||
      assetBytes > kMaximumArchiveBytes) {
    result.error = "Microsoft's signed DBX release has no usable verified archive";
    return result;
  }

  const QString currentVersion = QString::fromStdString(current.version);
  if (!isNewer(version, currentVersion)) {
    result.success = true;
    result.alreadyCurrent = true;
    result.message = QString("Secure Boot DBX %1 is current (latest: %2).")
                         .arg(currentVersion.isEmpty() ? "unversioned"
                                                      : currentVersion,
                              version);
    return result;
  }

  const auto decision = QMessageBox::question(
      parent, "Secure Boot DBX update available",
      QString("Microsoft Secure Boot DBX %1 was published %2.\n\n"
              "Download and verify the signed %3 KiB update? The packaged %4 "
              "baseline remains available offline.")
          .arg(version, publishedDate)
          .arg((assetBytes + 1023) / 1024)
          .arg(kPackagedVersion),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if (decision != QMessageBox::Yes) {
    result.cancelled = true;
    return result;
  }

  const DownloadResult archive =
      download(parent, downloadUrl,
               QString("Downloading Microsoft Secure Boot DBX %1...").arg(version),
               kMaximumArchiveBytes);
  if (archive.cancelled) {
    result.cancelled = true;
    return result;
  }
  if (!archive.success) {
    result.error = archive.error;
    return result;
  }
  const QString observedSha256 = QString::fromLatin1(
      QCryptographicHash::hash(archive.bytes, QCryptographicHash::Sha256)
          .toHex());
  if (observedSha256 != expectedSha256) {
    result.error = "The downloaded signed DBX archive failed SHA-256 verification";
    return result;
  }

  auto loaded = core::loadSecureBootDatabaseArchive(
      toBytes(archive.bytes), assetUrl.toStdString(), publishedDate.toStdString(),
      current);
  if (!loaded.success) {
    result.error = QString::fromStdString(loaded.error);
    return result;
  }
  if (QString::fromStdString(loaded.database.version) != version) {
    result.error = "The signed DBX archive version does not match its release";
    return result;
  }
  loaded.database.customOverlay = current.customOverlay;
  loaded.database.trustedBaseline = !current.customOverlay;
  loaded.database.source =
      officialSource(version, current.customOverlay
                                  ? "verified signed release + custom overlay"
                                  : "verified signed release")
          .toStdString();

  const QString directory = cacheDirectory();
  if (!QDir().mkpath(directory)) {
    result.error = "Unable to create the Secure Boot update cache";
    return result;
  }
  QString saveError;
  if (!saveFile(QDir(directory).filePath("current.zip"), archive.bytes,
                saveError)) {
    result.error = saveError;
    return result;
  }
  const QJsonObject manifest{{"schema", 1},
                             {"version", version},
                             {"publishedDate", publishedDate},
                             {"sha256", expectedSha256},
                             {"sourceUrl", assetUrl}};
  if (!saveFile(QDir(directory).filePath("current.json"),
                QJsonDocument(manifest).toJson(QJsonDocument::Indented),
                saveError)) {
    result.error = saveError;
    return result;
  }

  result.database = std::move(loaded.database);
  result.success = true;
  result.message =
      QString("Secure Boot DBX updated to %1 (published %2).")
          .arg(version, publishedDate);
  return result;
}

}  // namespace rufus::qt
