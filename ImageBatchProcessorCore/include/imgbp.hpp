#pragma once
#include "imgbp.h"
#include <stdexcept>
#include <span>
#include <utility>
#include <vector>
namespace imgbp {
inline void check(int status){if(status)throw std::runtime_error(IBP_LastError());}
class HostImage {
public:
 IBP_HostImage data{};
 HostImage()=default;
 explicit HostImage(const char* path,bool half=false){check(IBP_LoadImage(path,half,&data));}
 ~HostImage(){IBP_FreeHostImage(&data);}
 HostImage(const HostImage&)=delete;
 HostImage& operator=(const HostImage&)=delete;
 HostImage(HostImage&& other) noexcept:data(std::exchange(other.data,{})){}
 HostImage& operator=(HostImage&& other) noexcept{if(this!=&other){IBP_FreeHostImage(&data);data=std::exchange(other.data,{});}return *this;}
 void save(const char* path,int quality=95) const{check(IBP_SaveImage(path,&data,quality));}
};
class Processor {
 IBP_Context context_{};
public:
 explicit Processor(const char* shaders=nullptr){check(IBP_Create(shaders,&context_));}
 ~Processor(){IBP_Destroy(context_);}
 Processor(const Processor&)=delete;
 Processor& operator=(const Processor&)=delete;
 const char* device() const{return IBP_DeviceInfo(context_);}
 IBP_Image upload(const HostImage& host){IBP_Image id;check(UploadImage(context_,&host.data,&id));return id;}
 std::vector<IBP_Image> upload(std::span<const IBP_HostImage> images){std::vector<IBP_Image> ids(images.size());check(UploadImageBatchMode(context_,images.data(),uint32_t(images.size()),ids.data()));return ids;}
 void process(std::span<const IBP_Image> images,std::span<const IBP_Params> parameters){if(images.size()!=parameters.size())throw std::invalid_argument("Batch sizes differ");check(IBP_ProcessBatch(context_,images.data(),parameters.data(),uint32_t(images.size())));}
 HostImage download(IBP_Image id){HostImage host;check(DownloadImage(context_,id,&host.data));return host;}
 IBP_Analysis analyze(IBP_Image id){IBP_Analysis a;check(IBP_Analyze(context_,id,&a));return a;}
 void release(IBP_Image id){check(IBP_ReleaseImage(context_,id));}
 IBP_Context native() const{return context_;}
};
inline IBP_Params defaults(){IBP_Params p;IBP_DefaultParams(&p);return p;}
}
