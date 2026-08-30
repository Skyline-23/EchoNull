#pragma once

#include <string>
#include <string_view>

namespace echonull {

[[nodiscard]] bool has_capture_scoped_echonull(std::string_view config);
[[nodiscard]] std::string install_echonull_capture_scope(
    std::string_view config);
[[nodiscard]] std::string remove_echonull_capture_scope(
    std::string_view config);

}  // namespace echonull
