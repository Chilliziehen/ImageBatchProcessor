#pragma once
#include "imgbp.h"
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller owns Vulkan objects. All external queue use must be serialized with Core calls.
   Caller must finish GPU readers before releasing/changing an exposed image. */
typedef struct {
  VkInstance instance;
  VkPhysicalDevice physicalDevice;
  VkDevice device;
  VkQueue queue;
  uint32_t queueFamily;
} IBP_VulkanDevice;
typedef struct {
  uint32_t count[4];
  uint32_t interpolation[4];
  float points[4][32][2];
  float lut[4][1024];
} IBP_CurveSet;
typedef struct {
  IBP_Target target;
  IBP_CurveSet curves;
} IBP_Recipe;
IBP_API int IBP_CreateSharedVulkan(const IBP_VulkanDevice *, const char *, IBP_Context *);
IBP_API int IBP_CopyImage(IBP_Context, IBP_Image source, IBP_Image destination);
IBP_API int IBP_EncodeDisplay(IBP_Context, IBP_Image source, IBP_Image destination);
IBP_API int IBP_GetVulkanView(IBP_Context, IBP_Image, VkImageView *);
IBP_API int IBP_ProcessBatchCurves(IBP_Context, const IBP_Image *, const IBP_Params *,
                                   const IBP_CurveSet *, uint32_t);
IBP_API void IBP_DefaultCurves(IBP_CurveSet *);
IBP_API int IBP_CurvesFromParams(const IBP_Params *, IBP_CurveSet *);
IBP_API int IBP_BuildCurves(IBP_CurveSet *);
IBP_API int IBP_SaveRecipe(const char *, const IBP_Recipe *);
IBP_API int IBP_LoadRecipe(const char *, IBP_Recipe *);
#ifdef __cplusplus
}
#endif
