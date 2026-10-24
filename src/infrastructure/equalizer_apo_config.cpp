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
  // Remove only our exact obsolete wrapper. Never consume user filters or
  // conditions, even when they were placed between our old marker comments.
  const auto old_lines = split_lines(config);
  std::vector<std::string> unwrapped;
  bool removed_wrapper = false;
  for (std::size_t index = 0; index < old_lines.size(); ++index) {
    if (index + 4 < old_lines.size() &&
        lowercase_trimmed(old_lines[index]) == lowercase_trimmed(kBeginMarker) &&
        lowercase_trimmed(old_lines[index + 1]) == "if: stage == \"capture\"" &&
        is_active_echonull_plugin(old_lines[index + 2]) &&
        lowercase_trimmed(old_lines[index + 3]) == "endif:" &&
        lowercase_trimmed(old_lines[index + 4]) == lowercase_trimmed(kEndMarker)) {
      unwrapped.push_back("Stage: capture");
      unwrapped.push_back(old_lines[index + 2]);
      // Preserve the former scope of filters after the generated conditional.
      bool has_following_filter = false;
      for (std::size_t next = index + 5; next < old_lines.size(); ++next) {
        const auto line = lowercase_trimmed(old_lines[next]);
        if (line.empty() || line.starts_with('#')) continue;
        has_following_filter = !line.starts_with("stage:");
        break;
      }
      if (has_following_filter) {
        std::string prior_stage = "Stage: post-mix capture";
        for (std::size_t prior = 0; prior < index; ++prior) {
          if (lowercase_trimmed(old_lines[prior]).starts_with("stage:"))
            prior_stage = old_lines[prior];
        }
        if (lowercase_trimmed(prior_stage) != "stage: capture")
          unwrapped.push_back(prior_stage);
      }
      index += 4;
      removed_wrapper = true;
    } else {
      unwrapped.push_back(old_lines[index]);
    }
  }
  if (removed_wrapper) {
    // Collapse only adjacent capture rows created by the migration itself.
    for (std::size_t index = 1; index < unwrapped.size();) {
      if (lowercase_trimmed(unwrapped[index - 1]) == "stage: capture" &&
          lowercase_trimmed(unwrapped[index]) == "stage: capture")
        unwrapped.erase(unwrapped.begin() + static_cast<std::ptrdiff_t>(index));
      else ++index;
    }
    return install_echonull_capture_scope(
        join_lines(unwrapped, newline, trailing_newline));
  }
  // Existing capture scopes, including legacy setup blocks, are user state.
  // Updates must leave them exactly as they are instead of cleaning them up.
  if (has_capture_scoped_echonull(config)) return std::string(config);
  const auto input = split_lines(config);
  std::vector<std::string> output;
  output.reserve(input.size() + 2);
  std::vector<ConditionalFrame> conditions;
  constexpr auto no_stage = static_cast<std::size_t>(-1);
  std::size_t reusable_stage = no_stage;
  std::string original_stage = "Stage: post-mix capture";
  std::string pending_restore;
  bool capture_only = false;
  bool found_plugin = false;

  for (const auto& line : input) {
    const auto normalized = lowercase_trimmed(line);
    if (normalized.empty() || normalized.starts_with('#')) {
      output.push_back(line);
      continue;
    }
    if (normalized.starts_with("stage:")) {
      // A following explicit Stage supersedes any restoration we would need.
      pending_restore.clear();
      original_stage = line;
      capture_only = lowercase_trimmed(normalized.substr(6)) == "capture";
      reusable_stage = output.size();
      output.push_back(line);
      continue;
    }
    if (is_active_echonull_plugin(line)) {
      found_plugin = true;
      if (!capture_only && !capture_condition_active(conditions)) {
        if (reusable_stage != no_stage) {
          output[reusable_stage] = "Stage: capture";
        } else {
          output.emplace_back("Stage: capture");
        }
        capture_only = true;
        pending_restore = original_stage;
      }
      output.push_back(line);  // Retain location and ChunkData byte-for-byte.
      reusable_stage = no_stage;
      continue;
    }
    if (!pending_restore.empty()) {
      // Other filters keep their former stage. A single-EchoNull config needs
      // no restore row, comments, marker blocks, or generated conditionals.
      output.push_back(std::move(pending_restore));
      pending_restore.clear();
      capture_only = false;
    }
    reusable_stage = no_stage;
    if (normalized.starts_with("if:")) {
      const bool capture =
          lowercase_trimmed(normalized.substr(3)) == "stage == \"capture\"";
      conditions.push_back({capture, capture});
    } else if (normalized.starts_with("elseif:") || normalized == "else:") {
      if (!conditions.empty() && conditions.back().capture_condition) {
        conditions.back().active = false;
      }
    } else if (normalized == "endif:") {
      if (!conditions.empty()) conditions.pop_back();
    }
    output.push_back(line);
  }

  if (!found_plugin) {
    if (!output.empty() && !output.back().empty()) output.emplace_back();
    if (!capture_only) output.emplace_back("Stage: capture");
    output.emplace_back(kDefaultPluginLine);
  }
  return join_lines(output, newline, trailing_newline || !found_plugin);
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
    if (!is_active_echonull_plugin(line)) filtered.push_back(line);
  }
  if (!filtered.empty() && filtered.back().empty()) filtered.pop_back();
  return join_lines(filtered, newline, trailing_newline && !filtered.empty());
}

}  // namespace echonull
