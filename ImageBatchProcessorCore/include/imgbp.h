#pragma once
#include <stdint.h>
#ifdef _WIN32
#ifdef IBP_BUILD
#define IBP_API __declspec(dllexport)
#else
#define IBP_API __declspec(dllimport)
#endif
#else
#define IBP_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef void *IBP_Context;
typedef uint64_t IBP_Image;
typedef struct {
  uint32_t width, height;
  float *rgba;
} IBP_HostImage;
typedef struct {
  float v[256];
} IBP_Params;
typedef struct {
  double mean, median, p01, p05, p95, p99, rmsContrast, entropy, logAverage,
      dynamicRangeEV;
  double blackClip, whiteClip, meanSaturation, colorfulness, meanR, meanG,
      meanB;
  double labL, labA, labB, sharpness, edgeEnergy, noiseEstimate;
  uint64_t samples;
  uint32_t histogram[4][256];
} IBP_Analysis;
typedef struct {
  IBP_Params parameters;
  double medianLow, medianHigh, p99Low, p99High, maxWhiteClip, maxBlackClip,
      colorfulnessLow, colorfulnessHigh;
  IBP_Analysis reference;
} IBP_Target;
IBP_API const char *IBP_LastError(void);
IBP_API const char *IBP_Version(void);
IBP_API int IBP_Create(const char *shaderDirectoryUtf8, IBP_Context *out);
IBP_API void IBP_Destroy(IBP_Context ctx);
IBP_API const char *IBP_DeviceInfo(IBP_Context ctx);
IBP_API void IBP_DefaultParams(IBP_Params *p);
IBP_API void IBP_DefaultTarget(IBP_Target *t);
IBP_API int IBP_SetParam(IBP_Params *p, const char *name, float value);
IBP_API int IBP_LoadImage(const char *pathUtf8, int halfSize,
                          IBP_HostImage *out);
IBP_API void IBP_FreeHostImage(IBP_HostImage *image);
IBP_API int IBP_SaveImage(const char *pathUtf8, const IBP_HostImage *image,
                          int jpegQuality);
IBP_API int UploadImage(IBP_Context ctx, const IBP_HostImage *image,
                        IBP_Image *handle);
IBP_API int UploadImageBatchMode(IBP_Context ctx, const IBP_HostImage *images,
                                 uint32_t count, IBP_Image *handles);
IBP_API int IBP_ProcessBatch(IBP_Context ctx, const IBP_Image *handles,
                             const IBP_Params *parameters, uint32_t count);
IBP_API int DownloadImage(IBP_Context ctx, IBP_Image handle,
                          IBP_HostImage *image);
IBP_API int DownloadImageBatchMode(IBP_Context ctx, const IBP_Image *handles,
                                   uint32_t count, IBP_HostImage *images);
IBP_API int IBP_ReleaseImage(IBP_Context ctx, IBP_Image handle);
IBP_API int IBP_Analyze(IBP_Context ctx, IBP_Image handle,
                        IBP_Analysis *result);
IBP_API int IBP_SuggestParams(const IBP_Analysis *analysis,
                              const IBP_Target *target, IBP_Params *parameters);
IBP_API int IBP_SaveTarget(const char *pathUtf8, const IBP_Target *target);
IBP_API int IBP_LoadTarget(const char *pathUtf8, IBP_Target *target);
IBP_API int IBP_WriteAnalysisJson(const char *pathUtf8,
                                  const IBP_Analysis *analysis);
#ifdef __cplusplus
}
#endif
