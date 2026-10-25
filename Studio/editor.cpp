#define IMGUI_DEFINE_MATH_OPERATORS
#include "editor.h"
#include "imgui_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
using namespace std::chrono_literals;
namespace {
void ck(int rc) {
  if (rc)
    throw std::runtime_error(IBP_LastError());
}
std::string u8(const fs::path &p) {
  auto s = p.u8string();
  return {s.begin(), s.end()};
}
fs::path path8(const std::string &s) {
  return fs::path(reinterpret_cast<const char8_t *>(s.c_str()));
}
EditState defaults() {
  EditState e;
  IBP_DefaultParams(&e.params);
  IBP_DefaultCurves(&e.curves);
  return e;
}
std::vector<float> resize(const float *src, uint32_t w, uint32_t h, uint32_t &ow, uint32_t &oh,
                          uint32_t edge) {
  float scale = std::min(1.f, float(edge) / std::max(w, h));
  ow = std::max(1u, uint32_t(w * scale));
  oh = std::max(1u, uint32_t(h * scale));
  std::vector<float> result(size_t(ow) * oh * 4);
  for (uint32_t y = 0; y < oh; y++)
    for (uint32_t x = 0; x < ow; x++) {
      uint32_t x0 = uint64_t(x) * w / ow, x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * w / ow)),
               y0 = uint64_t(y) * h / oh, y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * h / oh));
      for (int c = 0; c < 3; c++) {
        double sum = 0;
        for (uint32_t j = y0; j < y1; j++)
          for (uint32_t i = x0; i < x1; i++)
            sum += src[(size_t(j) * w + i) * 4 + c];
        result[(size_t(y) * ow + x) * 4 + c] = float(sum / ((x1 - x0) * (y1 - y0)));
      }
      result[(size_t(y) * ow + x) * 4 + 3] = 1;
    }
  return result;
}
bool imagePath(const fs::path &p) {
  std::string e = u8(p.extension());
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  static std::set<std::string> exts = {".nef", ".nrw", ".cr2",  ".cr3", ".crw", ".arw",
                                       ".dng", ".raf", ".rw2",  ".orf", ".pef", ".raw",
                                       ".srw", ".jpg", ".jpeg", ".png", ".tif", ".tiff"};
  return exts.contains(e);
}
bool same(const EditState &a, const EditState &b) {
  return memcmp(&a.params, &b.params, sizeof(a.params)) == 0 &&
         memcmp(&a.curves, &b.curves, sizeof(a.curves)) == 0;
}
struct DialogRequest {
  Editor *app;
  int type;
};
} // namespace
Editor::Editor(SDL_Window *w, const IBP_VulkanDevice &vk) : window(w), device(vk.device) {
  ck(IBP_CreateSharedVulkan(&vk, nullptr, &core));
  char *pref = SDL_GetPrefPath("ImageBatchProcessor", "Studio");
  stateDir = path8(pref ? pref : ".") / "sessions";
  resettingLayout = !fs::exists(stateDir.parent_path() / "layout.ini");
  SDL_free(pref);
  fs::create_directories(stateDir);
}
Editor::~Editor() {
  if (decode.valid())
    decode.wait();
  cancelExport = true;
  if (exporting.valid())
    exporting.wait();
  vkDeviceWaitIdle(device);
  releasePreview();
  for (auto &p : photos) {
    if (p.thumbSet)
      ImGui_ImplVulkan_RemoveTexture(p.thumbSet);
    if (p.thumbnail)
      IBP_ReleaseImage(core, p.thumbnail);
  }
  IBP_Destroy(core);
}
bool Editor::canClose() {
  if (dialogOpen) {
    status = "请先关闭文件对话框";
    return false;
  }
  if (exporting.valid()) {
    cancelExport = true;
    status = "正在结束当前导出，请稍后关闭";
    return false;
  }
  endGesture();
  return true;
}
void Editor::event(const SDL_Event &e) {
  if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
    add(e.drop.data);
}
void Editor::add(const char *p) {
  try {
    addPath(path8(p));
    if (active < 0 && !photos.empty())
      select(0);
  } catch (const std::exception &e) {
    status = e.what();
  }
}
void Editor::addPath(const fs::path &p) {
  if (fs::is_directory(p)) {
    std::vector<fs::path> entries;
    for (auto &f : fs::directory_iterator(p))
      if (f.is_regular_file() && imagePath(f.path()))
        entries.push_back(f.path());
    std::sort(entries.begin(), entries.end());
    for (auto &f : entries)
      addPath(f);
    return;
  }
  if (!fs::is_regular_file(p) || !imagePath(p))
    return;
  auto absolute = fs::weakly_canonical(p);
  for (auto &photo : photos)
    if (photo.path == absolute)
      return;
  Photo photo;
  photo.path = absolute;
  photo.edit = defaults();
  IBP_Recipe recipe{};
  auto cache = sessionPath(absolute);
  if (fs::exists(cache) && IBP_LoadRecipe(u8(cache).c_str(), &recipe) == 0)
    photo.edit = {recipe.target.parameters, recipe.curves};
  photo.history.push_back({photo.edit, "打开照片"});
  photos.push_back(std::move(photo));
}
fs::path Editor::sessionPath(const fs::path &p) const {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : u8(p)) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  std::ostringstream os;
  os << std::hex << hash;
  return stateDir / (os.str() + ".imgarg");
}
void Editor::persist(Photo &p) {
  try {
    IBP_Recipe r{};
    IBP_DefaultTarget(&r.target);
    r.target.parameters = p.edit.params;
    r.curves = p.edit.curves;
    auto dest = sessionPath(p.path), temp = dest;
    temp += ".tmp";
    ck(IBP_SaveRecipe(u8(temp).c_str(), &r));
    // Cache files are app-owned. Do not overwrite original photos or nearby sidecars.
    std::error_code ec;
    fs::rename(temp, dest, ec);
    if (ec) {
      fs::remove(dest, ec);
      fs::rename(temp, dest);
    }
    p.saveError.clear();
  } catch (const std::exception &e) {
    p.saveError = std::string("调整尚未保存：") + e.what();
    status = p.saveError;
  }
}
void Editor::select(int index) {
  if (index < 0 || index >= int(photos.size()))
    return;
  endGesture();
  active = index;
  curvePoint = -1;
  pan = {};
  zoom = 0;
  status = "正在准备预览…";
  // Upload/release happens in tick(), before any ImGui draw commands refer to textures.
}
void Editor::startDecode(int index) {
  auto path = photos[index].path;
  decode = std::async(std::launch::async, [path, index] {
    DecodeResult r;
    r.index = index;
    IBP_HostImage h{};
    try {
      ck(IBP_LoadImage(u8(path).c_str(), 1, &h));
      r.pixels = resize(h.rgba, h.width, h.height, r.w, r.h, 2048);
    } catch (const std::exception &e) {
      r.error = e.what();
    }
    IBP_FreeHostImage(&h);
    return r;
  });
}
VkDescriptorSet Editor::texture(IBP_Image image) {
  VkImageView view;
  ck(IBP_GetVulkanView(core, image, &view));
  return ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_GENERAL);
}
void Editor::releasePreview() {
  vkDeviceWaitIdle(device);
  if (beforeSet)
    ImGui_ImplVulkan_RemoveTexture(beforeSet);
  if (afterSet)
    ImGui_ImplVulkan_RemoveTexture(afterSet);
  beforeSet = afterSet = {};
  for (auto id : {original, working, originalDisplay, editedDisplay})
    if (id)
      IBP_ReleaseImage(core, id);
  original = working = originalDisplay = editedDisplay = 0;
  loaded = -1;
}
void Editor::loadActive() {
  if (active < 0 || photos[active].cache.empty())
    return;
  releasePreview();
  auto &p = photos[active];
  previewW = p.width;
  previewH = p.height;
  IBP_HostImage host{p.width, p.height, p.cache.data()};
  IBP_HostImage inputs[] = {host, host, host, host};
  IBP_Image ids[4]{};
  ck(UploadImageBatchMode(core, inputs, 4, ids));
  original = ids[0];
  working = ids[1];
  originalDisplay = ids[2];
  editedDisplay = ids[3];
  ck(IBP_EncodeDisplay(core, original, originalDisplay));
  beforeSet = texture(originalDisplay);
  afterSet = texture(editedDisplay);
  ck(IBP_Analyze(core, original, &beforeStats));
  loaded = active;
  dirty = true;
  renderPreview();
  int cached = 0;
  for (auto &photo : photos)
    if (!photo.cache.empty())
      cached++;
  for (int i = 0; cached > 3 && i < int(photos.size()); i++)
    if (i != active && !photos[i].cache.empty()) {
      std::vector<float>().swap(photos[i].cache);
      cached--;
    }
}
void Editor::createThumbnail(int i, const DecodeResult &r) {
  auto &p = photos[i];
  if (p.thumbnail)
    return;
  uint32_t w, h;
  auto pixels = resize(r.pixels.data(), r.w, r.h, w, h, 180);
  IBP_HostImage host{w, h, pixels.data()};
  IBP_Image input{};
  ck(UploadImage(core, &host, &input));
  try {
    ck(UploadImage(core, &host, &p.thumbnail));
    ck(IBP_EncodeDisplay(core, input, p.thumbnail));
    p.thumbSet = texture(p.thumbnail);
    p.thumbW = int(w);
    p.thumbH = int(h);
  } catch (...) {
    IBP_ReleaseImage(core, input);
    throw;
  }
  ck(IBP_ReleaseImage(core, input));
}
void Editor::renderPreview() {
  if (loaded != active || active < 0)
    return;
  auto start = std::chrono::steady_clock::now();
  auto &edit = photos[active].edit;
  ck(IBP_CopyImage(core, original, working));
  ck(IBP_ProcessBatchCurves(core, &working, &edit.params, &edit.curves, 1));
  ck(IBP_EncodeDisplay(core, working, editedDisplay));
  if (!gesture)
    ck(IBP_Analyze(core, working, &afterStats));
  processingMs = float(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  dirty = false;
  lastRender = SDL_GetTicks() / 1000.;
  status = photos[active].saveError.empty()
               ? u8(photos[active].path.filename()) + " · 调整已自动保存"
               : photos[active].saveError;
}
void Editor::tick() {
  try {
    std::vector<std::pair<int, std::string>> results;
    {
      std::lock_guard lock(dialogMutex);
      results.swap(dialogResults);
    }
    for (auto &[type, path] : results) {
      dialogOpen = false;
      if (path.empty())
        continue;
      if (type <= 1)
        add(path.c_str());
      else if (type == 2) {
        snprintf(exportFolder, sizeof(exportFolder), "%s", path.c_str());
      } else if (type == 3 && active >= 0) {
        IBP_Recipe r;
        ck(IBP_LoadRecipe(path.c_str(), &r));
        photos[active].edit = {r.target.parameters, r.curves};
        commit("载入配方");
      } else if (type == 4 && active >= 0) {
        IBP_Recipe r{};
        IBP_DefaultTarget(&r.target);
        r.target.parameters = photos[active].edit.params;
        r.curves = photos[active].edit.curves;
        r.target.reference = afterStats;
        ck(IBP_SaveRecipe(path.c_str(), &r));
        status = "配方已保存";
      }
    }
    if (decode.valid() && decode.wait_for(0s) == std::future_status::ready) {
      auto result = decode.get();
      auto &p = photos[result.index];
      if (!result.error.empty()) {
        p.failed = true;
        p.error = result.error;
        if (result.index == active)
          status = result.error;
      } else {
        createThumbnail(result.index, result);
        p.width = result.w;
        p.height = result.h;
        p.cache = std::move(result.pixels);
        if (result.index == active)
          loadActive();
        else {
          int cached = 0;
          for (auto &x : photos)
            if (!x.cache.empty())
              cached++;
          if (cached > 3)
            std::vector<float>().swap(p.cache);
        }
      }
    }
    if (!decode.valid() && active >= 0 && loaded != active && !photos[active].failed) {
      if (!photos[active].cache.empty())
        loadActive();
      else
        startDecode(active);
    }
    if (dirty && SDL_GetTicks() / 1000. - lastRender > (gesture ? .04 : .005))
      renderPreview();
    if (exporting.valid() && exporting.wait_for(0s) == std::future_status::ready) {
      auto result = exporting.get();
      status = result.message;
    }
  } catch (const std::exception &e) {
    status = e.what();
    dirty = false;
  }
}
void Editor::beginGesture(const char *label) {
  if (!gesture && active >= 0) {
    gesture = true;
    gestureStart = photos[active].edit;
    currentEditLabel = label;
  }
}
void Editor::endGesture() {
  if (gesture) {
    gesture = false;
    if (active >= 0 && !same(gestureStart, photos[active].edit))
      commit(currentEditLabel);
    dirty = true;
  }
}
void Editor::commit(const std::string &label) {
  if (active < 0)
    return;
  auto &p = photos[active];
  if (same(p.history[p.cursor].state, p.edit))
    return;
  p.history.resize(p.cursor + 1);
  p.history.push_back({p.edit, label});
  if (p.history.size() > 80)
    p.history.erase(p.history.begin());
  p.cursor = int(p.history.size()) - 1;
  dirty = true;
  persist(p);
}
void Editor::undo(int direction) {
  if (active < 0)
    return;
  endGesture();
  auto &p = photos[active];
  p.cursor = std::clamp(p.cursor + direction, 0, int(p.history.size()) - 1);
  p.edit = p.history[p.cursor].state;
  dirty = true;
  persist(p);
}
void SDLCALL Editor::dialogCallback(void *userdata, const char *const *paths, int) {
  std::unique_ptr<DialogRequest> req((DialogRequest *)userdata);
  auto *app = req->app;
  std::lock_guard lock(app->dialogMutex);
  if (!paths || !paths[0])
    app->dialogResults.emplace_back(req->type, "");
  else
    for (int i = 0; paths[i]; i++)
      app->dialogResults.emplace_back(req->type, paths[i]);
}
void Editor::requestDialog(int type) {
  if (dialogOpen)
    return;
  dialogOpen = true;
  auto req = new DialogRequest{this, type};
  static SDL_DialogFileFilter imageFilters[] = {
      {"RAW & images", "nef;nrw;cr2;cr3;arw;dng;raf;rw2;orf;pef;raw;jpg;jpeg;png;tif;tiff"},
      {"All files", "*"}};
  static SDL_DialogFileFilter recipeFilter[] = {{"Image recipe", "imgarg"}};
  if (type == 0)
    SDL_ShowOpenFileDialog(dialogCallback, req, window, imageFilters, 2, nullptr, true);
  else if (type == 1 || type == 2)
    SDL_ShowOpenFolderDialog(dialogCallback, req, window, nullptr, false);
  else if (type == 3)
    SDL_ShowOpenFileDialog(dialogCallback, req, window, recipeFilter, 1, nullptr, false);
  else
    SDL_ShowSaveFileDialog(dialogCallback, req, window, recipeFilter, 1, "recipe.imgarg");
}
void Editor::openExport() {
  if (active < 0)
    return;
  exportPopup = true;
  if (!exportFolder[0]) {
    auto folder = u8(photos[active].path.parent_path() / "Studio_Out");
    snprintf(exportFolder, sizeof(exportFolder), "%s", folder.c_str());
  }
}
void Editor::startExport() {
  if (exporting.valid() || !exportFolder[0])
    return;
  endGesture();
  std::vector<std::pair<fs::path, EditState>> selected;
  for (auto &p : photos)
    if (p.selected)
      selected.emplace_back(p.path, p.edit);
  if (selected.empty() && active >= 0)
    selected.emplace_back(photos[active].path, photos[active].edit);
  if (selected.empty())
    return;
  auto destination = path8(exportFolder);
  fs::create_directories(destination);
  int format = exportFormat, quality = exportQuality;
  exportDone = 0;
  cancelExport = false;
  exportTotal = int(selected.size());
  exporting = std::async(std::launch::async, [this, selected = std::move(selected), destination,
                                              format, quality] {
    ExportResult result;
    IBP_Context ctx{};
    try {
      ck(IBP_Create(nullptr, &ctx));
      for (auto &[source, edit] : selected) {
        if (cancelExport)
          break;
        IBP_HostImage input{}, output{};
        IBP_Image id{};
        try {
          auto dest = destination / source.filename();
          dest.replace_extension(format == 1 ? ".tif" : format == 2 ? ".png" : ".jpg");
          ck(IBP_LoadImage(u8(source).c_str(), 0, &input));
          ck(UploadImage(ctx, &input, &id));
          IBP_FreeHostImage(&input);
          ck(IBP_ProcessBatchCurves(ctx, &id, &edit.params, &edit.curves, 1));
          ck(DownloadImage(ctx, id, &output));
          ck(IBP_SaveImage(u8(dest).c_str(), &output, quality));
          IBP_Recipe r{};
          IBP_DefaultTarget(&r.target);
          r.target.parameters = edit.params;
          r.curves = edit.curves;
          ck(IBP_Analyze(ctx, id, &r.target.reference));
          auto sidecar = dest;
          sidecar.replace_extension(".imgarg");
          ck(IBP_SaveRecipe(u8(sidecar).c_str(), &r));
          result.done++;
        } catch (const std::exception &e) {
          result.failed++;
          result.message = e.what();
        }
        IBP_FreeHostImage(&input);
        IBP_FreeHostImage(&output);
        if (id)
          IBP_ReleaseImage(ctx, id);
        exportDone++;
      }
    } catch (const std::exception &e) {
      result.failed++;
      result.message = e.what();
    }
    if (ctx)
      IBP_Destroy(ctx);
    result.message = std::string(cancelExport ? "已取消 · " : "导出完成 · ") +
                     std::to_string(result.done) + " 张成功 / " + std::to_string(result.failed) +
                     " 张失败" + (result.failed ? " · " + result.message : "");
    return result;
  });
  exportPopup = false;
}

bool Editor::slider(const char *label, int index, float low, float high, float reset,
                    const char *format) {
  auto &value = photos[active].edit.params.v[index];
  ImGui::PushID(index);
  float unit = ImGui::GetFontSize() / 18.f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * unit, 2 * unit));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9 * unit, 5 * unit));
  float numberWidth = std::max(100 * unit, ImGui::CalcTextSize("+5.00 EV").x + 20 * unit);
  float rowWidth = ImGui::GetContentRegionAvail().x;
  float rowStart = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(rowStart + std::max(90 * unit, rowWidth - numberWidth));
  float typed = value;
  ImGui::SetNextItemWidth(numberWidth);
  bool changed =
      ImGui::DragFloat("##number", &typed, .01f, low, high, format, ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemActivated())
    beginGesture(label);
  if (changed) {
    value = typed;
    dirty = true;
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    endGesture();
  float dragged = value;
  ImGui::SetNextItemWidth(-1);
  auto pos = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x;

  if (index == 9) {
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        pos, pos + ImVec2(width, ImGui::GetFrameHeight()), IM_COL32(40, 75, 110, 255),
        IM_COL32(125, 91, 43, 255), IM_COL32(125, 91, 43, 255), IM_COL32(40, 75, 110, 255));
  }
  if (index == 10) {
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        pos, pos + ImVec2(width, ImGui::GetFrameHeight()), IM_COL32(43, 97, 72, 255),
        IM_COL32(110, 55, 100, 255), IM_COL32(110, 55, 100, 255), IM_COL32(43, 97, 72, 255));
  }
  if (index == 9 || index == 10)
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, .15f));
  bool moved = ImGui::SliderFloat("##slider", &dragged, low, high, "", ImGuiSliderFlags_NoInput);
  if (index == 9 || index == 10)
    ImGui::PopStyleColor();
  if (ImGui::IsItemActivated())
    beginGesture(label);
  if (moved) {
    value = dragged;
    dirty = true;
    changed = true;
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    beginGesture(label);
    value = reset;
    dirty = true;
    endGesture();
  }
  if (ImGui::IsItemDeactivatedAfterEdit())
    endGesture();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("双击重置 · 右侧数值可拖动 / Ctrl+单击输入");
  ImGui::PopStyleVar(2);
  ImGui::PopID();
  return changed;
}
void Editor::resetGroup(const char *label, const std::vector<int> &ids) {
  if (ImGui::SmallButton(label)) {
    auto d = defaults();
    for (int i : ids)
      photos[active].edit.params.v[i] = d.params.v[i];
    commit(label);
  }
}
void Editor::histogram(ImVec2 size, bool background) {
  auto pos = ImGui::GetCursorScreenPos();
  auto dl = ImGui::GetWindowDrawList();
  if (!background) {
    ImGui::InvisibleButton("##histogram", size);
    dl->AddRectFilled(pos, pos + size, IM_COL32(22, 25, 29, 255), 4);
  }
  if (loaded != active)
    return;
  auto &stats = showOriginal ? beforeStats : afterStats;
  ImU32 colors[] = {IM_COL32(210, 215, 220, 100), IM_COL32(235, 97, 108, 140),
                    IM_COL32(114, 210, 154, 140), IM_COL32(105, 158, 240, 140)};
  for (int c = background ? 0 : 1; c < (background ? 1 : 4); c++) {
    uint32_t maximum = 1;
    for (int i = 1; i < 255; i++)
      maximum = std::max(maximum, stats.histogram[c][i]);
    for (int i = 0; i < 255; i++) {
      float y = std::min(1.f, float(stats.histogram[c][i]) / maximum);
      dl->AddLine(pos + ImVec2(i * size.x / 255, size.y),
                  pos + ImVec2(i * size.x / 255, size.y - y * (size.y - 5)), colors[c],
                  std::max(1.f, size.x / 255));
    }
  }
}
void Editor::drawCurve() {
  auto &c = photos[active].edit.curves;
  const char *channels[] = {"亮度", "红", "绿", "蓝"};
  for (int k = 0; k < 4; k++) {
    if (k)
      ImGui::SameLine();
    if (ImGui::Selectable(channels[k], curveChannel == k, 0, ImVec2(56, 25))) {
      curveChannel = k;
      curvePoint = -1;
    }
  }
  int channel = curveChannel;
  auto pos = ImGui::GetCursorScreenPos();
  float side = ImGui::GetContentRegionAvail().x;
  ImVec2 size(side, side);
  auto dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(pos, pos + size, IM_COL32(19, 22, 26, 255), 5);
  histogram(size, true);
  ImGui::SetCursorScreenPos(pos);
  ImGui::InvisibleButton("##curve", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hovered = ImGui::IsItemHovered();
  auto mouse = ImGui::GetIO().MousePos;
  for (int i = 0; i <= 4; i++) {
    float d = side * i / 4;
    dl->AddLine(pos + ImVec2(d, 0), pos + ImVec2(d, side), IM_COL32(62, 66, 72, 170));
    dl->AddLine(pos + ImVec2(0, d), pos + ImVec2(side, d), IM_COL32(62, 66, 72, 170));
  }
  dl->AddLine(pos + ImVec2(0, side), pos + ImVec2(side, 0), IM_COL32(100, 108, 116, 120));
  ImU32 col = channel == 1   ? IM_COL32(246, 127, 132, 255)
              : channel == 2 ? IM_COL32(120, 220, 162, 255)
              : channel == 3 ? IM_COL32(115, 173, 249, 255)
                             : IM_COL32(227, 235, 239, 255);
  auto screen = [&](float x, float y) { return pos + ImVec2(x * side, (1 - y) * side); };
  for (int i = 0; i < 255; i++) {
    int a = i * 1023 / 255, b = (i + 1) * 1023 / 255;
    dl->AddLine(screen(a / 1023.f, c.lut[channel][a]), screen(b / 1023.f, c.lut[channel][b]), col,
                2);
  }
  int nearest = -1;
  float distance = 12;
  for (uint32_t j = 0; j < c.count[channel]; j++) {
    auto point = screen(c.points[channel][j][0], c.points[channel][j][1]);
    float d = std::hypot(point.x - mouse.x, point.y - mouse.y);
    if (d < distance) {
      distance = d;
      nearest = int(j);
    }
    dl->AddCircleFilled(point, int(j) == curvePoint ? 6.f : 4.f,
                        int(j) == curvePoint ? IM_COL32(94, 211, 219, 255) : col);
  }
  if (hovered && ImGui::IsMouseClicked(0)) {
    beginGesture(
        (std::string("曲线 · ") + channels[channel]).c_str()); // label is copied immediately
    curvePoint = nearest;
    if (curvePoint < 0 && c.count[channel] < 32) {
      float x = std::clamp((mouse.x - pos.x) / side, .001f, .999f),
            y = std::clamp(1 - (mouse.y - pos.y) / side, 0.f, 1.f);
      int j = 1;
      while (j < int(c.count[channel]) && c.points[channel][j][0] < x)
        j++;
      if (x - c.points[channel][j - 1][0] > .0001f && c.points[channel][j][0] - x > .0001f) {
        for (int k = int(c.count[channel]); k > j; k--) {
          c.points[channel][k][0] = c.points[channel][k - 1][0];
          c.points[channel][k][1] = c.points[channel][k - 1][1];
        }
        c.points[channel][j][0] = x;
        c.points[channel][j][1] = y;
        c.count[channel]++;
        curvePoint = j;
      }
    }
    draggingCurve = curvePoint >= 0;
  }
  if (draggingCurve && ImGui::IsMouseDown(0)) {
    int j = curvePoint;
    float x = std::clamp((mouse.x - pos.x) / side, 0.f, 1.f),
          y = std::clamp(1 - (mouse.y - pos.y) / side, 0.f, 1.f);
    if (j == 0)
      x = 0;
    else if (j == int(c.count[channel]) - 1)
      x = 1;
    else
      x = std::clamp(x, c.points[channel][j - 1][0] + .0001f, c.points[channel][j + 1][0] - .0001f);
    c.points[channel][j][0] = x;
    c.points[channel][j][1] = y;
    c.interpolation[channel] = 1;
    ck(IBP_BuildCurves(&c));
    dirty = true;
  }
  if (draggingCurve && ImGui::IsMouseReleased(0)) {
    draggingCurve = false;
    endGesture();
  }
  if (hovered && (ImGui::IsMouseClicked(1) || ImGui::IsKeyPressed(ImGuiKey_Delete))) {
    int j = nearest >= 0 ? nearest : curvePoint;
    if (j > 0 && j < int(c.count[channel]) - 1) {
      for (int k = j; k < int(c.count[channel]) - 1; k++)
        memcpy(c.points[channel][k], c.points[channel][k + 1], 2 * sizeof(float));
      c.count[channel]--;
      curvePoint = -1;
      ck(IBP_BuildCurves(&c));
      commit("删除曲线点");
    }
  }
  if (curvePoint >= 0 && curvePoint < int(c.count[channel]))
    ImGui::Text("输入 %.0f   输出 %.0f", c.points[channel][curvePoint][0] * 255,
                c.points[channel][curvePoint][1] * 255);
  else
    ImGui::TextDisabled("单击加点 · 拖动调整 · 右键删除");
  if (ImGui::Button("线性")) {
    c.count[channel] = 2;
    c.interpolation[channel] = 1;
    c.points[channel][0][0] = c.points[channel][0][1] = 0;
    c.points[channel][1][0] = c.points[channel][1][1] = 1;
    ck(IBP_BuildCurves(&c));
    curvePoint = -1;
    commit("重置曲线");
  }
  ImGui::SameLine();
  if (ImGui::Button("柔和 S")) {
    c.count[channel] = 4;
    c.interpolation[channel] = 1;
    float p[4][2] = {{0, 0}, {.25f, .19f}, {.75f, .81f}, {1, 1}};
    memcpy(c.points[channel], p, sizeof(p));
    ck(IBP_BuildCurves(&c));
    curvePoint = -1;
    commit("柔和 S 曲线");
  }
}
void Editor::drawAdjustments() {
  ImGui::Begin("调整");
  if (active < 0) {
    ImGui::TextDisabled("打开照片后开始调整");
    ImGui::End();
    return;
  }
  if (loaded == active) {
    histogram({ImGui::GetContentRegionAvail().x, 92}, false);
    ImGui::TextDisabled("中位 %.2f   高光 %.1f%%", afterStats.median, afterStats.whiteClip * 100);
  }
  ImGui::BeginDisabled(loaded != active);
  if (ImGui::Button("自动调整", ImVec2(ImGui::GetContentRegionAvail().x * .48f, 0))) {
    IBP_Target target;
    IBP_DefaultTarget(&target);
    ck(IBP_SuggestParams(&beforeStats, &target, &photos[active].edit.params));
    commit("自动调整");
  }
  ImGui::SameLine();
  if (ImGui::Button("全部重置", ImVec2(-1, 0))) {
    photos[active].edit = defaults();
    commit("恢复原图");
  }
  if (ImGui::CollapsingHeader("光线", ImGuiTreeNodeFlags_DefaultOpen)) {
    slider("曝光", 0, -5, 5, 0, "%+.2f EV");
    slider("对比度", 1, .1f, 2.5f, 1);
    slider("高光", 3, -2, 2, 0, "%+.2f");
    slider("阴影", 2, -2, 2, 0, "%+.2f");
    slider("白点", 5, .1f, 3, 1);
    slider("黑点", 4, -.1f, .3f, 0, "%.3f");
    slider("高光压缩", 28, 0, 1);
    resetGroup("重置光线", {0, 1, 2, 3, 4, 5, 28});
  }
  if (ImGui::CollapsingHeader("颜色", ImGuiTreeNodeFlags_DefaultOpen)) {
    slider("冷暖", 9, -2, 2);
    slider("色调", 10, -2, 2);
    slider("自然饱和度", 7, -1, 1);
    slider("饱和度", 6, 0, 2, 1);
    resetGroup("重置颜色", {6, 7, 8, 9, 10});
  }
  if (ImGui::CollapsingHeader("曲线", ImGuiTreeNodeFlags_DefaultOpen))
    drawCurve();
  if (ImGui::CollapsingHeader("颜色混合器")) {
    static int sector = 0, axis = 0;
    const char *hues[] = {"红", "橙", "黄绿", "绿", "青", "蓝", "紫", "洋红"};
    ImGui::Combo("色彩", &sector, hues, 8);
    const char *axes[] = {"色相", "饱和度", "明度"};
    ImGui::Combo("调整", &axis, axes, 3);
    slider(axes[axis], 64 + sector * 3 + axis, axis == 0 ? -180.f : -.5f, axis == 0 ? 180.f : .5f);
  }
  if (ImGui::CollapsingHeader("细节")) {
    slider("锐化", 12, 0, 2);
    slider("降噪", 13, 0, 1);
    slider("清晰度", 14, -1, 1);
    ImGui::TextWrapped("预览最长边 2048 像素。细节参数需在全尺寸导出中复核。");
  }
  if (ImGui::CollapsingHeader("几何")) {
    slider("旋转", 19, -45, 45, 0, "%.1f");
    slider("Panini", 17, 0, 3);
    slider("水平视角", 18, 10, 160, 90, "%.0f");
    slider("透视 X", 22, -.8f, .8f);
    slider("透视 Y", 23, -.8f, .8f);
    slider("中心缩放", 24, .25f, 4, 1);
  }
  if (ImGui::CollapsingHeader("效果")) {
    slider("去雾", 15, -1, 1);
    slider("暗角", 16, -2, 2);
    slider("颗粒", 26, 0, 1);
    slider("黑白", 27, 0, 1);
  }
  ImGui::EndDisabled();
  ImGui::End();
}
void Editor::drawCanvas() {
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.065f, .074f, .085f, 1));
  ImGui::Begin("照片");
  if (ImGui::Button("适合")) {
    zoom = 0;
    pan = {};
  }
  ImGui::SameLine();
  if (ImGui::Button("预览 100%")) {
    zoom = 1;
    pan = {};
  }
  ImGui::SameLine();
  ImGui::Checkbox("前后分割", &showCompare);
  ImGui::SameLine();
  ImGui::Checkbox("原图", &showOriginal);
  ImGui::SameLine();
  ImGui::TextDisabled("滚轮缩放 · 空格拖动 · 按住 \\ 看原图");
  auto pos = ImGui::GetCursorScreenPos(), area = ImGui::GetContentRegionAvail();
  area.x = std::max(1.f, area.x);
  area.y = std::max(1.f, area.y);
  ImGui::InvisibleButton("##canvas", area,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  auto dl = ImGui::GetWindowDrawList();
  bool hovered = ImGui::IsItemHovered();
  auto &io = ImGui::GetIO();
  if (loaded == active && beforeSet && afterSet) {
    float fit = std::min((area.x - 32) / previewW, (area.y - 32) / previewH);
    float scale = zoom ? zoom : fit;
    if (hovered && io.MouseWheel != 0) {
      float next = std::clamp(scale * std::pow(1.15f, io.MouseWheel), .05f, 8.f);
      ImVec2 anchor = io.MousePos - (pos + area * .5f);
      pan = anchor - (anchor - pan) * (next / scale);
      zoom = next;
      scale = next;
    }
    if (hovered && (ImGui::IsMouseDragging(2) ||
                    (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(0))))
      pan += io.MouseDelta;
    ImVec2 imageSize(previewW * scale, previewH * scale),
        min = pos + (area - imageSize) * .5f + pan, max = min + imageSize;
    dl->PushClipRect(pos, pos + area, true);
    bool originalView = showOriginal || (!io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Backslash));
    dl->AddImage((ImTextureID)(uintptr_t)(originalView ? beforeSet : afterSet), min, max);
    if (showCompare && !originalView) {
      float x = min.x + split * imageSize.x;
      dl->PushClipRect(min, ImVec2(x, max.y), true);
      dl->AddImage((ImTextureID)(uintptr_t)beforeSet, min, max);
      dl->PopClipRect();
      dl->AddLine({x, std::max(pos.y, min.y)}, {x, std::min(pos.y + area.y, max.y)},
                  IM_COL32(235, 240, 242, 220), 2);
      dl->AddCircleFilled({x, pos.y + area.y * .5f}, 12, IM_COL32(230, 235, 239, 245));
      dl->AddText(min + ImVec2(12, 12), IM_COL32_WHITE, "原图");
      dl->AddText({x + 12, min.y + 12}, IM_COL32_WHITE, "调整后");
      if (hovered && ImGui::IsMouseDown(0) && !ImGui::IsKeyDown(ImGuiKey_Space))
        split = std::clamp((io.MousePos.x - min.x) / imageSize.x, 0.f, 1.f);
    }
    dl->PopClipRect();
    auto text = std::to_string(previewW) + " × " + std::to_string(previewH) + "  预览 · " +
                std::to_string(int(scale * 100)) + "%";
    dl->AddText(pos + ImVec2(12, area.y - 25), IM_COL32(155, 170, 182, 255), text.c_str());
  } else {
    const char *text = active < 0 ? "把照片拖到这里，或点击「打开照片」" : "正在解码照片…";
    if (active >= 0 && photos[active].failed)
      text = photos[active].error.c_str();
    auto t = ImGui::CalcTextSize(text);
    dl->AddText(pos + (area - t) * .5f, IM_COL32(161, 175, 186, 255), text);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}
void Editor::drawFilmstrip() {
  ImGui::Begin("胶片栏", nullptr, ImGuiWindowFlags_HorizontalScrollbar);
  if (ImGui::SmallButton("全选"))
    for (auto &p : photos)
      p.selected = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("清除选择"))
    for (auto &p : photos)
      p.selected = false;
  ImGui::SameLine();
  ImGui::TextDisabled("%d 张 · Ctrl+单击多选", int(photos.size()));
  float unit = ImGui::GetFontSize() / 18.f;
  for (int i = 0; i < int(photos.size()); i++) {
    auto &p = photos[i];
    if (i)
      ImGui::SameLine(0, 10);
    ImGui::PushID(i);
    auto pos = ImGui::GetCursorScreenPos();
    ImVec2 size(148 * unit, 112 * unit);
    if (ImGui::InvisibleButton("photo", size)) {
      if (ImGui::GetIO().KeyCtrl)
        p.selected = !p.selected;
      else {
        for (auto &x : photos)
          x.selected = false;
        p.selected = true;
      }
      select(i);
    }
    if (ImGui::IsItemVisible()) {
      auto dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(pos, pos + size,
                        i == active ? IM_COL32(43, 65, 72, 255) : IM_COL32(29, 33, 39, 255), 5);
      if (p.thumbSet) {
        float scale = std::min(138 * unit / p.thumbW, 76 * unit / p.thumbH);
        ImVec2 s(p.thumbW * scale, p.thumbH * scale);
        ImVec2 min = pos + ImVec2((size.x - s.x) * .5f, 5 * unit + (76 * unit - s.y) * .5f);
        dl->AddImage((ImTextureID)(uintptr_t)p.thumbSet, min, min + s);
      } else {
        dl->AddText(pos + ImVec2(14, 30), IM_COL32(125, 139, 151, 255),
                    p.failed ? "读取失败" : "准备缩略图");
        if (!decode.valid() && loaded == active && !p.failed)
          startDecode(i);
      }
      if (i == active || p.selected)
        dl->AddRect(pos, pos + size, IM_COL32(102, 196, 207, 255), 5, 0, i == active ? 2.f : 1.f);
      auto name = u8(p.path.filename());
      dl->PushClipRect(pos, pos + size, true);
      dl->AddText(pos + ImVec2(7 * unit, 85 * unit), IM_COL32(219, 225, 230, 255), name.c_str());
      dl->PopClipRect();
      if (p.cursor > 0)
        dl->AddCircleFilled(pos + ImVec2(size.x - 10 * unit, 12 * unit), 3 * unit,
                            IM_COL32(118, 219, 172, 255));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", u8(p.path).c_str());
    ImGui::PopID();
  }
  ImGui::End();
}
void Editor::drawHistory() {
  if (!showHistory)
    return;
  ImGui::Begin("历史与预设");
  if (active >= 0) {
    auto &p = photos[active];
    ImGui::TextDisabled("预设");
    if (ImGui::Button("自然", ImVec2(-1, 0)) && loaded == active) {
      IBP_Target t;
      IBP_DefaultTarget(&t);
      ck(IBP_SuggestParams(&beforeStats, &t, &p.edit.params));
      IBP_DefaultCurves(&p.edit.curves);
      commit("自然预设");
    }
    if (ImGui::Button("柔和黑白", ImVec2(-1, 0))) {
      p.edit.params.v[27] = 1;
      p.edit.params.v[1] = .95f;
      p.edit.params.v[2] = .15f;
      commit("柔和黑白");
    }
    ImGui::Separator();
    ImGui::TextDisabled("编辑历史 · 自动保存");
    for (int i = int(p.history.size()) - 1; i >= 0; i--) {
      ImGui::PushID(i);
      if (ImGui::Selectable(p.history[i].label.c_str(), p.cursor == i)) {
        p.cursor = i;
        p.edit = p.history[i].state;
        dirty = true;
        persist(p);
      }
      ImGui::PopID();
    }
  }
  ImGui::End();
}
void Editor::drawToolbar() {
  auto vp = ImGui::GetMainViewport();
  float h = ImGui::GetFrameHeight() + 18;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize({vp->WorkSize.x, h});
  ImGui::Begin("##toolbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextColored({.55f, .83f, .87f, 1}, "IBP");
  ImGui::SameLine();
  ImGui::TextUnformatted("STUDIO");
  ImGui::SameLine(0, 25);
  if (ImGui::Button("打开照片"))
    requestDialog(0);
  ImGui::SameLine();
  if (ImGui::Button("打开目录"))
    requestDialog(1);
  ImGui::SameLine();
  ImGui::BeginDisabled(active < 0);
  if (ImGui::Button("撤销"))
    undo(-1);
  ImGui::SameLine();
  if (ImGui::Button("重做"))
    undo(1);
  ImGui::SameLine();
  if (ImGui::Button("配方"))
    ImGui::OpenPopup("recipeMenu");
  if (ImGui::BeginPopup("recipeMenu")) {
    if (ImGui::MenuItem("载入配方…"))
      requestDialog(3);
    if (ImGui::MenuItem("另存配方…"))
      requestDialog(4);
    if (ImGui::MenuItem("复制调整")) {
      clipboard = photos[active].edit;
      hasClipboard = true;
    }
    ImGui::BeginDisabled(!hasClipboard);
    if (ImGui::MenuItem("粘贴调整")) {
      photos[active].edit = clipboard;
      commit("粘贴调整");
    }
    ImGui::EndDisabled();
    if (ImGui::MenuItem("将当前调整同步到选中照片")) {
      auto edit = photos[active].edit;
      for (int i = 0; i < int(photos.size()); i++)
        if (i != active && photos[i].selected) {
          auto &p = photos[i];
          p.edit = edit;
          p.history.resize(p.cursor + 1);
          p.history.push_back({edit, "同步调整"});
          if (p.history.size() > 80)
            p.history.erase(p.history.begin());
          p.cursor = int(p.history.size()) - 1;
          persist(p);
        }
      status = "已同步选中照片";
    }
    ImGui::EndPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("工作区"))
    ImGui::OpenPopup("workspace");
  if (ImGui::BeginPopup("workspace")) {
    ImGui::MenuItem("历史与预设", nullptr, &showHistory);
    if (ImGui::MenuItem("重置布局"))
      resettingLayout = true;
    ImGui::EndPopup();
  }
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 15, vp->WorkSize.x - 135));
  ImGui::BeginDisabled(active < 0 || exporting.valid());
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.22f, .46f, .51f, 1));
  if (ImGui::Button("导出…", ImVec2(110, 0)))
    openExport();
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::End();
}
void Editor::draw() {
  try {
    auto &io = ImGui::GetIO();
    if (!io.WantTextInput && !draggingCurve) {
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
        undo(-1);
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
        undo(1);
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
        requestDialog(0);
    }
    drawToolbar();
    auto vp = ImGui::GetMainViewport();
    float top = ImGui::GetFrameHeight() + 18, footer = ImGui::GetFrameHeight() + 4;
    ImGui::SetNextWindowPos(vp->WorkPos + ImVec2(0, top));
    ImGui::SetNextWindowSize({vp->WorkSize.x, vp->WorkSize.y - top - footer});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##dockroot", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto dock = ImGui::GetID("StudioDock");
    if (resettingLayout) {
      resettingLayout = false;
      ImGui::DockBuilderRemoveNode(dock);
      ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dock, ImGui::GetWindowSize());
      auto center = dock;
      float filmFraction = std::clamp(
          ImGui::GetTextLineHeightWithSpacing() * 7 / ImGui::GetWindowHeight(), .19f, .29f);
      auto bottom =
          ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, filmFraction, nullptr, &center);
      auto right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, .28f, nullptr, &center);
      auto left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, .17f, nullptr, &center);
      ImGui::DockBuilderDockWindow("照片", center);
      ImGui::DockBuilderDockWindow("调整", right);
      ImGui::DockBuilderDockWindow("胶片栏", bottom);
      ImGui::DockBuilderDockWindow("历史与预设", left);
      ImGui::DockBuilderFinish(dock);
    }
    ImGui::DockSpace(dock, {0, 0});
    ImGui::End();
    ImGui::PopStyleVar();
    drawCanvas();
    drawAdjustments();
    drawFilmstrip();
    drawHistory();
    if (gesture && !draggingCurve && !ImGui::IsAnyItemActive())
      endGesture();
    ImGui::SetNextWindowPos(vp->WorkPos + ImVec2(0, vp->WorkSize.y - footer));
    ImGui::SetNextWindowSize({vp->WorkSize.x, footer});
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings);
    if (exporting.valid()) {
      ImGui::Text("正在导出 %d / %d", int(exportDone), exportTotal);
      ImGui::SameLine();
      if (ImGui::SmallButton("取消导出"))
        cancelExport = true;
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(status.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  GPU %.0f ms", processingMs);
    ImGui::End();
    if (exportPopup && !ImGui::IsPopupOpen("导出照片"))
      ImGui::OpenPopup("导出照片");
    if (ImGui::BeginPopupModal("导出照片", &exportPopup, ImGuiWindowFlags_AlwaysAutoResize)) {
      int count = 0;
      for (auto &p : photos)
        count += p.selected;
      ImGui::Text("导出 %d 张 · 原始尺寸", std::max(count, 1));
      ImGui::SetNextItemWidth(460);
      ImGui::InputText("目录", exportFolder, sizeof(exportFolder));
      ImGui::SameLine();
      if (ImGui::Button("浏览…"))
        requestDialog(2);
      const char *formats[] = {"JPEG · sRGB", "TIFF · 16 位", "PNG · 16 位"};
      ImGui::Combo("格式", &exportFormat, formats, 3);
      if (exportFormat == 0)
        ImGui::SliderInt("JPEG 质量", &exportQuality, 1, 100);
      ImGui::TextDisabled("使用每张照片当前参数；现有图片不覆盖。附带 v2 配方。");
      if (ImGui::Button("开始导出")) {
        try {
          startExport();
          ImGui::CloseCurrentPopup();
        } catch (const std::exception &e) {
          status = e.what();
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("取消")) {
        exportPopup = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  } catch (const std::exception &e) {
    status = e.what();
  }
}
