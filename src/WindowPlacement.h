#pragma once
#include <windows.h>

namespace system_info
{
// The work area and requested outer size are in physical desktop pixels.
inline RECT InitialWindowBounds(const RECT& work, SIZE requested = {})
{
    const LONG availableWidth = work.right - work.left;
    const LONG availableHeight = work.bottom - work.top;
    const LONG width = requested.cx > 0 ? requested.cx : MulDiv(availableWidth, 2, 3);
    const LONG height = requested.cy > 0 ? requested.cy : MulDiv(availableHeight, 2, 3);
    const LONG left = work.left + (availableWidth - width) / 2;
    const LONG top = work.top + (availableHeight - height) / 2;
    return {left, top, left + width, top + height};
}
}
