#include "imgbp_editor.h"
#include "internal.h"
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <vector>
static_assert(std::endian::native == std::endian::little);
namespace {
void validate(const IBP_CurveSet &c) {
  for (int k = 0; k < 4; k++) {
    require(c.count[k] >= 2 && c.count[k] <= 32 && c.interpolation[k] <= 1, "Invalid curve layout");
    for (uint32_t j = 0; j < c.count[k]; j++) {
      float x = c.points[k][j][0], y = c.points[k][j][1];
      require(std::isfinite(x) && std::isfinite(y) && x >= 0 && x <= 1 && y >= 0 && y <= 1,
              "Invalid curve point");
      if (j)
        require(x - c.points[k][j - 1][0] >= .00001f, "Curve X coordinates must increase");
    }
    require(c.points[k][0][0] == 0 && c.points[k][c.count[k] - 1][0] == 1,
            "Curve endpoints must span 0..1");
  }
}
uint32_t crc(const std::vector<unsigned char> &b) {
  uint32_t c = ~0u;
  for (auto v : b) {
    c ^= v;
    for (int i = 0; i < 8; i++)
      c = (c >> 1) ^ (0xedb88320u & uint32_t(-int(c & 1)));
  }
  return ~c;
}
void put(std::vector<unsigned char> &b, const void *p, size_t n) {
  auto v = (const unsigned char *)p;
  b.insert(b.end(), v, v + n);
}
void putTarget(std::vector<unsigned char> &b, const IBP_Target &t) {
  put(b, t.parameters.v, 1024);
  put(b, &t.medianLow, 64);
  put(b, &t.reference.mean, 184);
  put(b, &t.reference.samples, 8);
  put(b, t.reference.histogram, 4096);
}
} // namespace
extern "C" {
void IBP_DefaultCurves(IBP_CurveSet *c) {
  if (!c)
    return;
  *c = {};
  for (int k = 0; k < 4; k++) {
    c->count[k] = 2;
    c->interpolation[k] = 1;
    c->points[k][1][0] = c->points[k][1][1] = 1;
    for (int i = 0; i < 1024; i++)
      c->lut[k][i] = i / 1023.f;
  }
}
int IBP_CurvesFromParams(const IBP_Params *p, IBP_CurveSet *c) {
  IBP_TRY {
    require(p && c, "Null curve argument");
    *c = {};
    int offsets[] = {32, 96, 113, 130};
    for (int k = 0; k < 4; k++) {
      c->count[k] = 17;
      c->interpolation[k] = 0;
      for (int i = 0; i < 17; i++) {
        c->points[k][i][0] = i / 16.f;
        c->points[k][i][1] = p->v[offsets[k] + i];
      }
    }
    return IBP_BuildCurves(c);
  }
  IBP_CATCH
}
int IBP_BuildCurves(IBP_CurveSet *c) {
  IBP_TRY {
    require(c, "Null curves");
    validate(*c);
    for (int k = 0; k < 4; k++) {
      int n = int(c->count[k]);
      float h[31]{}, d[31]{}, m[32]{};
      for (int i = 0; i < n - 1; i++) {
        h[i] = c->points[k][i + 1][0] - c->points[k][i][0];
        d[i] = (c->points[k][i + 1][1] - c->points[k][i][1]) / h[i];
      }
      m[0] = d[0];
      m[n - 1] = d[n - 2];
      if (n > 2) {
        auto endpoint = [](float h0, float h1, float d0, float d1) {
          float v = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
          if (v * d0 <= 0)
            return 0.f;
          if (d0 * d1 < 0 && std::abs(v) > 3 * std::abs(d0))
            return 3 * d0;
          return v;
        };
        m[0] = endpoint(h[0], h[1], d[0], d[1]);
        m[n - 1] = endpoint(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
        for (int i = 1; i < n - 1; i++)
          if (d[i - 1] * d[i] > 0) {
            float w1 = 2 * h[i] + h[i - 1], w2 = h[i] + 2 * h[i - 1];
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
          }
      }
      int j = 0;
      for (int i = 0; i < 1024; i++) {
        float x = i / 1023.f;
        while (j < n - 2 && x > c->points[k][j + 1][0])
          j++;
        float t = (x - c->points[k][j][0]) / h[j], a = c->points[k][j][1],
              b = c->points[k][j + 1][1];
        float y = c->interpolation[k]
                      ? (2 * t * t * t - 3 * t * t + 1) * a +
                            (t * t * t - 2 * t * t + t) * h[j] * m[j] +
                            (-2 * t * t * t + 3 * t * t) * b + (t * t * t - t * t) * h[j] * m[j + 1]
                      : a + (b - a) * t;
        c->lut[k][i] = std::clamp(y, 0.f, 1.f);
      }
    }
    return 0;
  }
  IBP_CATCH
}
int IBP_SaveRecipe(const char *path, const IBP_Recipe *r) {
  IBP_TRY {
    require(r, "Null recipe");
    validate(r->curves);
    for (float v : r->target.parameters.v)
      require(std::isfinite(v), "Invalid recipe parameter");
    auto ranges = &r->target.medianLow;
    for (int i = 0; i < 8; i++)
      require(std::isfinite(ranges[i]) && ranges[i] >= 0 && ranges[i] <= 1, "Invalid target range");
    require(ranges[0] <= ranges[1] && ranges[2] <= ranges[3] && ranges[6] <= ranges[7],
            "Reversed target range");
    for (auto &ch : r->curves.lut)
      for (float v : ch)
        require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid curve LUT");
    std::vector<unsigned char> b;
    putTarget(b, r->target);
    put(b, r->curves.count, 16);
    put(b, r->curves.interpolation, 16);
    put(b, r->curves.points, 1024);
    put(b, r->curves.lut, 16384);
    uint32_t h[] = {2, 0x01020304, 32, uint32_t(b.size()), crc(b), 1};
    std::ofstream f(upath(path), std::ios::binary);
    require(bool(f), "Cannot save recipe");
    f.write("IMGARG\r\n", 8);
    f.write((char *)h, 24);
    f.write((char *)b.data(), b.size());
    require(bool(f), "Recipe write failed");
    return 0;
  }
  IBP_CATCH
}
int IBP_LoadRecipe(const char *path, IBP_Recipe *out) {
  IBP_TRY {
    require(out, "Null recipe");
    std::ifstream f(upath(path), std::ios::binary | std::ios::ate);
    require(bool(f), "Cannot open recipe");
    auto size = f.tellg();
    IBP_Recipe r{};
    if (size == 5408) {
      int rc = IBP_LoadTarget(path, &r.target);
      if (rc)
        return rc;
      rc = IBP_CurvesFromParams(&r.target.parameters, &r.curves);
      if (rc)
        return rc;
      *out = r;
      return 0;
    }
    require(size == 22848, "Unsupported recipe length");
    f.seekg(0);
    char magic[8];
    uint32_t h[6];
    f.read(magic, 8);
    f.read((char *)h, 24);
    require(memcmp(magic, "IMGARG\r\n", 8) == 0 && h[0] == 2 && h[1] == 0x01020304 && h[2] == 32 &&
                h[3] == 22816 && h[5] == 1,
            "Unsupported recipe header");
    std::vector<unsigned char> b(h[3]);
    f.read((char *)b.data(), b.size());
    require(bool(f) && crc(b) == h[4], "Recipe CRC mismatch");
    size_t at = 0;
    auto read = [&](void *p, size_t n) {
      memcpy(p, b.data() + at, n);
      at += n;
    };
    read(r.target.parameters.v, 1024);
    read(&r.target.medianLow, 64);
    read(&r.target.reference.mean, 184);
    read(&r.target.reference.samples, 8);
    read(r.target.reference.histogram, 4096);
    read(r.curves.count, 16);
    read(r.curves.interpolation, 16);
    read(r.curves.points, 1024);
    read(r.curves.lut, 16384);
    validate(r.curves);
    for (float v : r.target.parameters.v)
      require(std::isfinite(v), "Invalid recipe parameter");
    auto ranges = &r.target.medianLow;
    for (int i = 0; i < 8; i++)
      require(std::isfinite(ranges[i]) && ranges[i] >= 0 && ranges[i] <= 1, "Invalid target range");
    require(ranges[0] <= ranges[1] && ranges[2] <= ranges[3] && ranges[6] <= ranges[7],
            "Reversed target range");
    for (auto &ch : r.curves.lut)
      for (float v : ch)
        require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid curve LUT");
    *out = r;
    return 0;
  }
  IBP_CATCH
}
}
