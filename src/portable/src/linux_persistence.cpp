/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/linux_persistence.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "linux_persistence_support.hpp"

namespace rufus::core {

namespace {

struct Token final {
  std::size_t begin{};
  std::size_t end{};
  std::string folded;
};

std::string asciiFold(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::vector<Token> tokens(const std::string_view line) {
  std::vector<Token> result;
  for (std::size_t offset = 0; offset < line.size();) {
    while (offset < line.size() &&
           std::isspace(static_cast<unsigned char>(line[offset])) != 0) {
      ++offset;
    }
    if (offset == line.size() || line[offset] == '#') {
      break;
    }
    const std::size_t begin = offset;
    while (offset < line.size() &&
           std::isspace(static_cast<unsigned char>(line[offset])) == 0) {
      ++offset;
    }
    result.push_back({begin, offset,
                      asciiFold(std::string(line.substr(begin, offset - begin)))});
  }
  return result;
}

bool hasOption(const std::vector<Token>& parsed, const std::string_view option) {
  return std::any_of(parsed.begin() + std::min<std::size_t>(1, parsed.size()),
                     parsed.end(), [option](const Token& token) {
                       return token.folded == option;
                     });
}

bool commandIs(const std::vector<Token>& parsed,
               const std::initializer_list<std::string_view> commands) {
  if (parsed.empty()) {
    return false;
  }
  return std::any_of(commands.begin(), commands.end(), [&](const std::string_view command) {
    return parsed.front().folded == command;
  });
}

bool insertBeforeToken(std::string& line, const std::vector<Token>& parsed,
                       const std::string_view prefix,
                       const std::string_view option) {
  for (std::size_t index = 1; index < parsed.size(); ++index) {
    if (parsed[index].folded.rfind(prefix, 0) == 0) {
      line.insert(parsed[index].begin, std::string(option) + ' ');
      return true;
    }
  }
  return false;
}

bool insertAfterToken(std::string& line, const std::vector<Token>& parsed,
                      const std::string_view prefix,
                      const std::string_view option) {
  for (std::size_t index = 1; index < parsed.size(); ++index) {
    if (parsed[index].folded.rfind(prefix, 0) == 0) {
      line.insert(parsed[index].end, std::string(" ") + std::string(option));
      return true;
    }
  }
  return false;
}

bool removeOption(std::string& line, const std::string_view option) {
  const auto parsed = tokens(line);
  for (std::size_t index = parsed.size(); index-- > 1U;) {
    if (parsed[index].folded != option) {
      continue;
    }
    std::size_t begin = parsed[index].begin;
    if (begin != 0U && line[begin - 1U] == ' ') {
      --begin;
    }
    line.erase(begin, parsed[index].end - begin);
    return true;
  }
  return false;
}

enum class PatchPattern {
  None,
  Preseed,
  CasperBoot,
  CasperLinux,
  CasperKernel,
  DebianBoot,
};

bool lineMatchesPattern(const std::string_view line, const bool grub,
                        const PatchPattern pattern) {
  const auto parsed = tokens(line);
  const bool kernelArguments = grub
                                   ? commandIs(parsed, {"linux", "linuxefi"})
                                   : commandIs(parsed, {"append"});
  const auto hasPrefix = [&](const std::string_view prefix) {
    return std::any_of(parsed.begin() + std::min<std::size_t>(1, parsed.size()),
                       parsed.end(), [prefix](const Token& token) {
                         return token.folded.rfind(prefix, 0) == 0;
                       });
  };
  switch (pattern) {
    case PatchPattern::None:
      return false;
    case PatchPattern::Preseed:
      return kernelArguments && hasPrefix("file=/cdrom/preseed");
    case PatchPattern::CasperBoot:
      return kernelArguments && hasPrefix("boot=casper");
    case PatchPattern::CasperLinux:
      return commandIs(parsed, {"linux", "linuxefi"}) &&
             hasPrefix("/casper/vmlinuz");
    case PatchPattern::CasperKernel:
      return commandIs(parsed, {"kernel"}) && hasPrefix("/casper/vmlinuz");
    case PatchPattern::DebianBoot:
      return kernelArguments && hasPrefix("boot=live");
  }
  return false;
}

bool patchLine(std::string& line, const bool grub, const PatchPattern pattern,
               const LinuxPersistenceStyle style) {
  auto parsed = tokens(line);
  const std::string_view option =
      style == LinuxPersistenceStyle::Casper ? "persistent" : "persistence";
  if (hasOption(parsed, option) || !lineMatchesPattern(line, grub, pattern)) {
    return false;
  }
  switch (pattern) {
    case PatchPattern::Preseed:
      if (!insertBeforeToken(line, parsed, "file=/cdrom/preseed", option)) {
        return false;
      }
      if (grub) {
        static_cast<void>(removeOption(line, "maybe-ubiquity"));
      }
      return true;
    case PatchPattern::CasperBoot:
      return insertAfterToken(line, parsed, "boot=casper", option);
    case PatchPattern::CasperLinux:
    case PatchPattern::CasperKernel:
      return insertAfterToken(line, parsed, "/casper/vmlinuz", option);
    case PatchPattern::DebianBoot:
      return insertAfterToken(line, parsed, "boot=live", option);
    case PatchPattern::None:
      return false;
  }
  return false;
}

}  // namespace

LinuxPersistencePatchResult patchLinuxPersistenceBootConfiguration(
    const std::string_view path, const std::string_view contents,
    const LinuxPersistenceStyle style) {
  LinuxPersistencePatchResult result;
  result.contents.assign(contents);
  if (style == LinuxPersistenceStyle::None ||
      !detail::isLinuxBootConfigurationPath(path)) {
    return result;
  }
  result.recognizedConfiguration = true;
  const std::string foldedPath = detail::normalizedPersistencePath(std::string(path));
  const std::size_t slash = foldedPath.find_last_of('/');
  const std::string basename = foldedPath.substr(
      slash == std::string::npos ? 0U : slash + 1U);
  const bool grub = basename == "grub.cfg" || basename == "loopback.cfg";

  struct Line final {
    std::string text;
    bool carriageReturn{};
    bool newline{};
  };
  std::vector<Line> lines;
  for (std::size_t begin = 0; begin < result.contents.size();) {
    const std::size_t newline = result.contents.find('\n', begin);
    const std::size_t end = newline == std::string::npos
                                ? result.contents.size()
                                : newline;
    Line line;
    line.text = result.contents.substr(begin, end - begin);
    line.carriageReturn = !line.text.empty() && line.text.back() == '\r';
    line.newline = newline != std::string::npos;
    if (line.carriageReturn) {
      line.text.pop_back();
    }
    lines.push_back(std::move(line));
    begin = newline == std::string::npos ? result.contents.size() : newline + 1U;
  }

  const std::array<PatchPattern, 4> casperPatterns = {
      PatchPattern::Preseed, PatchPattern::CasperBoot,
      PatchPattern::CasperLinux, PatchPattern::CasperKernel};
  PatchPattern selected = PatchPattern::None;
  if (style == LinuxPersistenceStyle::Casper) {
    for (const auto candidate : casperPatterns) {
      if (std::any_of(lines.begin(), lines.end(), [&](const Line& line) {
            return lineMatchesPattern(line.text, grub, candidate);
          })) {
        selected = candidate;
        break;
      }
    }
  } else if (std::any_of(lines.begin(), lines.end(), [&](const Line& line) {
               return lineMatchesPattern(line.text, grub,
                                         PatchPattern::DebianBoot);
             })) {
    selected = PatchPattern::DebianBoot;
  }

  std::string patched;
  patched.reserve(result.contents.size() + 128U);
  for (auto& line : lines) {
    result.modified = patchLine(line.text, grub, selected, style) ||
                      result.modified;
    patched += line.text;
    if (line.carriageReturn) {
      patched.push_back('\r');
    }
    if (line.newline) {
      patched.push_back('\n');
    }
  }
  result.contents = std::move(patched);
  return result;
}

const char* linuxPersistenceStyleName(const LinuxPersistenceStyle style) noexcept {
  switch (style) {
    case LinuxPersistenceStyle::None:
      return "Disabled";
    case LinuxPersistenceStyle::Casper:
      return "Ubuntu/Casper";
    case LinuxPersistenceStyle::DebianLive:
      return "Debian Live";
  }
  return "Disabled";
}

namespace detail {

std::string normalizedPersistencePath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.rfind("./", 0) == 0) {
    path.erase(0, 2);
  }
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  const std::size_t version = path.find(';');
  if (version != std::string::npos) {
    path.erase(version);
  }
  return asciiFold(std::move(path));
}

bool isLinuxBootConfigurationPath(const std::string_view path) {
  const std::string normalized = normalizedPersistencePath(std::string(path));
  const std::size_t slash = normalized.find_last_of('/');
  const std::string basename = normalized.substr(
      slash == std::string::npos ? 0U : slash + 1U);
  constexpr std::array<std::string_view, 8> recognized = {
      "grub.cfg", "loopback.cfg", "menu.cfg", "isolinux.cfg",
      "syslinux.cfg", "extlinux.conf", "txt.cfg", "live.cfg"};
  return std::find(recognized.begin(), recognized.end(), basename) !=
         recognized.end();
}

bool isMd5ManifestPath(const std::string_view path) {
  const std::string normalized = normalizedPersistencePath(std::string(path));
  return normalized == "md5sums" || normalized == "md5sum.txt";
}

namespace {

std::uint32_t rotateLeft(const std::uint32_t value, const unsigned int count) {
  return (value << count) | (value >> (32U - count));
}

}  // namespace

std::string md5Hex(const std::string_view contents) {
  static constexpr std::array<unsigned int, 64> shifts = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  static constexpr std::array<std::uint32_t, 64> constants = {
      0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
      0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
      0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
      0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
      0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
      0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
      0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
      0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
      0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
      0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
      0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
      0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
      0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
      0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
      0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
      0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};

  std::vector<unsigned char> message(contents.begin(), contents.end());
  const std::uint64_t bitLength = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while (message.size() % 64U != 56U) {
    message.push_back(0);
  }
  for (unsigned int index = 0; index < 8; ++index) {
    message.push_back(static_cast<unsigned char>((bitLength >> (8U * index)) & 0xffU));
  }

  std::uint32_t a0 = 0x67452301U;
  std::uint32_t b0 = 0xefcdab89U;
  std::uint32_t c0 = 0x98badcfeU;
  std::uint32_t d0 = 0x10325476U;
  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    std::array<std::uint32_t, 16> words{};
    for (unsigned int word = 0; word < words.size(); ++word) {
      for (unsigned int byte = 0; byte < 4; ++byte) {
        words[word] |= static_cast<std::uint32_t>(
                           message[offset + word * 4U + byte])
                       << (8U * byte);
      }
    }
    std::uint32_t a = a0;
    std::uint32_t b = b0;
    std::uint32_t c = c0;
    std::uint32_t d = d0;
    for (unsigned int index = 0; index < 64; ++index) {
      std::uint32_t function = 0;
      unsigned int word = 0;
      if (index < 16U) {
        function = (b & c) | (~b & d);
        word = index;
      } else if (index < 32U) {
        function = (d & b) | (~d & c);
        word = (5U * index + 1U) % 16U;
      } else if (index < 48U) {
        function = b ^ c ^ d;
        word = (3U * index + 5U) % 16U;
      } else {
        function = c ^ (b | ~d);
        word = (7U * index) % 16U;
      }
      const std::uint32_t previousD = d;
      d = c;
      c = b;
      b += rotateLeft(a + function + constants[index] + words[word], shifts[index]);
      a = previousD;
    }
    a0 += a;
    b0 += b;
    c0 += c;
    d0 += d;
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint32_t value : {a0, b0, c0, d0}) {
    for (unsigned int index = 0; index < 4; ++index) {
      output << std::setw(2) << ((value >> (8U * index)) & 0xffU);
    }
  }
  return output.str();
}

bool updateMd5Manifest(
    std::string& manifest,
    const std::unordered_map<std::string, std::string>& replacements) {
  bool modified = false;
  for (std::size_t begin = 0; begin < manifest.size();) {
    const std::size_t newline = manifest.find('\n', begin);
    const std::size_t end = newline == std::string::npos ? manifest.size() : newline;
    if (end - begin >= 34U) {
      bool digest = true;
      for (std::size_t index = 0; index < 32U; ++index) {
        digest = digest && std::isxdigit(
                               static_cast<unsigned char>(manifest[begin + index])) != 0;
      }
      std::size_t path = begin + 32U;
      while (path < end && std::isspace(static_cast<unsigned char>(manifest[path])) != 0) {
        ++path;
      }
      if (path < end && manifest[path] == '*') {
        ++path;
      }
      if (digest && path < end) {
        std::string normalized = normalizedPersistencePath(
            manifest.substr(path, end - path));
        const auto replacement = replacements.find(normalized);
        if (replacement != replacements.end() && replacement->second.size() == 32U) {
          manifest.replace(begin, 32U, replacement->second);
          modified = true;
        }
      }
    }
    begin = newline == std::string::npos ? manifest.size() : newline + 1U;
  }
  return modified;
}

}  // namespace detail

}  // namespace rufus::core
