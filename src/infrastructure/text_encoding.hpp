#pragma once

#include <string>

namespace echonull {

std::wstring utf8_to_wide(const std::string& input);
std::string wide_to_utf8(const std::wstring& input);

}  // namespace echonull
