#pragma once
#include "imgbp_editor.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace fs = std::filesystem;
struct EditState {
  IBP_Params params{};
  IBP_CurveSet curves{};
};
struct History {
  EditState state;
  std::string label;
};
struct Photo {
  fs::path path;
  EditState edit{};
  std::vector<History> history;
  int cursor = 0;
  IBP_Image thumbnail = 0;
  VkDescriptorSet thumbSet{};
  int thumbW = 0, thumbH = 0;
  std::vector<float> cache;
  uint32_t width = 0, height = 0;
  bool selected = false, failed = false;
  std::string error, saveError;
};
struct DecodeResult {
  int index = -1;
  uint32_t w = 0, h = 0;
  std::vector<float> pixels;
  std::string error;
};
struct ExportResult {
  int done = 0, failed = 0;
  std::string message;
};
class Editor {
  SDL_Window *window;
  VkDevice device;
  IBP_Context core{};
  fs::path stateDir;
  std::vector<Photo> photos;
  int active = -1, loaded = -1;
  std::future<DecodeResult> decode;
  std::future<ExportResult> exporting;
  std::atomic<int> exportDone{0};
  std::atomic<bool> cancelExport{false};
  int exportTotal = 0;
  std::mutex dialogMutex;
  std::vector<std::pair<int, std::string>> dialogResults;
  bool dialogOpen = false;
  IBP_Image original = 0, working = 0, originalDisplay = 0, editedDisplay = 0;
  VkDescriptorSet beforeSet{}, afterSet{};
  uint32_t previewW = 0, previewH = 0;
  IBP_Analysis beforeStats{}, afterStats{};
  bool dirty = false, showHistory = true, showCompare = false, showOriginal = false;
  bool draggingCurve = false;
  int curveChannel = 0, curvePoint = -1;
  double lastRender = 0, lastEdit = 0;
  float processingMs = 0;
  float zoom = 0, split = .5f;
  ImVec2 pan{};
  std::string status = "拖入 RAW 或照片，开始编辑", currentEditLabel;
  EditState gestureStart{};
  bool gesture = false;
  EditState clipboard{};
  bool hasClipboard = false;
  int exportFormat = 0, exportQuality = 95;
  bool exportPopup = false;
  char exportFolder[1024]{};
  bool resettingLayout = true;
  void select(int);
  void loadActive();
  void releasePreview();
  VkDescriptorSet texture(IBP_Image);
  void createThumbnail(int, const DecodeResult &);
  void renderPreview();
  void addPath(const fs::path &);
  void startDecode(int);
  void requestDialog(int);
  static void SDLCALL dialogCallback(void *, const char *const *, int);
  void commit(const std::string &);
  void beginGesture(const char *);
  void endGesture();
  void undo(int);
  void persist(Photo &);
  fs::path sessionPath(const fs::path &) const;
  void openExport();
  void startExport();
  void drawToolbar();
  void drawCanvas();
  void drawAdjustments();
  void drawFilmstrip();
  void drawHistory();
  void drawCurve();
  void histogram(ImVec2, bool);
  bool slider(const char *, int, float, float, float = 0, const char *format = "%.2f");
  void resetGroup(const char *, const std::vector<int> &);

public:
  Editor(SDL_Window *, const IBP_VulkanDevice &);
  ~Editor();
  void add(const char *);
  void event(const SDL_Event &);
  void tick();
  void draw();
  bool canClose();
};
