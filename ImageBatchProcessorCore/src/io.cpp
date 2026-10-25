#include "internal.h"
#include <cstring>
#include <atomic>
#include <libraw/libraw.h>
#include <memory>
#include <vector>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static void hr(HRESULT r) {
  if (FAILED(r))
    throw std::runtime_error("Windows imaging error " +
                             std::to_string(uint32_t(r)));
}
struct Com {
  bool owns = false;
  Com() {
    HRESULT r = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owns = SUCCEEDED(r);
    if (FAILED(r) && r != RPC_E_CHANGED_MODE)
      hr(r);
  }
  ~Com() {
    if (owns)
      CoUninitialize();
  }
};
static bool rawExtension(std::filesystem::path p) {
  auto e = p.extension().wstring();
  std::transform(e.begin(), e.end(), e.begin(), ::towlower);
  return std::wstring(L".nef .nrw .cr2 .cr3 .crw .arw .srf .sr2 .dng .raf .rw2 "
                      L".orf .pef .ptx .raw .rwl .srw .3fr .fff .iiq .kdc .dcr "
                      L".mos .mrw .erf .x3f")
                 .find(e) != std::wstring::npos &&
         !e.empty();
}
extern "C" {
void IBP_FreeHostImage(IBP_HostImage *i) {
  if (i) {
    delete[] i->rgba;
    *i = {};
  }
}
int IBP_LoadImage(const char *path, int half, IBP_HostImage *out) {
  IBP_TRY {
    require(out, "Null image output");
    *out = {};
    auto p = upath(path);
    if (rawExtension(p)) {
      LibRaw raw;
      auto &o = raw.imgdata.params;
      o.output_bps = 16;
      o.output_color = 1;
      o.gamm[0] = 1;
      o.gamm[1] = 1;
      o.no_auto_bright = 1;
      o.use_camera_wb = 1;
      o.use_auto_wb = 0;
      o.user_qual = 3;
      o.half_size = half ? 1 : 0;
      o.highlight = 2;
      o.bright = 1;
      auto ck = [](int e) {
        if (e != LIBRAW_SUCCESS)
          throw std::runtime_error(libraw_strerror(e));
      };
      ck(raw.open_file(p.c_str()));
      ck(raw.unpack());
      ck(raw.dcraw_process());
      int err = 0;
      auto mem = raw.dcraw_make_mem_image(&err);
      ck(err);
      require(mem, "LibRaw returned no image");
      std::unique_ptr<libraw_processed_image_t,
                      decltype(&LibRaw::dcraw_clear_mem)>
          guard(mem, LibRaw::dcraw_clear_mem);
      require(mem->type == LIBRAW_IMAGE_BITMAP && mem->bits == 16 &&
                  mem->colors >= 3,
              "Unsupported decoded bitmap");
      size_t count = size_t(mem->width) * mem->height;
      auto pixels = std::make_unique<float[]>(count * 4);
      auto data = reinterpret_cast<const uint16_t *>(mem->data);
      for (size_t i = 0; i < count; i++) {
        for (int j = 0; j < 3; j++)
          pixels[i * 4 + j] = data[i * mem->colors + j] / 65535.f;
        pixels[i * 4 + 3] = 1;
      }
      *out = {mem->width, mem->height, pixels.release()};
      return 0;
    }
    Com com;
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;
    hr(factory->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnLoad,
                                          &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    hr(decoder->GetFrame(0, &frame));
    ComPtr<IWICFormatConverter> converter;
    hr(factory->CreateFormatConverter(&converter));
    hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat48bppRGB,
                             WICBitmapDitherTypeNone, nullptr, 0,
                             WICBitmapPaletteTypeCustom));
    UINT w, h;
    hr(converter->GetSize(&w, &h));
    require(uint64_t(w) * h < 150000000, "Image too large");
    std::vector<uint16_t> data(size_t(w) * h * 3);
    hr(converter->CopyPixels(nullptr, w * 6, UINT(data.size() * 2),
                             (BYTE *)data.data()));
    auto pixels = std::make_unique<float[]>(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; i++) {
      for (int j = 0; j < 3; j++)
        pixels[i * 4 + j] = linear(data[i * 3 + j] / 65535.f);
      pixels[i * 4 + 3] = 1;
    }
    *out = {w, h, pixels.release()};
    return 0;
  }
  IBP_CATCH
}
int IBP_SaveImage(const char *path, const IBP_HostImage *image, int quality) {
  IBP_TRY {
    require(image && image->rgba && image->width && image->height,
            "Empty output image");
    auto p = upath(path);
    require(!std::filesystem::exists(p), "Output exists (overwrite disabled)");
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    bool tiff = ext == L".tif" || ext == L".tiff";
    bool png = ext == L".png";
    require(tiff || png || ext == L".jpg" || ext == L".jpeg",
            "Use .jpg, .png or .tif");
    Com com;
    GUID unique;
    hr(CoCreateGuid(&unique));
    wchar_t suffix[40];
    StringFromGUID2(unique, suffix, 40);
    auto temporary = p;
    temporary += std::wstring(suffix) + L".tmp";
    struct TemporaryFile {
      std::filesystem::path path;
      ~TemporaryFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temporary};
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream));
    hr(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(tiff  ? GUID_ContainerFormatTiff
                              : png ? GUID_ContainerFormatPng
                                    : GUID_ContainerFormatJpeg,
                              nullptr, &encoder));
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr(encoder->CreateNewFrame(&frame, &options));
    PROPBAG2 option{};
    VARIANT value;
    VariantInit(&value);
    if (tiff) {
      option.pstrName = const_cast<wchar_t *>(L"TiffCompressionMethod");
      value.vt = VT_UI1;
      value.bVal = WICTiffCompressionZIP;
    } else if (!png) {
      option.pstrName = const_cast<wchar_t *>(L"ImageQuality");
      value.vt = VT_R4;
      value.fltVal = std::clamp(quality, 1, 100) / 100.f;
    }
    if (!png)
      hr(options->Write(1, &option, &value));
    hr(frame->Initialize(options.Get()));
    hr(frame->SetSize(image->width, image->height));
    hr(frame->SetResolution(300, 300));
    WICPixelFormatGUID format =
        tiff || png ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
    auto requested = format;
    hr(frame->SetPixelFormat(&format));
    require(IsEqualGUID(format, requested), "Encoder rejected pixel precision");
    ComPtr<IWICColorContext> color;
    hr(factory->CreateColorContext(&color));
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    auto icc = std::filesystem::path(windir) /
               L"System32/spool/drivers/color/sRGB Color Space Profile.icm";
    if (SUCCEEDED(color->InitializeFromFilename(icc.c_str()))) {
      IWICColorContext *c = color.Get();
      hr(frame->SetColorContexts(1, &c));
    }
    uint32_t bytes = tiff || png ? 6 : 3;
    size_t length = size_t(image->width) * image->height * bytes;
    require(length <= UINT32_MAX, "Encoded pixel buffer exceeds WIC limit");
    std::vector<unsigned char> encoded(length);
    std::atomic<bool> invalid{false};
#pragma omp parallel for schedule(static)
    for (int64_t y = 0; y < int64_t(image->height); y++) {
      auto row = encoded.data() + size_t(y) * image->width * bytes;
      for (uint32_t x = 0; x < image->width; x++) {
        size_t i = (size_t(y) * image->width + x) * 4;
        for (int j = 0; j < 3; j++) {
          float v = image->rgba[i + j];
          if (!std::isfinite(v)) { invalid.store(true, std::memory_order_relaxed); v=0; }
          float s = std::clamp(srgb(std::max(v, 0.f)), 0.f, 1.f);
          if (bytes == 6)
            reinterpret_cast<uint16_t *>(row)[x * 3 + j] =
                uint16_t(std::round(s * 65535));
          else
            row[x * 3 + 2 - j] = uint8_t(std::round(s * 255));
        }
      }
    }
    require(!invalid.load(), "Non-finite output pixels");
    hr(frame->WritePixels(image->height, image->width * bytes, UINT(encoded.size()), encoded.data()));
    hr(frame->Commit());
    hr(encoder->Commit());
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    // Same-directory rename publishes a complete image without overwriting an existing one.
    std::filesystem::rename(temporary, p);
    return 0;
  }
  IBP_CATCH
}
}
