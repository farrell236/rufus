/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/backend/block_device_backend.hpp"

namespace rufus::backend {

std::unique_ptr<BlockDeviceBackend> makeMacOSBlockDeviceBackend();
std::unique_ptr<BlockDeviceBackend> makeLinuxBlockDeviceBackend();
std::unique_ptr<BlockDeviceBackend> makeWindowsBlockDeviceBackend();

std::unique_ptr<BlockDeviceBackend> makePlatformBlockDeviceBackend() {
#if defined(__APPLE__)
  return makeMacOSBlockDeviceBackend();
#elif defined(__linux__)
  return makeLinuxBlockDeviceBackend();
#elif defined(_WIN32)
  return makeWindowsBlockDeviceBackend();
#else
  return nullptr;
#endif
}

}  // namespace rufus::backend
