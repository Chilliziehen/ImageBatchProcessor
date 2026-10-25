#include "internal.h"
#include "imgbp_editor.h"
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>
thread_local std::string ibp_error;
static void check(VkResult r) {
  if (r != VK_SUCCESS)
    throw std::runtime_error("Vulkan error " + std::to_string(r));
}
struct Buffer {
  VkDevice dev{};
  VkBuffer b{};
  VkDeviceMemory m{};
  void *data{};
  ~Buffer() {
    if (data)
      vkUnmapMemory(dev, m);
    if (b)
      vkDestroyBuffer(dev, b, nullptr);
    if (m)
      vkFreeMemory(dev, m, nullptr);
  }
};
struct Resident {
  VkDevice dev{};
  uint32_t w{}, h{}, current{};
  VkImage im[2]{};
  VkImageView view[2]{};
  VkDeviceMemory mem[2]{};
  ~Resident() {
    for (int i = 0; i < 2; i++) {
      if (view[i])
        vkDestroyImageView(dev, view[i], nullptr);
      if (im[i])
        vkDestroyImage(dev, im[i], nullptr);
      if (mem[i])
        vkFreeMemory(dev, mem[i], nullptr);
    }
  }
};
struct Context {
  bool ownsDevice = true;
  VkInstance instance{};
  VkPhysicalDevice gpu{};
  VkDevice device{};
  VkQueue cq{}, tq{};
  uint32_t cf{}, tf{};
  VkCommandPool cp{}, tp{};
  VkDescriptorSetLayout layout{};
  VkPipelineLayout pl{};
  VkPipeline process{}, analyze{};
  VkSemaphore chain[2]{};
  int previous = -1;
  std::string info;
  VkPhysicalDeviceProperties properties{};
  std::unordered_map<uint64_t, std::unique_ptr<Resident>> images;
  uint64_t next = 1;
  std::mutex mutex;
  ~Context() {
    if (device) {
      vkDeviceWaitIdle(device);
      images.clear();
      if (process)
        vkDestroyPipeline(device, process, nullptr);
      if (analyze)
        vkDestroyPipeline(device, analyze, nullptr);
      if (pl)
        vkDestroyPipelineLayout(device, pl, nullptr);
      if (layout)
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
      if (cp)
        vkDestroyCommandPool(device, cp, nullptr);
      if (tp)
        vkDestroyCommandPool(device, tp, nullptr);
      for (auto s : chain)
        if (s)
          vkDestroySemaphore(device, s, nullptr);
      if (ownsDevice) vkDestroyDevice(device, nullptr);
    }
    if (instance && ownsDevice)
      vkDestroyInstance(instance, nullptr);
  }
  uint32_t memory(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
      if ((bits & (1u << i)) &&
          (p.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No compatible Vulkan memory type");
  }
  std::unique_ptr<Buffer> buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    auto r = std::make_unique<Buffer>();
    r->dev = device;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    check(vkCreateBuffer(device, &ci, nullptr, &r->b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, r->b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    auto hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // CPU reads from write-combined heaps can be orders of magnitude slower.
    try {
      ai.memoryTypeIndex = memory(mr.memoryTypeBits, hostFlags |
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    } catch (const std::exception &) {
      ai.memoryTypeIndex = memory(mr.memoryTypeBits, hostFlags);
    }
    check(vkAllocateMemory(device, &ai, nullptr, &r->m));
    check(vkBindBufferMemory(device, r->b, r->m, 0));
    check(vkMapMemory(device, r->m, 0, VK_WHOLE_SIZE, 0, &r->data));
    return r;
  }
  VkCommandBuffer begin(bool transfer) {
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = transfer ? tp : cp;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
  }
  void submit(VkCommandBuffer cmd, bool transfer) {
    check(vkEndCommandBuffer(cmd));
    VkFence fence;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fi, nullptr, &fence));
    int signal = previous < 0 ? 0 : 1 - previous;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &chain[signal];
    if (previous >= 0) {
      si.waitSemaphoreCount = 1;
      si.pWaitSemaphores = &chain[previous];
      si.pWaitDstStageMask = &stage;
    }
    VkResult r = vkQueueSubmit(transfer ? tq : cq, 1, &si, fence);
    if (r == VK_SUCCESS) {
      previous = signal;
      r = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transfer ? tp : cp, 1, &cmd);
    check(r);
  }
  void barrier(VkCommandBuffer cmd, VkImage im, bool initial = false) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = initial ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
  Resident &get(uint64_t id) {
    auto it = images.find(id);
    require(it != images.end(), "Invalid or released image handle");
    return *it->second;
  }
  VkPipeline pipeline(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    require(bool(f), "SPIR-V file not found");
    auto bytes = f.tellg();
    require(bytes > 0 && size_t(bytes) % 4 == 0, "Malformed SPIR-V size");
    std::vector<uint32_t> code(size_t(bytes) / 4);
    f.seekg(0);
    f.read((char *)code.data(), bytes);
    require(code[0] == 0x07230203, "Invalid SPIR-V magic");
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size_t(bytes);
    mi.pCode = code.data();
    VkShaderModule module;
    check(vkCreateShaderModule(device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.layout = pl;
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = module;
    pi.stage.pName = "main";
    VkPipeline p;
    auto r =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
    vkDestroyShaderModule(device, module, nullptr);
    check(r);
    return p;
  }
  void init(const char *directory) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ImageBatchProcessor";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    check(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 0;
    check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    require(n > 0, "No Vulkan GPU");
    std::vector<VkPhysicalDevice> devices(n);
    check(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
    int best = -1;
    for (auto g : devices) {
      VkPhysicalDeviceProperties prop;
      vkGetPhysicalDeviceProperties(g, &prop);
      uint32_t qn;
      vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
      for (uint32_t i = 0; i < qn; i++)
        if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
          int rank =
              prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 100
              : prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 80
                                                                          : 1;
          if (rank > best) {
            best = rank;
            gpu = g;
            cf = i;
            properties = prop;
          }
          break;
        }
    }
    require(gpu != VK_NULL_HANDLE, "No compute capable Vulkan device");
    uint32_t qn;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qs.data());
    tf = cf;
    bool dedicated = false;
    for (uint32_t i = 0; i < qn; i++)
      if ((qs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
          !(qs[i].queueFlags &
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
        tf = i;
        dedicated = true;
        break;
      }
    if (!dedicated)
      for (uint32_t i = 0; i < qn; i++)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          tf = i;
          break;
        }
    float priority = 1;
    std::vector<VkDeviceQueueCreateInfo> qci;
    for (auto i : std::set<uint32_t>{cf, tf}) {
      VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      q.queueFamilyIndex = i;
      q.queueCount = 1;
      q.pQueuePriorities = &priority;
      qci.push_back(q);
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = uint32_t(qci.size());
    di.pQueueCreateInfos = qci.data();
    check(vkCreateDevice(gpu, &di, nullptr, &device));
    vkGetDeviceQueue(device, cf, 0, &cq);
    vkGetDeviceQueue(device, tf, 0, &tq);
    resources(directory, dedicated);
  }
  void resources(const char* directory, bool dedicated) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R32G32B32A32_SFLOAT,
                                        &fp);
    constexpr auto requiredFormatFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    require((fp.optimalTilingFeatures & requiredFormatFeatures) == requiredFormatFeatures,
            "RGBA32F storage/linear sampled images unsupported");
    for (int i = 0; i < 2; i++) {
      VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pi.queueFamilyIndex = i ? tf : cf;
      pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      check(vkCreateCommandPool(device, &pi, nullptr, i ? &tp : &cp));
      VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      check(vkCreateSemaphore(device, &si, nullptr, &chain[i]));
    }
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (int i = 0; i < 4; i++) {
      bindings[i].binding = i;
      bindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &pli, nullptr, &pl));
    std::filesystem::path dir;
    if (directory && *directory)
      dir = upath(directory);
    else {
      wchar_t path[32768];
      HMODULE mod;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)&IBP_Create, &mod);
      GetModuleFileNameW(mod, path, 32768);
      dir = std::filesystem::path(path).parent_path() / "shaders";
      if (!std::filesystem::exists(dir))
        dir = IBP_SHADER_DIR;
    }
    process = pipeline(dir / "process.spv");
    analyze = pipeline(dir / "analyze.spv");
    info = std::string(properties.deviceName) +
           "; compute=" + std::to_string(cf) +
           "; transfer=" + std::to_string(tf) +
           (dedicated ? " (dedicated)" : " (fallback)") +
           "; resident RGBA32F; batched descriptor sets";
  }
};
static Context &context(IBP_Context c) {
  require(c, "Null context");
  return *static_cast<Context *>(c);
}
struct Sets {
  VkDevice dev;
  VkDescriptorPool pool{};
  std::vector<VkDescriptorSet> sets;
  Sets(Context &c, uint32_t n) : dev(c.device) {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n * 2},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n * 2}};
    VkDescriptorPoolCreateInfo pi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = n;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(dev, &pi, nullptr, &pool));
    std::vector<VkDescriptorSetLayout> layouts(n, c.layout);
    sets.resize(n);
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = n;
    ai.pSetLayouts = layouts.data();
    check(vkAllocateDescriptorSets(dev, &ai, sets.data()));
  }
  ~Sets() { vkDestroyDescriptorPool(dev, pool, nullptr); }
  void update(uint32_t i, Resident &im, uint32_t src, Buffer &args,
              Buffer &stats) {
    VkDescriptorImageInfo image[] = {
        {VK_NULL_HANDLE, im.view[src], VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, im.view[1 - src], VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo bufs[] = {{args.b, 0, VK_WHOLE_SIZE},
                                     {stats.b, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[4]{};
    for (int j = 0; j < 4; j++) {
      auto &w = writes[j];
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = sets[i];
      w.dstBinding = j;
      w.descriptorCount = 1;
      w.descriptorType = j < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                               : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      if (j < 2)
        w.pImageInfo = &image[j];
      else
        w.pBufferInfo = &bufs[j - 2];
    }
    vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
  }
};
extern "C" {
const char *IBP_LastError() { return ibp_error.c_str(); }
const char *IBP_Version() { return "1.1.0"; }
int IBP_Create(const char *d, IBP_Context *out) {
  IBP_TRY {
    require(out, "Null output");
    *out = nullptr;
    auto c = std::make_unique<Context>();
    c->init(d);
    *out = c.release();
    return 0;
  }
  IBP_CATCH
}

int IBP_CreateSharedVulkan(const IBP_VulkanDevice* shared,const char* dir,IBP_Context* out) {
  IBP_TRY {
    require(shared && out && shared->instance && shared->device && shared->physicalDevice && shared->queue,"Invalid shared device");
    *out=nullptr;
    auto c=std::make_unique<Context>();
    c->ownsDevice=false;c->instance=shared->instance;c->gpu=shared->physicalDevice;
    c->device=shared->device;c->cq=c->tq=shared->queue;c->cf=c->tf=shared->queueFamily;
    vkGetPhysicalDeviceProperties(c->gpu,&c->properties);
    uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(c->gpu,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu,&count,families.data());
    require(c->cf<count && (families[c->cf].queueFlags&VK_QUEUE_COMPUTE_BIT),"Shared queue needs compute");
    c->resources(dir,false);*out=c.release();return 0;
  } IBP_CATCH
}
int IBP_GetVulkanView(IBP_Context opaque,IBP_Image handle,VkImageView* view) {
  IBP_TRY {
    auto& c=context(opaque);std::lock_guard lock(c.mutex);require(view,"Null view");
    auto& im=c.get(handle);*view=im.view[im.current];return 0;
  } IBP_CATCH
}
int IBP_CopyImage(IBP_Context opaque,IBP_Image source,IBP_Image destination) {
  IBP_TRY {
    auto& c=context(opaque);std::lock_guard lock(c.mutex);
    require(source!=destination,"Copy requires distinct handles");
    auto& a=c.get(source);auto& b=c.get(destination);
    require(a.w==b.w && a.h==b.h,"Copy dimensions differ");
    auto cmd=c.begin(false);
    c.barrier(cmd,a.im[a.current]);c.barrier(cmd,b.im[b.current]);
    VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.extent={a.w,a.h,1};
    vkCmdCopyImage(cmd,a.im[a.current],VK_IMAGE_LAYOUT_GENERAL,b.im[b.current],VK_IMAGE_LAYOUT_GENERAL,1,&region);
    c.barrier(cmd,b.im[b.current]);c.submit(cmd,false);return 0;
  } IBP_CATCH
}
int IBP_EncodeDisplay(IBP_Context opaque,IBP_Image source,IBP_Image destination) {
  IBP_TRY {
    auto& c=context(opaque);std::lock_guard lock(c.mutex);
    require(source!=destination,"Display requires distinct handles");
    auto& a=c.get(source);auto& b=c.get(destination);
    require(a.w==b.w && a.h==b.h,"Display dimensions differ");
    auto args=c.buffer(sizeof(IBP_Params),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto stats=c.buffer(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Sets sets(c,1);sets.update(0,a,a.current,*args,*stats);
    VkDescriptorImageInfo dst{VK_NULL_HANDLE,b.view[b.current],VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=sets.sets[0];write.dstBinding=1;write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&dst;
    vkUpdateDescriptorSets(c.device,1,&write,0,nullptr);
    auto cmd=c.begin(false);c.barrier(cmd,a.im[a.current]);c.barrier(cmd,b.im[b.current]);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,c.process);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,c.pl,0,1,sets.sets.data(),0,nullptr);
    uint32_t push[]={0,4,0,0};vkCmdPushConstants(cmd,c.pl,VK_SHADER_STAGE_COMPUTE_BIT,0,16,push);
    vkCmdDispatch(cmd,(a.w+15)/16,(a.h+15)/16,1);
    c.barrier(cmd,b.im[b.current]);c.submit(cmd,false);return 0;
  } IBP_CATCH
}

void IBP_Destroy(IBP_Context c) { delete static_cast<Context *>(c); }
const char *IBP_DeviceInfo(IBP_Context c) {
  return c ? static_cast<Context *>(c)->info.c_str() : "No context";
}
int UploadImageBatchMode(IBP_Context opaque, const IBP_HostImage *src,
                         uint32_t n, IBP_Image *handles) {
  IBP_TRY {
    auto &c = context(opaque);
    std::lock_guard lock(c.mutex);
    require(src && handles && n > 0 && n <= 64, "Invalid upload batch");
    std::vector<std::unique_ptr<Buffer>> stages;
    std::vector<std::unique_ptr<Resident>> pending;
    for (uint32_t i = 0; i < n; i++) {
      handles[i] = 0;
      require(src[i].rgba && src[i].width && src[i].height, "Empty image");
      require(src[i].width <= c.properties.limits.maxImageDimension2D &&
                  src[i].height <= c.properties.limits.maxImageDimension2D,
              "Image exceeds GPU dimension limit");
      auto r = std::make_unique<Resident>();
      r->dev = c.device;
      r->w = src[i].width;
      r->h = src[i].height;
      for (int j = 0; j < 2; j++) {
        uint32_t families[] = {c.cf, c.tf};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ci.extent = {r->w, r->h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (c.cf != c.tf) {
          ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
          ci.queueFamilyIndexCount = 2;
          ci.pQueueFamilyIndices = families;
        }
        check(vkCreateImage(c.device, &ci, nullptr, &r->im[j]));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(c.device, r->im[j], &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex =
            c.memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(c.device, &ai, nullptr, &r->mem[j]));
        check(vkBindImageMemory(c.device, r->im[j], r->mem[j], 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = r->im[j];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(c.device, &vi, nullptr, &r->view[j]));
      }
      auto stage = c.buffer(uint64_t(r->w) * r->h * 16,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
      memcpy(stage->data, src[i].rgba, size_t(r->w) * r->h * 16);
      stages.push_back(std::move(stage));
      pending.push_back(std::move(r));
    }
    auto cmd = c.begin(true);
    for (uint32_t i = 0; i < n; i++) {
      auto &r = *pending[i];
      c.barrier(cmd, r.im[0], true);
      c.barrier(cmd, r.im[1], true);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {r.w, r.h, 1};
      vkCmdCopyBufferToImage(cmd, stages[i]->b, r.im[0],
                             VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    c.submit(cmd, true);
    for (uint32_t i = 0; i < n; i++) {
      handles[i] = c.next++;
      c.images.emplace(handles[i], std::move(pending[i]));
    }
    return 0;
  }
  IBP_CATCH
}
int UploadImage(IBP_Context c, const IBP_HostImage *s, IBP_Image *h) {
  return UploadImageBatchMode(c, s, 1, h);
}
int IBP_ProcessBatchCurves(IBP_Context opaque, const IBP_Image *handles,
                     const IBP_Params *params, const IBP_CurveSet* curves, uint32_t n) {
  IBP_TRY {
    auto &c = context(opaque);
    std::lock_guard lock(c.mutex);
    require(handles && params && n > 0 && n <= 64, "Invalid process batch");
    std::set<uint64_t> unique;
    for (uint32_t i = 0; i < n; i++) {
      c.get(handles[i]);
      require(unique.insert(handles[i]).second, "Duplicate batch handle");
      for (float v : params[i].v)
        require(std::isfinite(v), "Non-finite processing parameter");
    }
    auto args =
        c.buffer(n * sizeof(IBP_Params) + (curves ? n * 4096ull * sizeof(float) : 0), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(args->data, params, n * sizeof(IBP_Params));
    if(curves) for(uint32_t i=0;i<n;i++) {
      for(auto& channel:curves[i].lut) for(float v:channel)
        require(std::isfinite(v) && v>=0 && v<=1, "Invalid curve LUT");
      memcpy((char*)args->data+n*sizeof(IBP_Params)+i*4096ull*sizeof(float),curves[i].lut,4096*sizeof(float));
    }
    auto dummy = c.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Sets sets(c, n * 2);
    for (uint32_t i = 0; i < n; i++)
      for (uint32_t j = 0; j < 2; j++)
        sets.update(i * 2 + j, c.get(handles[i]), j, *args, *dummy);
    auto cmd = c.begin(false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.process);
    for (uint32_t i = 0; i < n; i++) {
      auto &im = c.get(handles[i]);
      for (uint32_t pass = 0; pass < 4; pass++) {
        bool active =
            pass == 2 ||
            (pass == 0 && (params[i].v[17] != 0 || params[i].v[19] != 0 ||
                           params[i].v[20] != 0 || params[i].v[21] != 0 ||
                           params[i].v[22] != 0 || params[i].v[23] != 0 ||
                           params[i].v[24] != 1 || params[i].v[25] != 0)) ||
            (pass == 1 && params[i].v[13] > 0) ||
            (pass == 3 && (params[i].v[12] != 0 || params[i].v[14] != 0));
        if (!active)
          continue;
        c.barrier(cmd, im.im[im.current]);
        c.barrier(cmd, im.im[1 - im.current]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1,
                                &sets.sets[i * 2 + im.current], 0, nullptr);
        uint32_t push[] = {i, pass, curves ? n*256+i*4096 : 0, im.h};
        vkCmdPushConstants(cmd, c.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
        vkCmdDispatch(cmd, (im.w + 15) / 16, (im.h + 15) / 16, 1);
        im.current = 1 - im.current;
      }
    }
    c.submit(cmd, false);
    return 0;
  }
  IBP_CATCH
}
int IBP_ProcessBatch(IBP_Context c,const IBP_Image* images,const IBP_Params* params,uint32_t n) {
  return IBP_ProcessBatchCurves(c,images,params,nullptr,n);
}
int DownloadImageBatchMode(IBP_Context opaque, const IBP_Image *handles,
                           uint32_t n, IBP_HostImage *out) {
  IBP_TRY {
    auto &c = context(opaque);
    std::lock_guard lock(c.mutex);
    require(handles && out && n > 0 && n <= 64, "Invalid download batch");
    std::vector<std::unique_ptr<Buffer>> stages;
    std::vector<std::unique_ptr<float[]>> host;
    for (uint32_t i = 0; i < n; i++) {
      out[i] = {};
      auto &im = c.get(handles[i]);
      stages.push_back(c.buffer(uint64_t(im.w) * im.h * 16,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT));
      host.push_back(std::make_unique<float[]>(size_t(im.w) * im.h * 4));
    }
    auto cmd = c.begin(true);
    for (uint32_t i = 0; i < n; i++) {
      auto &im = c.get(handles[i]);
      c.barrier(cmd, im.im[im.current]);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {im.w, im.h, 1};
      vkCmdCopyImageToBuffer(cmd, im.im[im.current], VK_IMAGE_LAYOUT_GENERAL,
                             stages[i]->b, 1, &copy);
    }
    c.submit(cmd, true);
    for (uint32_t i = 0; i < n; i++) {
      auto &im = c.get(handles[i]);
      memcpy(host[i].get(), stages[i]->data, size_t(im.w) * im.h * 16);
      out[i] = {im.w, im.h, host[i].release()};
    }
    return 0;
  }
  IBP_CATCH
}
int DownloadImage(IBP_Context c, IBP_Image h, IBP_HostImage *o) {
  return DownloadImageBatchMode(c, &h, 1, o);
}
int IBP_ReleaseImage(IBP_Context opaque, IBP_Image h) {
  IBP_TRY {
    auto &c = context(opaque);
    std::lock_guard lock(c.mutex);
    require(c.images.erase(h) == 1, "Invalid image handle");
    return 0;
  }
  IBP_CATCH
}
int IBP_Analyze(IBP_Context opaque, IBP_Image h, IBP_Analysis *out) {
  IBP_TRY {
    auto &c = context(opaque);
    std::lock_guard lock(c.mutex);
    require(out, "Null analysis output");
    auto &im = c.get(h);
    uint32_t stride = std::max(
        1u, uint32_t(std::ceil(std::sqrt(double(im.w) * im.h / 1048576.))));
    uint32_t count = ((im.w + stride - 1) / stride) *
                     ((im.h + stride - 1) / stride),
             groups = (count + 255) / 256;
    auto result =
        c.buffer(4096 + groups * 96ull, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(result->data, 0, 4096 + groups * 96ull);
    auto args = c.buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Sets sets(c, 1);
    sets.update(0, im, im.current, *args, *result);
    auto cmd = c.begin(false);
    c.barrier(cmd, im.im[im.current]);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.analyze);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1,
                            sets.sets.data(), 0, nullptr);
    uint32_t push[] = {0, stride, im.w, im.h};
    vkCmdPushConstants(cmd, c.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
    vkCmdDispatch(cmd, groups, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    c.submit(cmd, false);
    *out = {};
    memcpy(out->histogram, result->data, 4096);
    double s[24]{};
    auto data = (float *)((char *)result->data + 4096);
    for (uint32_t g = 0; g < groups; g++)
      for (int j = 0; j < 24; j++)
        s[j] += data[g * 24 + j];
    double n = s[7];
    require(n > 0, "No analysis samples");
    out->samples = uint64_t(n);
    out->meanR = s[0] / n;
    out->meanG = s[1] / n;
    out->meanB = s[2] / n;
    out->mean = s[3] / n;
    out->rmsContrast =
        std::sqrt(std::max(0., s[4] / n - out->mean * out->mean));
    out->logAverage = std::exp(s[5] / n);
    out->meanSaturation = s[6] / n;
    out->colorfulness =
        std::sqrt(std::max(0., (s[9] + s[11]) / n -
                                   (s[8] * s[8] + s[10] * s[10]) / (n * n))) +
        .3 * std::hypot(s[8], s[10]) / n;
    out->labL = s[12] / n;
    out->labA = s[13] / n;
    out->labB = s[14] / n;
    out->sharpness = s[15] / n;
    out->edgeEnergy = s[16] / n;
    out->blackClip = s[18] / n;
    out->whiteClip = s[19] / n;
    out->noiseEstimate = s[21] > 0 ? s[20] / s[21] / std::sqrt(20.) : 0;
    auto percentile = [&](double fraction) {
      uint64_t total = 0;
      for (int i = 0; i < 256; i++) {
        total += out->histogram[0][i];
        if (total >= fraction * n)
          return (i + .5) / 256.;
      }
      return 1.;
    };
    out->p01 = percentile(.01);
    out->p05 = percentile(.05);
    out->median = percentile(.5);
    out->p95 = percentile(.95);
    out->p99 = percentile(.99);
    out->dynamicRangeEV = std::log2(std::max(1e-7f, linear(float(out->p99))) /
                                    std::max(1e-7f, linear(float(out->p01))));
    for (int i = 0; i < 256; i++) {
      double p = out->histogram[0][i] / n;
      if (p > 0)
        out->entropy -= p * std::log2(p);
    }
    return 0;
  }
  IBP_CATCH
}
}
