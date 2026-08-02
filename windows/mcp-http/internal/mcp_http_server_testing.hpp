#pragma once

#include <chrono>
#include <cstdint>

namespace palmier::mcp::testing {

std::uint32_t socketTimeoutMilliseconds(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline
) noexcept;

}
