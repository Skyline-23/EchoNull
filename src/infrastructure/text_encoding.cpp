#include "infrastructure/text_encoding.hpp"

#include <Windows.h>

#include <stdexcept>

namespace echonull {

std::wstring utf8_to_wide(const std::string& input) {
  if (input.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         input.data(),
                                         static_cast<int>(input.size()),
                                         nullptr, 0);
  if (length <= 0) throw std::runtime_error("invalid UTF-8 text");
  std::wstring output(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                      static_cast<int>(input.size()), output.data(), length);
  return output;
}

std::string wide_to_utf8(const std::wstring& input) {
  if (input.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         input.data(),
                                         static_cast<int>(input.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) throw std::runtime_error("invalid UTF-16 text");
  std::string output(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                      static_cast<int>(input.size()), output.data(), length,
                      nullptr, nullptr);
  return output;
}

}  // namespace echonull
