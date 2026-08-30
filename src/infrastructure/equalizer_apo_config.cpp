#include "infrastructure/equalizer_apo_config.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace echonull {
namespace {

constexpr std::string_view kBeginMarker = "# EchoNull setup begin";
constexpr std::string_view kEndMarker = "# EchoNull setup end";
constexpr std::string_view kDefaultPluginLine =
    "VSTPlugin: Library EchoNullPlugin.dll";

std::string lowercase_trimmed(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  std::string normalized(value.substr(first, last - first + 1));
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](const char character) {
                   return static_cast<char>(std::tolower(
                       static_cast<unsigned char>(character)));
                 });
  return normalized;
}

bool is_active_echonull_plugin(const std::string_view line) {
  const auto normalized = lowercase_trimmed(line);
  return !normalized.starts_with('#') &&
         normalized.starts_with("vstplugin:") &&
         normalized.find("echonullplugin.dll") != std::string::npos;
}

std::vector<std::string> split_lines(const std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto length = end == std::string_view::npos ? text.size() - start
                                                      : end - start;
    std::string line(text.substr(start, length));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines,
                       const std::string_view newline,
                       const bool trailing_newline) {
  std::string output;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (index != 0) output.append(newline);
    output.append(lines[index]);
  }
  if (trailing_newline && !lines.empty()) output.append(newline);
  return output;
}

struct ConditionalFrame {
  bool capture_condition = false;
  bool active = false;
};

bool capture_condition_active(const std::vector<ConditionalFrame>& frames) {
  return std::ranges::any_of(frames, [](const ConditionalFrame& frame) {
    return frame.capture_condition && frame.active;
  });
}

}  // namespace

bool has_capture_scoped_echonull(const std::string_view config) {
  bool capture_only_stage = false;
  bool found_plugin = false;
  std::vector<ConditionalFrame> conditions;
  for (const auto& line : split_lines(config)) {
    const auto normalized = lowercase_trimmed(line);
    if (normalized.empty() || normalized.starts_with('#')) continue;
    if (normalized.starts_with("stage:")) {
      capture_only_stage =
          lowercase_trimmed(normalized.substr(6)) == "capture";
    } else if (normalized.starts_with("if:")) {
      const auto expression = lowercase_trimmed(normalized.substr(3));
      const bool capture = expression == "stage == \"capture\"";
      conditions.push_back({capture, capture});
    } else if (normalized.starts_with("elseif:") || normalized == "else:") {
      if (!conditions.empty() && conditions.back().capture_condition) {
        conditions.back().active = false;
      }
    } else if (normalized == "endif:") {
      if (!conditions.empty()) conditions.pop_back();
    } else if (is_active_echonull_plugin(line)) {
      found_plugin = true;
      if (!capture_only_stage && !capture_condition_active(conditions)) {
        return false;
      }
    }
  }
  return found_plugin;
}

std::string install_echonull_capture_scope(const std::string_view config) {
  const std::string_view newline =
      config.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
  const bool trailing_newline = !config.empty() && config.back() == '\n';
  const auto input = split_lines(config);
  std::vector<std::string> filtered;
  filtered.reserve(input.size() + 6);
  std::string plugin_line;
  std::size_t insertion_index = static_cast<std::size_t>(-1);
  bool in_owned_block = false;

  for (const auto& line : input) {
    const auto normalized = lowercase_trimmed(line);
    if (normalized == lowercase_trimmed(kBeginMarker)) {
      if (insertion_index == static_cast<std::size_t>(-1)) {
        insertion_index = filtered.size();
      }
      in_owned_block = true;
      continue;
    }
    if (in_owned_block) {
      if (is_active_echonull_plugin(line) && plugin_line.empty()) {
        plugin_line = line;
      }
      if (normalized == lowercase_trimmed(kEndMarker)) {
        in_owned_block = false;
      }
      continue;
    }
    if (is_active_echonull_plugin(line)) {
      if (plugin_line.empty()) plugin_line = line;
      if (insertion_index == static_cast<std::size_t>(-1)) {
        insertion_index = filtered.size();
      }
      continue;
    }
    filtered.push_back(line);
  }

  if (plugin_line.empty()) plugin_line = std::string(kDefaultPluginLine);
  if (insertion_index == static_cast<std::size_t>(-1)) {
    insertion_index = filtered.size();
    if (!filtered.empty() && !filtered.back().empty()) filtered.emplace_back();
    insertion_index = filtered.size();
  }

  const std::vector<std::string> block{
      std::string(kBeginMarker), "If: stage == \"capture\"", plugin_line,
      "EndIf:", std::string(kEndMarker)};
  filtered.insert(filtered.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                  block.begin(), block.end());
  return join_lines(filtered, newline, trailing_newline || !config.empty());
}

std::string remove_echonull_capture_scope(const std::string_view config) {
  const std::string_view newline =
      config.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
  const bool trailing_newline = !config.empty() && config.back() == '\n';
  std::vector<std::string> filtered;
  bool in_owned_block = false;
  for (const auto& line : split_lines(config)) {
    const auto normalized = lowercase_trimmed(line);
    if (normalized == lowercase_trimmed(kBeginMarker)) {
      in_owned_block = true;
      continue;
    }
    if (in_owned_block) {
      if (normalized == lowercase_trimmed(kEndMarker)) in_owned_block = false;
      continue;
    }
    filtered.push_back(line);
  }
  if (!filtered.empty() && filtered.back().empty()) filtered.pop_back();
  return join_lines(filtered, newline, trailing_newline && !filtered.empty());
}

}  // namespace echonull
