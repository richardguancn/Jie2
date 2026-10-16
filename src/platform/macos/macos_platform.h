#pragma once

#include <memory>

#include "platform/platform.h"

namespace jie2::platform {

std::unique_ptr<Platform> createMacPlatform();

} // namespace jie2::platform
