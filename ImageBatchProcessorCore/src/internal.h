#pragma once
#include "imgbp.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
extern thread_local std::string ibp_error;
#define IBP_TRY try
#define IBP_CATCH                                                              \
  catch (const std::exception &e) {                                            \
    ibp_error = e.what();                                                      \
    return -1;                                                                 \
  }                                                                            \
  catch (...) {                                                                \
    ibp_error = "Unknown failure";                                             \
    return -1;                                                                 \
  }
inline void require(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
inline std::filesystem::path upath(const char *p) {
  require(p, "Null path");
  return std::filesystem::path(reinterpret_cast<const char8_t *>(p));
}
inline float srgb(float x) {
  return x <= 0.0031308f ? 12.92f * x
                         : 1.055f * std::pow(x, 1.f / 2.4f) - .055f;
}
inline float linear(float x) {
  return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}
