#pragma once

#include <memory>

#include "platform/platform.h"

namespace jie2::platform {

std::unique_ptr<Platform> createWindowsPlatform();

} // namespace jie2::platform
