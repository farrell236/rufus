/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "wim_inspector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace rufus::core::detail {

namespace {

constexpr std::size_t kHeaderSize = 208;
constexpr std::uint64_t kMaximumXmlSize = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kDefaultVersion = 0x00010d00U;
constexpr std::uint32_t kSolidVersion = 0x00000e00U;
constexpr std::uint32_t kWriteInProgress = 0x00000040U;
constexpr unsigned char kCompressedResource = 0x04U;

std::uint16_t littleEndian16(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t littleEndian32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t littleEndian64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t littleEndian56(const unsigned char* data) {
  std::uint64_t value = 0;
  for (unsigned int index = 0; index < 7; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

std::string normalizedPath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  const auto version = path.find(';');
  if (version != std::string::npos) {
    path.erase(version);
  }
  return path;
}

bool isInstallWim(const std::string& path) {
  const std::string normalized = normalizedPath(path);
  return normalized == "sources/install.wim" || normalized == "sources/install.esd" ||
         (normalized.size() > 20U &&
          (normalized.compare(normalized.size() - 20U, 20U, "/sources/install.wim") == 0 ||
           normalized.compare(normalized.size() - 20U, 20U, "/sources/install.esd") == 0));
}

std::string utf16LeToUtf8(const std::vector<unsigned char>& bytes) {
  std::string output;
  output.reserve(bytes.size() / 2U);
  std::size_t offset = bytes.size() >= 2U && bytes[0] == 0xffU && bytes[1] == 0xfeU ? 2U : 0U;
  while (offset + 1U < bytes.size()) {
    std::uint32_t codePoint = littleEndian16(bytes.data() + offset);
    offset += 2U;
    if (codePoint >= 0xd800U && codePoint <= 0xdbffU && offset + 1U < bytes.size()) {
      const std::uint32_t low = littleEndian16(bytes.data() + offset);
      if (low >= 0xdc00U && low <= 0xdfffU) {
        codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) + (low - 0xdc00U);
        offset += 2U;
      }
    }
    if (codePoint <= 0x7fU) {
      output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
      output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
      output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
      output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0x10ffffU) {
      output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
  }
  return output;
}

ImageArchitecture architectureFromNumber(const unsigned int value) {
  switch (value) {
    case 0:
      return ImageArchitecture::X86;
    case 5:
      return ImageArchitecture::Arm;
    case 6:
      return ImageArchitecture::Itanium;
    case 9:
      return ImageArchitecture::X64;
    case 12:
      return ImageArchitecture::Arm64;
    default:
      return ImageArchitecture::Unknown;
  }
}

ImageArchitecture mergeArchitecture(const ImageArchitecture left,
                                    const ImageArchitecture right) {
  if (left == ImageArchitecture::Unknown) {
    return right;
  }
  if (right == ImageArchitecture::Unknown || left == right) {
    return left;
  }
  return ImageArchitecture::Multiple;
}

bool parseUnsigned(const std::string& text, const std::size_t begin,
                   const std::size_t end, unsigned int& value) {
  if (begin >= end) {
    return false;
  }
  unsigned int parsed = 0;
  for (std::size_t index = begin; index < end; ++index) {
    const unsigned char character = static_cast<unsigned char>(text[index]);
    if (!std::isdigit(character) ||
        parsed > (std::numeric_limits<unsigned int>::max() - (character - '0')) / 10U) {
      return false;
    }
    parsed = parsed * 10U + (character - '0');
  }
  value = parsed;
  return true;
}

bool parseFirstTag(const std::string& xml, const std::string& tag,
                   unsigned int& value) {
  const std::string opening = '<' + tag + '>';
  const std::string closing = "</" + tag + '>';
  const std::size_t start = xml.find(opening);
  if (start == std::string::npos) {
    return false;
  }
  const std::size_t valueStart = start + opening.size();
  const std::size_t valueEnd = xml.find(closing, valueStart);
  return valueEnd != std::string::npos && parseUnsigned(xml, valueStart, valueEnd, value);
}

bool parseFirstTag64(const std::string& xml, const std::string& tag,
                     std::uint64_t& value) {
  const std::string opening = '<' + tag + '>';
  const std::string closing = "</" + tag + '>';
  const std::size_t start = xml.find(opening);
  if (start == std::string::npos) {
    return false;
  }
  const std::size_t valueStart = start + opening.size();
  const std::size_t valueEnd = xml.find(closing, valueStart);
  if (valueEnd == std::string::npos || valueStart == valueEnd) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (std::size_t index = valueStart; index < valueEnd; ++index) {
    const unsigned char character = static_cast<unsigned char>(xml[index]);
    if (!std::isdigit(character) ||
        parsed > (std::numeric_limits<std::uint64_t>::max() - (character - '0')) / 10U) {
      return false;
    }
    parsed = parsed * 10U + (character - '0');
  }
  value = parsed;
  return true;
}

std::string decodeXmlText(std::string text) {
  struct Entity final {
    const char* encoded;
    const char* decoded;
  };
  constexpr std::array<Entity, 5> entities{{
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
      {"&quot;", "\""}, {"&apos;", "'"},
  }};
  for (const auto& entity : entities) {
    std::size_t position = 0;
    while ((position = text.find(entity.encoded, position)) != std::string::npos) {
      text.replace(position, std::char_traits<char>::length(entity.encoded), entity.decoded);
      position += std::char_traits<char>::length(entity.decoded);
    }
  }
  return text;
}

std::string firstTextTag(const std::string& xml, const std::string& tag) {
  const std::string opening = '<' + tag + '>';
  const std::string closing = "</" + tag + '>';
  const std::size_t start = xml.find(opening);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t valueStart = start + opening.size();
  const std::size_t valueEnd = xml.find(closing, valueStart);
  return valueEnd == std::string::npos
             ? std::string{}
             : decodeXmlText(xml.substr(valueStart, valueEnd - valueStart));
}

bool parseImageIndex(const std::string& openingTag, std::uint32_t& index) {
  const std::size_t attribute = openingTag.find("INDEX=");
  if (attribute == std::string::npos) {
    return false;
  }
  const std::size_t quote = attribute + 6U;
  if (quote >= openingTag.size() ||
      (openingTag[quote] != '\"' && openingTag[quote] != '\'')) {
    return false;
  }
  const std::size_t end = openingTag.find(openingTag[quote], quote + 1U);
  unsigned int parsed = 0;
  if (end == std::string::npos ||
      !parseUnsigned(openingTag, quote + 1U, end, parsed) || parsed == 0) {
    return false;
  }
  index = parsed;
  return true;
}

bool inspectXml(const std::string& xml, const std::uint32_t expectedImages,
                ImageArchitecture& architecture, std::uint32_t& versionMajor,
                std::uint32_t& versionMinor, std::uint32_t& build,
                std::vector<WindowsEditionInfo>& editions) {
  if (xml.find("<WIM") == std::string::npos || xml.find("</WIM>") == std::string::npos) {
    return false;
  }
  std::size_t imageCount = 0;
  for (std::size_t position = 0; (position = xml.find("<IMAGE", position)) != std::string::npos;) {
    const std::size_t afterName = position + 6U;
    if (afterName < xml.size() &&
        (std::isspace(static_cast<unsigned char>(xml[afterName])) || xml[afterName] == '>')) {
      ++imageCount;
      const std::size_t openingEnd = xml.find('>', afterName);
      const std::size_t imageEnd = openingEnd == std::string::npos
                                       ? std::string::npos
                                       : xml.find("</IMAGE>", openingEnd + 1U);
      if (imageEnd == std::string::npos) {
        return false;
      }
      WindowsEditionInfo edition;
      if (!parseImageIndex(xml.substr(position, openingEnd - position + 1U), edition.index)) {
        return false;
      }
      const std::string body = xml.substr(openingEnd + 1U, imageEnd - openingEnd - 1U);
      edition.name = firstTextTag(body, "DISPLAYNAME");
      if (edition.name.empty()) {
        edition.name = firstTextTag(body, "NAME");
      }
      edition.description = firstTextTag(body, "DESCRIPTION");
      unsigned int value = 0;
      if (parseFirstTag(body, "ARCH", value)) {
        edition.architecture = architectureFromNumber(value);
      }
      if (parseFirstTag(body, "MAJOR", value)) {
        edition.versionMajor = value;
      }
      if (parseFirstTag(body, "MINOR", value)) {
        edition.versionMinor = value;
      }
      if (parseFirstTag(body, "BUILD", value)) {
        edition.build = value;
      }
      static_cast<void>(parseFirstTag64(body, "TOTALBYTES", edition.totalBytes));
      if (edition.name.empty()) {
        edition.name = "Windows image " + std::to_string(edition.index);
      }
      editions.push_back(std::move(edition));
      position = imageEnd + 8U;
      continue;
    }
    position = afterName;
  }
  if (imageCount != expectedImages || editions.size() != expectedImages) {
    return false;
  }

  std::size_t position = 0;
  while ((position = xml.find("<ARCH>", position)) != std::string::npos) {
    const std::size_t valueStart = position + 6U;
    const std::size_t valueEnd = xml.find("</ARCH>", valueStart);
    if (valueEnd == std::string::npos) {
      return false;
    }
    unsigned int value = 0;
    if (!parseUnsigned(xml, valueStart, valueEnd, value)) {
      return false;
    }
    architecture = mergeArchitecture(architecture, architectureFromNumber(value));
    position = valueEnd + 7U;
  }
  unsigned int parsedMajor = 0;
  unsigned int parsedMinor = 0;
  unsigned int parsedBuild = 0;
  if (!parseFirstTag(xml, "MAJOR", parsedMajor) ||
      !parseFirstTag(xml, "MINOR", parsedMinor) ||
      !parseFirstTag(xml, "BUILD", parsedBuild)) {
    return false;
  }
  versionMajor = parsedMajor;
  versionMinor = parsedMinor;
  build = parsedBuild;
  return true;
}

}  // namespace

WimInspectionResult inspectWindowsImage(const std::filesystem::path& imagePath,
                                        const std::vector<ImageFileRecord>& files) {
  WimInspectionResult result;
  const auto file = std::find_if(files.begin(), files.end(), [](const ImageFileRecord& entry) {
    return isInstallWim(entry.path);
  });
  if (file == files.end()) {
    return result;
  }
  result.found = true;
  if (file->sizeBytes < kHeaderSize) {
    result.warnings.emplace_back("Windows install image has a truncated WIM header");
    return result;
  }

  std::array<unsigned char, kHeaderSize> header{};
  std::string error;
  if (!readImageFile(imagePath, *file, 0, header.data(), header.size(), error)) {
    result.warnings.emplace_back("Unable to read Windows install image metadata: " + error);
    return result;
  }
  constexpr std::array<unsigned char, 8> magic{'M', 'S', 'W', 'I', 'M', 0, 0, 0};
  if (!std::equal(magic.begin(), magic.end(), header.begin()) ||
      littleEndian32(header.data() + 8) != kHeaderSize) {
    result.warnings.emplace_back("Windows install image has an invalid WIM header");
    return result;
  }
  const std::uint32_t version = littleEndian32(header.data() + 12);
  const std::uint32_t flags = littleEndian32(header.data() + 16);
  const std::uint16_t partNumber = littleEndian16(header.data() + 40);
  const std::uint16_t totalParts = littleEndian16(header.data() + 42);
  result.imageCount = littleEndian32(header.data() + 44);
  result.bootIndex = littleEndian32(header.data() + 120);
  if ((version != kDefaultVersion && version != kSolidVersion) ||
      (flags & kWriteInProgress) != 0 || partNumber == 0 || totalParts == 0 ||
      partNumber > totalParts || totalParts != 1 || result.imageCount == 0 ||
      result.imageCount > 65535U || result.bootIndex > result.imageCount) {
    result.warnings.emplace_back("Windows install image has inconsistent WIM metadata");
    return result;
  }

  const unsigned char* resource = header.data() + 72;
  const std::uint64_t storedSize = littleEndian56(resource);
  const unsigned char resourceFlags = resource[7];
  const std::uint64_t resourceOffset = littleEndian64(resource + 8);
  const std::uint64_t expandedSize = littleEndian64(resource + 16);
  if (storedSize == 0 || storedSize != expandedSize || storedSize > kMaximumXmlSize ||
      (resourceFlags & kCompressedResource) != 0 || resourceOffset > file->sizeBytes ||
      storedSize > file->sizeBytes - resourceOffset ||
      storedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    result.warnings.emplace_back("Windows install image has an invalid XML metadata resource");
    return result;
  }

  std::vector<unsigned char> xmlBytes(static_cast<std::size_t>(storedSize));
  if (!readImageFile(imagePath, *file, resourceOffset, xmlBytes.data(), xmlBytes.size(), error)) {
    result.warnings.emplace_back("Unable to read Windows install image XML metadata: " + error);
    return result;
  }
  const std::string xml = utf16LeToUtf8(xmlBytes);
  if (!inspectXml(xml, result.imageCount, result.architecture,
                  result.versionMajor, result.versionMinor, result.build,
                  result.editions)) {
    result.warnings.emplace_back("Windows install image XML metadata is inconsistent");
    return result;
  }
  result.valid = true;
  return result;
}

}  // namespace rufus::core::detail
