#pragma once

#include <Windows.h>

#include <cstdint>
#include <vector>

struct TextBitmap
{
    UINT width = 0;
    UINT height = 0;
    UINT rowPitch = 0;
    std::vector<std::uint8_t> pixels;
};

HRESULT CreateTextBitmap(TextBitmap& bitmap);
