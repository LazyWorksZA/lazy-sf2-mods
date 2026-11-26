#include "hooks/wic_jpeg.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

namespace LazyWorks::SF2Patch::hooks {

using Microsoft::WRL::ComPtr;

struct JpegStreamWriter::Impl {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    std::vector<BYTE> row;
    int w = 0, h = 0, written = 0;
    bool failed = false;
};

JpegStreamWriter::JpegStreamWriter() = default;
JpegStreamWriter::~JpegStreamWriter() = default;

bool JpegStreamWriter::open(const std::wstring& path, int w, int h, float quality) {
    impl_.reset();
    if (w < 1 || h < 1 || w > 65535 || h > 65535) return false;
    auto im = std::make_unique<Impl>();
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    if (FAILED(factory->CreateStream(&im->stream)) || FAILED(im->stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &im->encoder)) ||
        FAILED(im->encoder->Initialize(im->stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IPropertyBag2> props;
    if (FAILED(im->encoder->CreateNewFrame(&im->frame, &props))) return false;
    PROPBAG2 opt = {};
    wchar_t name[] = L"ImageQuality";
    opt.pstrName = name;
    VARIANT v{};  // VT_EMPTY; a float needs no clearing
    v.vt = VT_R4;
    v.fltVal = quality;
    if (FAILED(props->Write(1, &opt, &v))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(im->frame->Initialize(props.Get())) ||
        FAILED(im->frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h))) ||
        FAILED(im->frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR)
        return false;
    im->row.resize(static_cast<size_t>(w) * 3);
    im->w = w;
    im->h = h;
    impl_ = std::move(im);
    return true;
}

bool JpegStreamWriter::write_rows(const uint8_t* rgb, int rows) {
    if (!impl_ || impl_->failed || !rgb || rows < 0 || rows > impl_->h - impl_->written) return false;
    Impl& im = *impl_;
    const size_t stride = im.row.size();
    for (int y = 0; y < rows; ++y) {
        const uint8_t* src = rgb + static_cast<size_t>(y) * stride;
        for (size_t x = 0; x < stride; x += 3) {
            im.row[x] = src[x + 2];
            im.row[x + 1] = src[x + 1];
            im.row[x + 2] = src[x];
        }
        if (FAILED(im.frame->WritePixels(1, static_cast<UINT>(stride), static_cast<UINT>(stride), im.row.data()))) {
            im.failed = true;
            return false;
        }
        ++im.written;
    }
    return true;
}

bool JpegStreamWriter::finish() {
    if (!impl_) return false;
    Impl& im = *impl_;
    const bool ok = !im.failed && im.written == im.h && SUCCEEDED(im.frame->Commit()) && SUCCEEDED(im.encoder->Commit()) &&
                    SUCCEEDED(im.stream->Commit(STGC_DEFAULT));
    impl_.reset();
    return ok;
}

bool write_jpeg_wic(const acmi::RGBImage& img, const std::wstring& path, float quality) {
    if (img.w < 1 || img.h < 1 || img.px.size() != static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 3)
        return false;
    JpegStreamWriter jw;
    return jw.open(path, img.w, img.h, quality) && jw.write_rows(img.px.data(), img.h) && jw.finish();
}

}  // namespace LazyWorks::SF2Patch::hooks
