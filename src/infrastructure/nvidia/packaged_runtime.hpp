#pragma once

#include <filesystem>

namespace echonull {

struct PackagedRuntimePaths {
  std::filesystem::path directory;
  std::filesystem::path aec_model;
  std::filesystem::path noise_model;
};

class PackagedRuntime {
 public:
  [[nodiscard]] static PackagedRuntimePaths prepare(
      const std::filesystem::path& plugin_path);
};

}  // namespace echonull
