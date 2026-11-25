#include "hooks/wic_jpeg.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

namespace lazysf2::hooks {

using Microsoft::WRL::ComPtr;

bool write_jpeg_wic(const acmi::RGBImage& img, const std::wstring& path, float quality) {
    if (img.w < 1 || img.h < 1 || img.px.size() != static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 3)
        return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    PROPBAG2 opt = {};
    wchar_t name[] = L"ImageQuality";
    opt.pstrName = name;
    VARIANT v{};  // VT_EMPTY; a float needs no clearing
    v.vt = VT_R4;
    v.fltVal = quality;
    if (FAILED(props->Write(1, &opt, &v))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.Get())) ||
        FAILED(frame->SetSize(static_cast<UINT>(img.w), static_cast<UINT>(img.h))) ||
        FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR)
        return false;
    std::vector<BYTE> row(static_cast<size_t>(img.w) * 3);
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* src = &img.px[static_cast<size_t>(y) * static_cast<size_t>(img.w) * 3];
        for (int x = 0; x < img.w; ++x) {
            row[static_cast<size_t>(x) * 3] = src[x * 3 + 2];
            row[static_cast<size_t>(x) * 3 + 1] = src[x * 3 + 1];
            row[static_cast<size_t>(x) * 3 + 2] = src[x * 3];
        }
        if (FAILED(frame->WritePixels(1, static_cast<UINT>(row.size()), static_cast<UINT>(row.size()), row.data())))
            return false;
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit()) || FAILED(stream->Commit(STGC_DEFAULT))) return false;
    return true;
}

}  // namespace lazysf2::hooks
