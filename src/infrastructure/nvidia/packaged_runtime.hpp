#pragma once

#include <filesystem>
#include <vector>

namespace echonull {

struct PackagedRuntimePaths {
  std::filesystem::path directory;
  std::vector<std::filesystem::path> aec_models;
  std::vector<std::filesystem::path> noise_models;
};

class PackagedRuntime {
 public:
  [[nodiscard]] static PackagedRuntimePaths prepare(
      const std::filesystem::path& plugin_path);
};

}  // namespace echonull
