/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/windows_to_go.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "iso9660_reader.hpp"
#include "udf_reader.hpp"

namespace rufus::core {
namespace {

constexpr std::uint64_t kGibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMinimumTargetBytes = 32ULL * kGibibyte;
constexpr std::uint64_t kDeploymentOverheadBytes = 4ULL * kGibibyte;
constexpr std::size_t kTransferBytes = 8U * 1024U * 1024U;

std::string xmlEscape(const std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '\"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

std::string foldAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

bool validLocalAccountName(const std::string& value, std::string& error) {
  if (value.empty()) {
    error = "Enter a name for the local Windows account";
    return false;
  }
  constexpr std::string_view forbidden = "\\/[]:;|=.,+*?<>%@&\"";
  std::size_t characterCount = 0;
  for (std::size_t index = 0; index < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    std::uint32_t codePoint = 0;
    std::size_t length = 0;
    if (first < 0x80U) {
      codePoint = first;
      length = 1;
    } else if ((first & 0xe0U) == 0xc0U) {
      codePoint = first & 0x1fU;
      length = 2;
    } else if ((first & 0xf0U) == 0xe0U) {
      codePoint = first & 0x0fU;
      length = 3;
    } else if ((first & 0xf8U) == 0xf0U) {
      codePoint = first & 0x07U;
      length = 4;
    } else {
      error = "The local Windows account name is not valid Unicode";
      return false;
    }
    if (length > value.size() - index) {
      error = "The local Windows account name is not valid Unicode";
      return false;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      const unsigned char byte =
          static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        error = "The local Windows account name is not valid Unicode";
        return false;
      }
      codePoint = (codePoint << 6U) | (byte & 0x3fU);
    }
    const bool overlong = (length == 2 && codePoint < 0x80U) ||
                          (length == 3 && codePoint < 0x800U) ||
                          (length == 4 && codePoint < 0x10000U);
    if (overlong || codePoint > 0x10ffffU ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
        codePoint < 0x20U || codePoint == 0x7fU ||
        (codePoint < 0x80U &&
         forbidden.find(static_cast<char>(codePoint)) != std::string_view::npos)) {
      error = "The local Windows account name contains a character Windows does not allow";
      return false;
    }
    ++characterCount;
    index += length;
  }
  if (characterCount > 20U) {
    error = "The local Windows account name cannot exceed 20 characters";
    return false;
  }
  if (value.back() == ' ' || value.back() == '.') {
    error = "The local Windows account name cannot end in a space or period";
    return false;
  }
  const std::string folded = foldAscii(value);
  if (folded == "administrator" || folded == "guest" ||
      folded == "defaultaccount" || folded == "wdagutilityaccount" ||
      folded == "helpassistant" || folded == "krbtgt" || folded == "local" ||
      folded == "none" || folded == "system") {
    error = "That local Windows account name is reserved";
    return false;
  }
  return true;
}

bool validLocaleName(const std::string& value) {
  if (value.size() < 2U || value.size() > 35U || value.front() == '-' ||
      value.back() == '-') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
    return std::isalnum(character) || character == '-';
  });
}

const char* unattendArchitecture(const ImageArchitecture architecture) {
  switch (architecture) {
    case ImageArchitecture::X86:
      return "x86";
    case ImageArchitecture::X64:
      return "amd64";
    case ImageArchitecture::Arm:
      return "arm";
    case ImageArchitecture::Arm64:
      return "arm64";
    case ImageArchitecture::Itanium:
      return "ia64";
    case ImageArchitecture::Unknown:
    case ImageArchitecture::RiscV64:
    case ImageArchitecture::LoongArch64:
    case ImageArchitecture::Multiple:
      return nullptr;
  }
  return nullptr;
}

void appendComponentStart(std::string& xml, const char* name, const char* architecture) {
  xml += "    <component name=\"";
  xml += name;
  xml += "\" processorArchitecture=\"";
  xml += architecture;
  xml += "\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" "
         "versionScope=\"nonSxS\">\n";
}

void appendCommand(std::string& xml, const unsigned int order,
                   const std::string_view description,
                   const std::string_view command) {
  xml += "      <RunSynchronousCommand wcm:action=\"add\">\n"
         "        <Order>" + std::to_string(order) + "</Order>\n"
         "        <Description>" + xmlEscape(description) + "</Description>\n"
         "        <Path>cmd /c " + xmlEscape(command) + "</Path>\n"
         "      </RunSynchronousCommand>\n";
}

std::string normalizedVolumeLabel(std::string label) {
  label.erase(std::remove_if(label.begin(), label.end(), [](const unsigned char character) {
                return character < 0x20U || std::string_view("\"*/:<>?\\|").find(
                                                  static_cast<char>(character)) !=
                                              std::string_view::npos;
              }),
              label.end());
  while (!label.empty() && (label.back() == ' ' || label.back() == '.')) {
    label.pop_back();
  }
  if (label.empty()) {
    label = "Windows";
  }
  if (label.size() > 32U) {
    label.resize(32U);
  }
  return label;
}

void appendIssues(std::vector<SafetyIssue>& destination, SafetyCheckResult source) {
  destination.insert(destination.end(), std::make_move_iterator(source.issues.begin()),
                     std::make_move_iterator(source.issues.end()));
}

std::string normalizedOpticalPath(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  const auto version = value.find(';');
  if (version != std::string::npos) {
    value.erase(version);
  }
  return foldAscii(std::move(value));
}

bool isWindowsInstallImage(const std::string& path) {
  const std::string normalized = normalizedOpticalPath(path);
  return normalized == "sources/install.wim" ||
         normalized == "sources/install.esd";
}

bool sourceUnchanged(const WindowsToGoPlan& plan, std::string& error) {
  std::error_code fileError;
  const auto path = std::filesystem::u8path(plan.image().path);
  const auto size = std::filesystem::file_size(path, fileError);
  if (fileError || size != plan.image().sizeBytes) {
    error = fileError ? "The Windows source ISO is unavailable: " + fileError.message()
                      : "The Windows source ISO size changed after planning";
    return false;
  }
  const auto timestamp = std::filesystem::last_write_time(path, fileError);
  if (fileError || timestamp != plan.sourceLastWriteTime()) {
    error = fileError ? "The Windows source ISO timestamp is unavailable: " +
                            fileError.message()
                      : "The Windows source ISO changed after planning";
    return false;
  }
  return true;
}

}  // namespace

WindowsUnattendResult createWindowsToGoUnattend(
    const ImageArchitecture architecture,
    const WindowsUserExperienceOptions& options) {
  return createWindowsUnattend(architecture, options,
                               WindowsDeploymentMode::WindowsToGo);
}

WindowsUnattendResult createWindowsUnattend(
    const ImageArchitecture architecture,
    const WindowsUserExperienceOptions& options,
    const WindowsDeploymentMode mode) {
  WindowsUnattendResult result;
  const char* const processor = unattendArchitecture(architecture);
  if (processor == nullptr) {
    result.error = "The selected Windows edition has an unsupported or ambiguous architecture";
    return result;
  }
  if (options.createLocalAccount &&
      !validLocalAccountName(options.localAccountName, result.error)) {
    return result;
  }
  if (options.useRegionalOptions && !validLocaleName(options.localeName)) {
    result.error = "Enter a valid Windows locale name such as en-US";
    return result;
  }

  std::string xml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\" "
      "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n";

  if (mode == WindowsDeploymentMode::StandardInstallation &&
      options.bypassHardwareRequirements) {
    xml += "  <settings pass=\"windowsPE\">\n";
    appendComponentStart(xml, "Microsoft-Windows-Setup", processor);
    xml += "      <RunSynchronous>\n";
    unsigned int order = 1;
    for (const std::string_view check : {"BypassTPMCheck", "BypassSecureBootCheck",
                                         "BypassRAMCheck"}) {
      appendCommand(xml, order++, "Bypass Windows hardware requirement",
                    "reg add HKLM\\SYSTEM\\Setup\\LabConfig /v " +
                        std::string(check) + " /t REG_DWORD /d 1 /f");
    }
    xml += "      </RunSynchronous>\n"
           "    </component>\n"
           "  </settings>\n";
  }

  if (mode == WindowsDeploymentMode::WindowsToGo &&
      options.preventInternalDiskAccess) {
    xml += "  <settings pass=\"offlineServicing\">\n";
    appendComponentStart(xml, "Microsoft-Windows-PartitionManager", processor);
    xml += "      <SanPolicy>4</SanPolicy>\n"
           "    </component>\n"
           "  </settings>\n";
  }

  if (options.bypassOnlineAccountRequirement || options.disableDataCollection ||
      options.disableAutomaticDeviceEncryption || options.applyQualityOfLifeOptions) {
    xml += "  <settings pass=\"specialize\">\n";
    appendComponentStart(xml, "Microsoft-Windows-Deployment", processor);
    xml += "      <RunSynchronous>\n";
    unsigned int order = 1;
    if (options.bypassOnlineAccountRequirement) {
      appendCommand(xml, order++, "Allow an offline account during OOBE",
                    "reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE "
                    "/v BypassNRO /t REG_DWORD /d 1 /f");
    }
    if (options.disableDataCollection) {
      appendCommand(xml, order++, "Set diagnostic data to the minimum level",
                    "reg add HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection "
                    "/v AllowTelemetry /t REG_DWORD /d 0 /f");
    }
    if (options.disableAutomaticDeviceEncryption) {
      appendCommand(xml, order++, "Disable automatic device encryption",
                    "reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\BitLocker "
                    "/v PreventDeviceEncryption /t REG_DWORD /d 1 /f");
    }
    if (options.applyQualityOfLifeOptions) {
      appendCommand(xml, order++, "Disable Windows consumer experiences",
                    "reg add HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent "
                    "/v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f");
    }
    xml += "      </RunSynchronous>\n"
           "    </component>\n"
           "  </settings>\n";
  }

  if (options.createLocalAccount || options.useRegionalOptions ||
      options.bypassOnlineAccountRequirement || options.disableDataCollection) {
    xml += "  <settings pass=\"oobeSystem\">\n";
    if (options.useRegionalOptions) {
      appendComponentStart(xml, "Microsoft-Windows-International-Core", processor);
      const std::string locale = xmlEscape(options.localeName);
      xml += "      <InputLocale>" + locale + "</InputLocale>\n"
             "      <SystemLocale>" + locale + "</SystemLocale>\n"
             "      <UILanguage>" + locale + "</UILanguage>\n"
             "      <UserLocale>" + locale + "</UserLocale>\n"
             "    </component>\n";
    }
    appendComponentStart(xml, "Microsoft-Windows-Shell-Setup", processor);
    xml += "      <OOBE>\n";
    if (options.bypassOnlineAccountRequirement) {
      xml += "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n";
    }
    if (options.disableDataCollection) {
      xml += "        <HideEULAPage>true</HideEULAPage>\n";
      xml += "        <ProtectYourPC>3</ProtectYourPC>\n";
    }
    xml += "      </OOBE>\n";
    if (options.createLocalAccount) {
      const std::string username = xmlEscape(options.localAccountName);
      xml += "      <UserAccounts>\n"
             "        <LocalAccounts>\n"
             "          <LocalAccount wcm:action=\"add\">\n"
             "            <Name>" + username + "</Name>\n"
             "            <DisplayName>" + username + "</DisplayName>\n"
             "            <Group>Administrators;Power Users</Group>\n"
             "            <Password>\n"
             "              <Value></Value>\n"
             "              <PlainText>true</PlainText>\n"
             "            </Password>\n"
             "          </LocalAccount>\n"
             "        </LocalAccounts>\n"
             "      </UserAccounts>\n"
             "      <FirstLogonCommands>\n"
             "        <SynchronousCommand wcm:action=\"add\">\n"
             "          <Order>1</Order>\n"
             "          <Description>Require a password at first sign-in</Description>\n"
             "          <CommandLine>net user &quot;" + username +
             "&quot; /logonpasswordchg:yes</CommandLine>\n"
             "        </SynchronousCommand>\n"
             "        <SynchronousCommand wcm:action=\"add\">\n"
             "          <Order>2</Order>\n"
             "          <Description>Keep local account passwords from expiring</Description>\n"
             "          <CommandLine>net accounts /maxpwage:unlimited</CommandLine>\n"
             "        </SynchronousCommand>\n"
             "      </FirstLogonCommands>\n";
    }
    xml += "    </component>\n"
           "  </settings>\n";
  }
  xml += "</unattend>\n";
  result.xml = std::move(xml);
  return result;
}

WindowsToGoPlanResult WindowsToGoPlanner::build(
    const ImageInfo& image, const BlockDeviceInfo& target,
    WindowsToGoOptions options, std::string volumeLabel) const {
  WindowsToGoPlanResult result;
  appendIssues(result.issues, SafetyPolicy{}.validateWrite(image, target));
  if (image.format != ImageFormat::Iso || image.family != ImageFamily::WindowsInstaller ||
      !image.capabilities.windowsToGo || !image.capabilities.windowsImageMetadata) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "The selected image is not a validated Windows To Go source"});
  }
  if (target.logicalSectorSize != 512U) {
    result.issues.push_back(
        {SafetyIssueCode::InvalidSectorSize,
         "Windows To Go GPT staging currently requires 512-byte logical sectors"});
  }

  const auto edition = std::find_if(
      image.windowsEditions.begin(), image.windowsEditions.end(),
      [&options](const WindowsEditionInfo& candidate) {
        return candidate.index == options.editionIndex;
      });
  if (edition == image.windowsEditions.end()) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "The selected Windows edition is not present in install.wim/install.esd"});
  }

  std::uint64_t requiredBytes = kMinimumTargetBytes;
  if (edition != image.windowsEditions.end() && edition->totalBytes != 0 &&
      edition->totalBytes <=
          std::numeric_limits<std::uint64_t>::max() - kDeploymentOverheadBytes) {
    requiredBytes = std::max(requiredBytes, edition->totalBytes + kDeploymentOverheadBytes);
  }
  if (target.capacityBytes < requiredBytes) {
    result.issues.push_back(
        {SafetyIssueCode::DeviceTooSmall,
         "Windows To Go requires at least 32 GiB and enough space for the selected edition"});
  }

  const ImageArchitecture editionArchitecture =
      edition == image.windowsEditions.end() ||
              edition->architecture == ImageArchitecture::Unknown
          ? image.architecture
          : edition->architecture;
  WindowsUnattendResult unattended =
      createWindowsToGoUnattend(editionArchitecture, options.userExperience);
  if (!unattended.succeeded()) {
    result.issues.push_back({SafetyIssueCode::ModeUnsupported, unattended.error});
  }

  std::error_code fileError;
  const auto sourceTime = std::filesystem::last_write_time(
      std::filesystem::u8path(image.path), fileError);
  if (fileError) {
    result.issues.push_back(
        {SafetyIssueCode::ImageMissing,
         "The Windows source image cannot be revalidated: " + fileError.message()});
  }
  if (!result.issues.empty() || edition == image.windowsEditions.end()) {
    return result;
  }

  volumeLabel = normalizedVolumeLabel(std::move(volumeLabel));
  result.plan = WindowsToGoPlan(image, target, *edition, std::move(options),
                                std::move(volumeLabel), std::move(unattended.xml),
                                sourceTime);
  return result;
}

WindowsToGoSourceExtractionResult extractWindowsToGoSource(
    const WindowsToGoPlan& plan, const std::filesystem::path& outputPath,
    const WindowsToGoSourceProgressCallback& onProgress,
    const WindowsToGoCancelCallback& isCancelled) {
  WindowsToGoSourceExtractionResult result;
  if (outputPath.empty()) {
    result.error = "No Windows image extraction path was provided";
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Windows image extraction was cancelled before it began";
    return result;
  }
  if (!sourceUnchanged(plan, result.error)) {
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(outputPath, fileError) || fileError) {
    result.error = fileError ? "Unable to validate the Windows image extraction path: " +
                                   fileError.message()
                             : "The Windows image extraction output already exists";
    return result;
  }

  const auto sourcePath = std::filesystem::u8path(plan.image().path);
  auto iso = detail::readIso9660Contents(sourcePath);
  auto udf = detail::readUdfContents(sourcePath);
  const bool preferUdf =
      udf.valid && (!iso.valid || udf.entries.size() > iso.entries.size());
  const auto& files = preferUdf ? udf.files : iso.files;
  const auto source = std::find_if(files.begin(), files.end(),
                                   [](const detail::ImageFileRecord& file) {
                                     return isWindowsInstallImage(file.path);
                                   });
  if (source == files.end()) {
    result.error = "The validated sources/install.wim or install.esd entry is no longer present";
    return result;
  }

  std::ifstream sourceImage(sourcePath, std::ios::binary);
  if (!sourceImage) {
    result.error = "Unable to open the source ISO for Windows image extraction";
    return result;
  }

  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create the private Windows image extraction file";
    return result;
  }
  const auto removePartial = [&] {
    output.close();
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);
  };
  std::vector<unsigned char> buffer(kTransferBytes);
  std::uint64_t offset = 0;
  while (offset < source->sizeBytes) {
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Windows image extraction was cancelled";
      removePartial();
      return result;
    }
    const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        buffer.size(), source->sizeBytes - offset));
    if (!detail::readImageFile(sourceImage, *source, offset, buffer.data(), amount,
                               result.error)) {
      removePartial();
      return result;
    }
    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(amount));
    if (!output) {
      result.error = "Unable to write the extracted Windows install image";
      removePartial();
      return result;
    }
    offset += amount;
    if (onProgress) {
      onProgress({offset, source->sizeBytes});
    }
  }
  output.flush();
  if (!output) {
    result.error = "Unable to flush the extracted Windows install image";
    removePartial();
    return result;
  }
  output.close();
  if (!sourceUnchanged(plan, result.error)) {
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);
    return result;
  }
  result.success = true;
  result.bytesExtracted = offset;
  result.sourceEntry = source->path;
  return result;
}

}  // namespace rufus::core
