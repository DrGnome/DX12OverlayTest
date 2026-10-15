#include "text_bitmap.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

constexpr UINT BitmapWidth = 1200;
constexpr UINT BitmapHeight = 360;

HRESULT CopyBitmapPixels(IWICBitmap* bitmap, TextBitmap& output)
{
    ComPtr<IWICBitmapLock> bitmapLock;
    WICRect rectangle{0, 0, static_cast<INT>(BitmapWidth),
                      static_cast<INT>(BitmapHeight)};
    HRESULT result = bitmap->Lock(&rectangle, WICBitmapLockRead, &bitmapLock);
    if (FAILED(result))
    {
        return result;
    }

    UINT sourceStride = 0;
    UINT byteCount = 0;
    result = bitmapLock->GetStride(&sourceStride);
    if (FAILED(result))
    {
        return result;
    }
    BYTE* sourcePixels = nullptr;
    result = bitmapLock->GetDataPointer(&byteCount, &sourcePixels);
    if (FAILED(result))
    {
        return result;
    }

    constexpr UINT bytesPerPixel = 4;
    const UINT outputStride = BitmapWidth * bytesPerPixel;
    if (!sourcePixels || sourceStride < outputStride ||
        byteCount < sourceStride * BitmapHeight)
    {
        return E_UNEXPECTED;
    }

    output.width = BitmapWidth;
    output.height = BitmapHeight;
    output.rowPitch = outputStride;
    output.pixels.resize(static_cast<std::size_t>(outputStride) * BitmapHeight);
    for (UINT row = 0; row < BitmapHeight; ++row)
    {
        memcpy(output.pixels.data() + static_cast<std::size_t>(row) * outputStride,
               sourcePixels + static_cast<std::size_t>(row) * sourceStride,
               outputStride);
    }
    return S_OK;
}
} // namespace

HRESULT CreateTextBitmap(TextBitmap& output)
{
    ComPtr<IWICImagingFactory> imagingFactory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&imagingFactory));
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<IWICBitmap> bitmap;
    result = imagingFactory->CreateBitmap(
        BitmapWidth, BitmapHeight, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<ID2D1Factory> d2dFactory;
    const D2D1_FACTORY_OPTIONS factoryOptions{};
    result = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
        &factoryOptions,
        reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
    if (FAILED(result))
    {
        return result;
    }

    D2D1_RENDER_TARGET_PROPERTIES renderTargetProperties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(
                DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> renderTarget;
    result = d2dFactory->CreateWicBitmapRenderTarget(
        bitmap.Get(), renderTargetProperties, &renderTarget);
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<IDWriteFactory> writeFactory;
    result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()));
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<IDWriteTextFormat> titleFormat;
    result = writeFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        58.0f, L"en-us", &titleFormat);
    if (FAILED(result))
    {
        return result;
    }
    titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextFormat> bodyFormat;
    result = writeFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        28.0f, L"en-us", &bodyFormat);
    if (FAILED(result))
    {
        return result;
    }
    bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    ComPtr<ID2D1SolidColorBrush> titleBrush;
    result = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.91f, 0.96f, 1.0f, 1.0f), &titleBrush);
    if (FAILED(result))
    {
        return result;
    }
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    result = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.20f, 0.75f, 1.0f, 1.0f), &accentBrush);
    if (FAILED(result))
    {
        return result;
    }
    ComPtr<ID2D1SolidColorBrush> bodyBrush;
    result = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.72f, 0.79f, 0.87f, 1.0f), &bodyBrush);
    if (FAILED(result))
    {
        return result;
    }

    renderTarget->BeginDraw();
    renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(1.0f, 1.0f, 1199.0f, 359.0f), 24.0f, 24.0f),
        accentBrush.Get(), 2.0f);

    constexpr wchar_t title[] = L"Direct3D 12";
    renderTarget->DrawText(
        title, static_cast<UINT32>(_countof(title) - 1),
        titleFormat.Get(), D2D1::RectF(44.0f, 30.0f, 1156.0f, 124.0f),
        titleBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);

    constexpr wchar_t subtitle[] = L"64-bit DirectWrite text rendering demo";
    renderTarget->DrawText(
        subtitle, static_cast<UINT32>(_countof(subtitle) - 1),
        bodyFormat.Get(), D2D1::RectF(48.0f, 147.0f, 1152.0f, 198.0f),
        accentBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);

    constexpr wchar_t instructions[] =
        L"Inject DX12OverlayHook.dll into this window to test the spinning cube.";
    renderTarget->DrawText(
        instructions, static_cast<UINT32>(_countof(instructions) - 1),
        bodyFormat.Get(), D2D1::RectF(48.0f, 218.0f, 1152.0f, 330.0f),
        bodyBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);

    result = renderTarget->EndDraw();
    if (FAILED(result))
    {
        return result;
    }

    return CopyBitmapPixels(bitmap.Get(), output);
}
