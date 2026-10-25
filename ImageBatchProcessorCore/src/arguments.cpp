#include "internal.h"
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <vector>
static_assert(std::endian::native == std::endian::little,
              "imgarg encoder requires little endian");
static uint32_t crc32(const unsigned char *p, size_t n) {
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int j = 0; j < 8; j++)
      c = (c >> 1) ^ (0xedb88320u & uint32_t(-int(c & 1)));
  }
  return ~c;
}
template <class T>
static void append(std::vector<unsigned char> &b, const T &v) {
  auto p = (const unsigned char *)&v;
  b.insert(b.end(), p, p + sizeof(T));
}
extern "C" {
void IBP_DefaultParams(IBP_Params *p) {
  if (!p)
    return;
  *p = {};
  p->v[1] = p->v[5] = p->v[6] = p->v[11] = p->v[24] = 1;
  p->v[18] = 90;
  p->v[31]=1;p->v[49]=p->v[50]=.5f;p->v[51]=.5f;p->v[52]=.2f;p->v[58]=1;
  for (int i = 0; i < 17; i++) {
    p->v[32 + i] = p->v[96 + i] = p->v[113 + i] = p->v[130 + i] = i / 16.f;
  }
  p->v[160] = p->v[164] = p->v[168] = 1;
}
void IBP_DefaultTarget(IBP_Target *t) {
  if (!t)
    return;
  *t = {};
  IBP_DefaultParams(&t->parameters);
  t->medianLow = .40;
  t->medianHigh = .58;
  t->p99Low = .85;
  t->p99High = .98;
  t->maxWhiteClip = .005;
  t->maxBlackClip = .015;
  t->colorfulnessLow = .12;
  t->colorfulnessHigh = .36;
  t->parameters.v[7] = .08f;
  t->parameters.v[12] = .18f;
  t->parameters.v[28] = 1.f;
}
int IBP_SetParam(IBP_Params *p, const char *key, float v) {
  IBP_TRY {
    require(p && key && std::isfinite(v), "Invalid parameter");
    static const std::map<std::string, int> names = {
        {"exposure", 0},
        {"contrast", 1},
        {"shadows", 2},
        {"highlights", 3},
        {"black", 4},
        {"white", 5},
        {"saturation", 6},
        {"vibrance", 7},
        {"hue", 8},
        {"temperature", 9},
        {"tint", 10},
        {"gamma", 11},
        {"sharpen", 12},
        {"denoise", 13},
        {"clarity", 14},
        {"dehaze", 15},
        {"vignette", 16},
        {"panini", 17},
        {"hfov", 18},
        {"rotation", 19},
        {"k1", 20},
        {"k2", 21},
        {"perspective_x", 22},
        {"perspective_y", 23},
        {"zoom", 24},
        {"chromatic_aberration", 25},
        {"grain", 26},
        {"monochrome", 27},
        {"shoulder", 28},
        {"mask_type",30},{"mask_strength",31},{"mask_x",49},{"mask_y",50},
        {"mask_radius",51},{"mask_feather",52},{"mask_angle",53},
        {"local_exposure",54},{"local_saturation",55},{"mask_invert",56},
        {"mask_luma_low",57},{"mask_luma_high",58}};
    auto it = names.find(key);
    require(it != names.end(), "Unknown parameter name");
    p->v[it->second] = v;
    return 0;
  }
  IBP_CATCH
}
int IBP_SuggestParams(const IBP_Analysis *a, const IBP_Target *t,
                      IBP_Params *p) {
  IBP_TRY {
    require(a && t && p, "Null target matching argument");
    *p = t->parameters;
    double target = (t->medianLow + t->medianHigh) * .5;
    double median = std::max(a->median, .025);
    double desired = std::log2(
        std::max(.001f, linear(float(target))) /
        std::max(.001f,
                 linear(float(median)))); // Preserve low-key scenes rather than
                                          // flattening scene intent.
    if (a->median > .60)
      desired = std::max(desired, -.40); // High-key content remains high-key.
    double maxEV = (a->meanSaturation > .65 && a->median < .25) ? 1.45 : 2.4;
    desired = std::clamp(desired, -.75, maxEV);
    p->v[0] += float(desired * .85);
    p->v[2] += float(std::clamp((.24 - a->p05) * 2., 0., .48));
    p->v[3] -= float(std::clamp((a->p95 - .75) * 1.1, 0., .23));
    p->v[13] = float(std::clamp(a->noiseEstimate * 12., .06, .30));
    if (a->whiteClip > t->maxWhiteClip || a->p99 > t->p99High)
      p->v[28] = 1;
    if (a->blackClip > t->maxBlackClip)
      p->v[2] += .12f;
    if (a->labB > 15 && a->meanR > 2 * a->meanB)
      p->v[9] -= .65f;
    if (a->colorfulness > t->colorfulnessHigh)
      p->v[6] *= float(std::max(.85, t->colorfulnessHigh / a->colorfulness));
    if (a->colorfulness < t->colorfulnessLow)
      p->v[7] += .04f;
    return 0;
  }
  IBP_CATCH
}
int IBP_SaveTarget(const char *path, const IBP_Target *t) {
  IBP_TRY {
    require(t, "Null target");
    std::vector<unsigned char> payload;
    for (float x : t->parameters.v) {
      require(std::isfinite(x), "Non-finite parameter");
      append(payload, x);
    }
    const double *ranges = &t->medianLow;
    for (int i = 0; i < 8; i++) {
      require(std::isfinite(ranges[i]), "Non-finite target");
      append(payload, ranges[i]);
    }
    const double *metrics = &t->reference.mean;
    for (int i = 0; i < 23; i++)
      append(payload, metrics[i]);
    append(payload, t->reference.samples);
    for (auto &h : t->reference.histogram)
      for (auto x : h)
        append(payload, x);
    std::ofstream f(upath(path), std::ios::binary);
    require(bool(f), "Cannot write target");
    f.write("IMGARG\r\n", 8);
    uint32_t header[] = {1,
                         0x01020304,
                         32,
                         uint32_t(payload.size()),
                         crc32(payload.data(), payload.size()),
                         1};
    f.write((char *)header, 24);
    f.write((char *)payload.data(), payload.size());
    require(bool(f), "Target write failed");
    return 0;
  }
  IBP_CATCH
}
int IBP_LoadTarget(const char *path, IBP_Target *t) {
  IBP_TRY {
    require(t, "Null target");
    std::ifstream f(upath(path), std::ios::binary | std::ios::ate);
    require(bool(f), "Cannot open target");
    auto size = f.tellg();
    require(size == 5408, "Invalid imgarg length");
    f.seekg(0);
    char magic[8];
    uint32_t h[6];
    f.read(magic, 8);
    f.read((char *)h, 24);
    require(memcmp(magic, "IMGARG\r\n", 8) == 0 && h[0] == 1 &&
                h[1] == 0x01020304 && h[2] == 32 && h[3] == 5376 && h[5] == 1,
            "Unsupported imgarg header");
    std::vector<unsigned char> b(h[3]);
    f.read((char *)b.data(), b.size());
    require(bool(f) && crc32(b.data(), b.size()) == h[4], "Corrupt imgarg CRC");
    IBP_Target temp{};
    size_t at = 0;
    auto read = [&](void *dst, size_t n) {
      memcpy(dst, b.data() + at, n);
      at += n;
    };
    read(temp.parameters.v, 1024);
    read(&temp.medianLow, 64);
    read(&temp.reference.mean, 184);
    read(&temp.reference.samples, 8);
    read(temp.reference.histogram, 4096);
    for (float v : temp.parameters.v)
      require(std::isfinite(v), "Invalid imgarg parameter");
    auto r = &temp.medianLow;
    for (int i = 0; i < 8; i++)
      require(std::isfinite(r[i]) && r[i] >= 0 && r[i] <= 1,
              "Invalid target range");
    require(temp.medianLow <= temp.medianHigh && temp.p99Low <= temp.p99High &&
                temp.colorfulnessLow <= temp.colorfulnessHigh,
            "Reversed target range");
    *t = temp;
    return 0;
  }
  IBP_CATCH
}
int IBP_WriteAnalysisJson(const char *path, const IBP_Analysis *a) {
  IBP_TRY {
    require(a, "Null analysis");
    std::ofstream f(upath(path));
    require(bool(f), "Cannot write analysis JSON");
    const char *names[] = {"mean",
                           "median",
                           "p01",
                           "p05",
                           "p95",
                           "p99",
                           "rmsContrast",
                           "entropy",
                           "logAverage",
                           "dynamicRangeEV",
                           "blackClip",
                           "whiteClip",
                           "meanSaturation",
                           "colorfulness",
                           "meanR",
                           "meanG",
                           "meanB",
                           "labL",
                           "labA",
                           "labB",
                           "sharpness",
                           "edgeEnergy",
                           "noiseEstimate"};
    f << std::setprecision(9)
      << "{\n  \"schema\": 1, \"space\": \"sRGB D65; linear logAverage\",\n";
    auto p = &a->mean;
    for (int i = 0; i < 23; i++)
      f << "  \"" << names[i] << "\": " << p[i] << ",\n";
    f << "  \"samples\": " << a->samples << ",\n  \"histogramYRGB\": [";
    for (int c = 0; c < 4; c++) {
      if (c)
        f << ",";
      f << "[";
      for (int i = 0; i < 256; i++) {
        if (i)
          f << ",";
        f << a->histogram[c][i];
      }
      f << "]";
    }
    f << "]\n}\n";
    require(bool(f), "Analysis write failed");
    return 0;
  }
  IBP_CATCH
}
}
