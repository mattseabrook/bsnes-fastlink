#pragma once

#include <limits>

#include <nall/stdint.hpp>
#include <nall/traits.hpp>

namespace nall {

template<uint bits> inline auto uclamp(const uintmax x) -> uintmax {
  enum : uintmax { b = 1ull << (bits - 1), y = b * 2 - 1 };
  return y + ((x - y) & -(x < y));  //min(x, y);
}

template<uint bits, typename T, enable_if_t<is_floating_point<T>::value, int> = 0>
inline auto uclamp(const T x) -> uintmax {
  constexpr uint64_t max = bits >= 64 ? std::numeric_limits<uint64_t>::max() : ((1ull << bits) - 1ull);
  if(x <= (T)0.0) return 0;
  if(x >= (T)max) return max;
  return (uint64_t)x;
}

template<uint bits> inline auto uclip(const uintmax x) -> uintmax {
  enum : uintmax { b = 1ull << (bits - 1), m = b * 2 - 1 };
  return (x & m);
}

template<uint bits> inline auto sclamp(const intmax x) -> intmax {
  enum : intmax { b = 1ull << (bits - 1), m = b - 1 };
  return (x > m) ? m : (x < -b) ? -b : x;
}

template<uint bits, typename T, enable_if_t<is_floating_point<T>::value, int> = 0>
inline auto sclamp(const T x) -> intmax {
  constexpr int64_t max = bits >= 64 ? std::numeric_limits<int64_t>::max() : (int64_t)((1ull << (bits - 1)) - 1ull);
  constexpr int64_t min = bits >= 64 ? std::numeric_limits<int64_t>::min() : -(max + 1);
  if(x >= (T)max) return max;
  if(x <= (T)min) return min;
  return (int64_t)x;
}

template<uint bits> inline auto sclip(const intmax x) -> intmax {
  enum : uintmax { b = 1ull << (bits - 1), m = b * 2 - 1 };
  return ((x & m) ^ b) - b;
}

namespace bit {
  constexpr inline auto mask(const char* s, uintmax sum = 0) -> uintmax {
    return (
      *s == '0' || *s == '1' ? mask(s + 1, (sum << 1) | 1) :
      *s == ' ' || *s == '_' ? mask(s + 1, sum) :
      *s ? mask(s + 1, sum << 1) :
      sum
    );
  }

  constexpr inline auto test(const char* s, uintmax sum = 0) -> uintmax {
    return (
      *s == '0' || *s == '1' ? test(s + 1, (sum << 1) | (*s - '0')) :
      *s == ' ' || *s == '_' ? test(s + 1, sum) :
      *s ? test(s + 1, sum << 1) :
      sum
    );
  }

  //lowest(0b1110) == 0b0010
  constexpr inline auto lowest(const uintmax x) -> uintmax {
    return x & -x;
  }

  //clear_lowest(0b1110) == 0b1100
  constexpr inline auto clearLowest(const uintmax x) -> uintmax {
    return x & (x - 1);
  }

  //set_lowest(0b0101) == 0b0111
  constexpr inline auto setLowest(const uintmax x) -> uintmax {
    return x | (x + 1);
  }

  //count number of bits set in a byte
  constexpr inline auto count(uintmax x) -> uint {
    uint count = 0;
    while(x) x &= x - 1, count++;  //clear the least significant bit
    return count;
  }

  //return index of the first bit set (or zero of no bits are set)
  //first(0b1000) == 3
  constexpr inline auto first(uintmax x) -> uint {
    uint first = 0;
    while(x) { if(x & 1) break; x >>= 1; first++; }
    return first;
  }

  //round up to next highest single bit:
  //round(15) == 16, round(16) == 16, round(17) == 32
  constexpr inline auto round(uintmax x) -> uintmax {
    if((x & (x - 1)) == 0) return x;
    while(x & (x - 1)) x &= x - 1;
    return x << 1;
  }
}

}
