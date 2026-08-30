#pragma once

#include <Windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace echonull {

struct ReferenceBusBlock {
  std::int64_t timestamp_hns = 0;
  std::vector<float> samples;
};

class ReferenceBusWriter {
 public:
  ReferenceBusWriter();
  ~ReferenceBusWriter();

  ReferenceBusWriter(const ReferenceBusWriter&) = delete;
  ReferenceBusWriter& operator=(const ReferenceBusWriter&) = delete;

  void open();
  void close() noexcept;
  void publish(std::int64_t timestamp_hns, std::span<const float> samples);
  [[nodiscard]] bool is_open() const { return view_ != nullptr; }

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
};

class ReferenceBusReader {
 public:
  ReferenceBusReader();
  ~ReferenceBusReader();

  ReferenceBusReader(const ReferenceBusReader&) = delete;
  ReferenceBusReader& operator=(const ReferenceBusReader&) = delete;

  bool open();
  void close() noexcept;
  [[nodiscard]] std::vector<ReferenceBusBlock> read_available();
  [[nodiscard]] bool is_open() const { return view_ != nullptr; }

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  std::int64_t last_sequence_ = 0;
};

}  // namespace echonull
