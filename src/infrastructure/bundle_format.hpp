#pragma once

#include <array>
#include <cstdint>

namespace echonull::bundle {

inline constexpr std::array<char, 8> kMagic{'E', 'N', 'B', 'N', 'D', 'L', '0', '1'};
inline constexpr std::uint32_t kVersion = 1;

#pragma pack(push, 1)
struct Entry {
  std::uint32_t name_size = 0;
  std::uint32_t reserved = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct Footer {
  std::array<char, 8> magic{};
  std::uint32_t version = 0;
  std::uint32_t entry_count = 0;
  std::uint64_t original_size = 0;
  std::uint64_t index_offset = 0;
  std::uint64_t index_size = 0;
  std::uint64_t bundle_id = 0;
};
#pragma pack(pop)

static_assert(sizeof(Entry) == 24);
static_assert(sizeof(Footer) == 48);

}  // namespace echonull::bundle
