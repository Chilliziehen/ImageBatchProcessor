#include "imgbp.h"
#include "imgbp_editor.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>
namespace fs = std::filesystem;
static std::string utf8(const std::wstring &s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string r(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr,
                      nullptr);
  return r;
}
static std::string u8(const fs::path &p) { return utf8(p.wstring()); }
static void ck(int r) {
  if (r)
    throw std::runtime_error(IBP_LastError());
}
struct Host {
  IBP_HostImage i{};
  ~Host() { IBP_FreeHostImage(&i); }
  Host() = default;
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;
};
struct Engine {
  IBP_Context c{};
  Engine() { ck(IBP_Create(nullptr, &c)); }
  ~Engine() { IBP_Destroy(c); }
};
static void resize(IBP_HostImage &in, uint32_t edge) {
  if (!edge || std::max(in.width, in.height) <= edge)
    return;
  double scale = double(edge) / std::max(in.width, in.height);
  uint32_t w = uint32_t(in.width * scale), h = uint32_t(in.height * scale);
  auto p = new float[size_t(w) * h * 4];
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint32_t x0 = uint32_t(double(x) * in.width / w),
               x1 = std::max(x0 + 1, uint32_t(double(x + 1) * in.width / w));
      uint32_t y0 = uint32_t(double(y) * in.height / h),
               y1 = std::max(y0 + 1, uint32_t(double(y + 1) * in.height / h));
      for (int k = 0; k < 4; k++) {
        double sum = 0;
        for (uint32_t yy = y0; yy < y1; yy++)
          for (uint32_t xx = x0; xx < x1; xx++)
            sum += in.rgba[(size_t(yy) * in.width + xx) * 4 + k];
        p[(size_t(y) * w + x) * 4 + k] = float(sum / ((x1 - x0) * (y1 - y0)));
      }
    }
  IBP_FreeHostImage(&in);
  in = {w, h, p};
}
static bool rawfile(const fs::path &p) {
  std::string e = u8(p.extension());
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){return char(std::tolower(c));});
  static std::set<std::string> exts = {
      ".nef", ".nrw", ".cr2", ".cr3", ".crw", ".arw", ".srf", ".sr2", ".dng",
      ".raf", ".rw2", ".orf", ".pef", ".ptx", ".raw", ".rwl", ".srw", ".3fr",
      ".fff", ".iiq", ".kdc", ".dcr", ".mos", ".mrw", ".erf", ".x3f"};
  return exts.contains(e);
}
static void expect(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
static int selftest() {
  Engine e;
  std::cout << IBP_DeviceInfo(e.c) << "\n";
  std::vector<float> pixels(67 * 35 * 4);
  for (size_t i = 0; i < pixels.size() / 4; i++) {
    pixels[i * 4] = float(i % 67) / 200;
    pixels[i * 4 + 1] = .1f;
    pixels[i * 4 + 2] = .2f;
    pixels[i * 4 + 3] = 1;
  }
  IBP_HostImage host{67, 35, pixels.data()};
  IBP_Image ids[2];
  IBP_HostImage inputs[] = {host, host};
  ck(UploadImageBatchMode(e.c, inputs, 2, ids));
  Host round;
  ck(DownloadImage(e.c, ids[0], &round.i));
  expect(memcmp(round.i.rgba, pixels.data(), pixels.size() * 4) == 0,
         "Upload/download exact roundtrip");
  IBP_Params ps[2];
  for (auto &p : ps)
    IBP_DefaultParams(&p);
  ps[1].v[0] = 1;
  ck(IBP_ProcessBatch(e.c, ids, ps, 2));
  Host a, b;
  ck(DownloadImage(e.c, ids[0], &a.i));
  ck(DownloadImage(e.c, ids[1], &b.i));
  for (size_t i = 0; i < pixels.size(); i++) {
    expect(std::abs(a.i.rgba[i] - pixels[i]) < 2e-5,
           "Identity pipeline mismatch");
    if (i % 4 != 3)
      expect(std::abs(b.i.rgba[i] - 2 * pixels[i]) < 3e-5,
             "One EV exposure mismatch");
  }
  IBP_Analysis s;
  ck(IBP_Analyze(e.c, ids[0], &s));
  for (auto &h : s.histogram) {
    uint64_t sum = 0;
    for (auto v : h)
      sum += v;
    expect(sum == 67 * 35, "Histogram conservation");
  }
  expect(s.samples == 67 * 35 && std::isfinite(s.colorfulness) &&
             std::isfinite(s.entropy),
         "Analysis finite");
  IBP_Params changed;
  IBP_DefaultParams(&changed);
  changed.v[6] = 0;
  ck(IBP_ProcessBatch(e.c, ids, &changed, 1));
  Host mono;
  ck(DownloadImage(e.c, ids[0], &mono.i));
  for (size_t i = 0; i < pixels.size(); i += 4)
    expect(std::abs(mono.i.rgba[i] - mono.i.rgba[i + 1]) < 1e-5,
           "HSL saturation zero");
  auto temp = fs::temp_directory_path() / L"imgbp-contract.imgarg";
  IBP_Target target{}, loaded{};
  IBP_DefaultTarget(&target);
  target.reference = s;
  ck(IBP_SaveTarget(u8(temp).c_str(), &target));
  ck(IBP_LoadTarget(u8(temp).c_str(), &loaded));
  expect(memcmp(&target, &loaded, sizeof target) == 0,
         "Target exact roundtrip");
  {
    std::fstream f(temp, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(100);
    char z = 99;
    f.write(&z, 1);
  }
  expect(IBP_LoadTarget(u8(temp).c_str(), &loaded) != 0,
         "CRC must reject mutation");
  fs::remove(temp);
  for (auto id : ids)
    ck(IBP_ReleaseImage(e.c, id));
  expect(IBP_Analyze(e.c, ids[0], &s) != 0, "Stale handle rejection");
  expect(UploadImage(e.c, nullptr, ids) != 0, "Null upload rejection");
  std::cout << "PASS: GPU identity / exposure / HSL / batch / histogram / "
               "handles / imgarg CRC\n";
  return 0;
}
static void help() {
  std::cout << R"(ImageBatchProcessor 1.0 — Vulkan RAW development
imgbp device
imgbp selftest
imgbp target --output natural.imgarg [--set exposure=0.2 ...]
imgbp analyze --input photo.NEF --output analysis.json
imgbp batch --input DIRECTORY --output DIRECTORY [--target natural.imgarg]
  --batch-size 2   --format jpg|tif|png|both   --quality 95
  --preview 1600   --limit 20   --stride 1   --recursive   --resume
  --before        --fixed     --set exposure=0.2
RAW originals are read-only. Existing images are not overwritten.
All file paths accept Unicode. Each output has an .imgarg recipe and analysis JSON.
)";
}
int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  try {
    if (argc < 2) {
      help();
      return 0;
    }
    std::string command = utf8(argv[1]);
    std::map<std::string, std::string> opts;
    std::vector<std::string> overrides;
    std::set<std::string> flags;
    for (int i = 2; i < argc; i++) {
      std::string key = utf8(argv[i]);
      if (key == "--recursive" || key == "--resume" || key == "--before" ||
          key == "--fixed")
        flags.insert(key);
      else {
        static const std::set<std::string> known={"--input","--output","--target","--reference","--set","--batch-size","--format","--quality","--preview","--limit","--stride"};
        expect(known.contains(key), "Unknown command-line option");
        expect(i + 1 < argc, "Missing option value");
        std::string value = utf8(argv[++i]);
        if (key == "--set")
          overrides.push_back(value);
        else
          opts[key] = value;
      }
    }
    auto path = [&](const char *k) {
      expect(opts.contains(k), "Required --input or --output missing");
      return fs::path(reinterpret_cast<const char8_t*>(opts[k].c_str()));
    };
    auto num = [&](const char *k, int fallback) {
      return opts.contains(k) ? std::stoi(opts[k]) : fallback;
    };
    auto apply = [&](IBP_Params &p) {
      for (auto &kv : overrides) {
        auto eq = kv.find('=');
        expect(eq != std::string::npos, "--set requires name=value");
        ck(IBP_SetParam(&p, kv.substr(0, eq).c_str(),
                        std::stof(kv.substr(eq + 1))));
      }
    };
    if (command == "selftest")
      return selftest();
    if (command == "target") {
      IBP_Target t;
      IBP_DefaultTarget(&t);
      if(opts.contains("--reference")) {
        Engine referenceEngine;Host reference;IBP_Image id;
        ck(IBP_LoadImage(opts["--reference"].c_str(),0,&reference.i));
        ck(UploadImage(referenceEngine.c,&reference.i,&id));
        ck(IBP_Analyze(referenceEngine.c,id,&t.reference));
        ck(IBP_ReleaseImage(referenceEngine.c,id));
        t.medianLow=std::max(.01,t.reference.median-.03);
        t.medianHigh=std::min(.99,t.reference.median+.03);
        t.p99Low=std::max(.01,t.reference.p99-.03);
        t.p99High=std::min(.99,t.reference.p99+.03);
        t.colorfulnessLow=std::max(0.,t.reference.colorfulness-.025);
        t.colorfulnessHigh=std::min(1.,t.reference.colorfulness+.025);
      }
      apply(t.parameters);
      ck(IBP_SaveTarget(u8(path("--output")).c_str(), &t));
      std::cout << "Target saved\n";
      return 0;
    }
    Engine engine;
    auto c = engine.c;
    std::cout << IBP_DeviceInfo(c) << std::endl;
    if (command == "device")
      return 0;
    if (command == "analyze") {
      Host h;
      ck(IBP_LoadImage(u8(path("--input")).c_str(), 0, &h.i));
      IBP_Image id;
      ck(UploadImage(c, &h.i, &id));
      IBP_Analysis a;
      ck(IBP_Analyze(c, id, &a));
      ck(IBP_WriteAnalysisJson(u8(path("--output")).c_str(), &a));
      ck(IBP_ReleaseImage(c, id));
      return 0;
    }
    expect(command == "batch", "Unknown command");
    auto input = path("--input"), output = path("--output");
    expect(fs::exists(input), "Input does not exist");
    fs::create_directories(output);
    IBP_Target target;
    IBP_DefaultTarget(&target);
    bool extendedRecipe = false;
    IBP_Recipe loadedRecipe{};
    if (opts.contains("--target")) {
      auto targetPath=fs::path(reinterpret_cast<const char8_t*>(opts["--target"].c_str()));
      if(fs::file_size(targetPath)==5408) ck(IBP_LoadTarget(opts["--target"].c_str(), &target));
      else {ck(IBP_LoadRecipe(opts["--target"].c_str(),&loadedRecipe));target=loadedRecipe.target;extendedRecipe=true;}
    }
    std::string format = opts.contains("--format") ? opts["--format"] : "jpg";
    expect(format == "jpg" || format == "tif" || format == "png" ||
               format == "both",
           "Invalid format");
    uint32_t batch = uint32_t(num("--batch-size", 2)),
             preview = uint32_t(num("--preview", 0));
    expect(batch > 0 && batch <= 8, "batch-size must be 1..8");
    int limit = num("--limit", 2147483647), stride = num("--stride", 1),
        quality = num("--quality", 95);
    expect(stride > 0 && limit > 0 && preview <= 20000,
           "Invalid selection/preview value");
    std::vector<fs::path> files;
    if (fs::is_regular_file(input))
      files.push_back(input);
    else if (flags.contains("--recursive")) {
      for (auto &x : fs::recursive_directory_iterator(input))
        if (x.is_regular_file() && rawfile(x.path()))
          files.push_back(x.path());
    } else
      for (auto &x : fs::directory_iterator(input))
        if (x.is_regular_file() && rawfile(x.path()))
          files.push_back(x.path());
    std::sort(files.begin(), files.end());
    std::vector<fs::path> selected;
    for (size_t i = 0; i < files.size() && selected.size() < size_t(limit);
         i += stride)
      selected.push_back(files[i]);
    std::ofstream log(output / "batch.log", std::ios::app);
    expect(!selected.empty(), "No supported input images found");
    int success = 0, failed = 0, skipped = 0;
    auto start = std::chrono::steady_clock::now();
    for (size_t offset = 0; offset < selected.size();) {
      std::vector<std::unique_ptr<Host>> decoded;
      std::vector<IBP_HostImage> images;
      std::vector<fs::path> sources, dests;
      while (offset < selected.size() && images.size() < batch) {
        auto source = selected[offset++];
        fs::path relative = fs::is_directory(input)
                                ? fs::relative(source, input)
                                : source.filename();
        auto dest = output / relative;
        dest.replace_extension();
        fs::create_directories(dest.parent_path());
        auto main = dest;
        main += format == "both" ? ".jpg" : "." + format;
        auto recipe = dest;
        recipe += ".imgarg";
        auto analysis = dest;
        analysis += ".analysis.json";
        auto inputAnalysis=dest; inputAnalysis+=".input.json";
        auto extraTiff=dest; extraTiff+=".tif";
        if (fs::exists(main)) {
          if (flags.contains("--resume") && fs::exists(recipe) &&
              fs::exists(analysis) && fs::exists(inputAnalysis) &&
              (format!="both" || fs::exists(extraTiff))) {
            ++skipped;
            continue;
          }
          ++failed;
          log << "EXISTS " << u8(main) << "\n";
          continue;
        }
        try {
          auto h = std::make_unique<Host>();
          ck(IBP_LoadImage(u8(source).c_str(), preview ? 1 : 0, &h->i));
          resize(h->i, preview);
          if (flags.contains("--before")) {
            auto before = dest;
            before += ".before.jpg";
            if (!fs::exists(before))
              ck(IBP_SaveImage(u8(before).c_str(), &h->i, quality));
          }
          images.push_back(h->i);
          decoded.push_back(std::move(h));
          sources.push_back(source);
          dests.push_back(dest);
        } catch (const std::exception &ex) {
          ++failed;
          log << "DECODE_FAIL " << u8(source) << " " << ex.what() << "\n";
          std::cerr << ex.what() << std::endl;
        }
      }
      if (images.empty())
        continue;
      std::vector<IBP_Image> ids(images.size(), 0);
      try {
        ck(UploadImageBatchMode(c, images.data(), uint32_t(images.size()),
                                ids.data()));
        decoded.clear();
        std::vector<IBP_Params> ps(ids.size());
        std::vector<IBP_Analysis> before(ids.size()), after(ids.size());
        for (size_t i = 0; i < ids.size(); i++) {
          ck(IBP_Analyze(c, ids[i], &before[i]));
          if (flags.contains("--fixed"))
            ps[i] = target.parameters;
          else
            ck(IBP_SuggestParams(&before[i], &target, &ps[i]));
          apply(ps[i]);
        }
        if(extendedRecipe) {
          std::vector<IBP_CurveSet> curves(ids.size(),loadedRecipe.curves);
          ck(IBP_ProcessBatchCurves(c,ids.data(),ps.data(),curves.data(),uint32_t(ids.size())));
        } else ck(IBP_ProcessBatch(c, ids.data(), ps.data(), uint32_t(ids.size())));
        for (size_t i = 0; i < ids.size(); i++) {
          try {
            ck(IBP_Analyze(c, ids[i], &after[i]));
            Host result;
            ck(DownloadImage(c, ids[i], &result.i));
            for (auto ext : format == "both"
                                ? std::vector<std::string>{"jpg", "tif"}
                                : std::vector<std::string>{format}) {
              auto out = dests[i];
              out += "." + ext;
              ck(IBP_SaveImage(u8(out).c_str(), &result.i, quality));
            }
            auto recipe = dests[i];
            recipe += ".imgarg";
            IBP_Target record = target;
            record.parameters = ps[i];
            record.reference = after[i];
            if(extendedRecipe){IBP_Recipe full=loadedRecipe;full.target=record;ck(IBP_SaveRecipe(u8(recipe).c_str(),&full));}
            else ck(IBP_SaveTarget(u8(recipe).c_str(), &record));
            auto analysis = dests[i];
            analysis += ".analysis.json";
            ck(IBP_WriteAnalysisJson(u8(analysis).c_str(), &after[i]));
            auto orig = dests[i];
            orig += ".input.json";
            ck(IBP_WriteAnalysisJson(u8(orig).c_str(), &before[i]));
            ++success;
            double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
            std::cout << "[" << success + failed + skipped << "/"
                      << selected.size() << "] " << u8(sources[i].filename())
                      << " EV=" << ps[i].v[0] << " median=" << before[i].median
                      << " -> " << after[i].median
                      << " clip=" << after[i].whiteClip
                      << " elapsed=" << int(seconds) << "s" << std::endl;
            log << "OK " << u8(sources[i]) << " " << result.i.width << "x"
                << result.i.height << " EV=" << ps[i].v[0] << "\n";
            log.flush();
          } catch (const std::exception &ex) {
            ++failed;
            log << "OUTPUT_FAIL " << u8(sources[i]) << " " << ex.what() << "\n";
            std::cerr << ex.what() << std::endl;
          }
        }
      } catch (const std::exception &ex) {
        failed += int(ids.size());
        log << "BATCH_FAIL " << ex.what() << "\n";
        std::cerr << ex.what() << std::endl;
      }
      for (auto id : ids)
        if (id)
          IBP_ReleaseImage(c, id);
    }
    std::cout << "Completed: " << success << ", skipped: " << skipped
              << ", failed: " << failed << std::endl;
    return failed ? 2 : 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
}
