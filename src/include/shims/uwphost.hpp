#pragma once
#include <windows.h>

namespace bsx::uwp {
    void log(const char* format, ...);

    HWND m_hwnd = nullptr;

    struct coreWindowImpl;
    coreWindowImpl* m_coreWindow = nullptr;

    constexpr wchar_t m_windowClassName[] = L"BSXboxUWPHostWindow";
    constexpr int kDefaultWindowWidth = 1280;
    constexpr int kDefaultWindowHeight = 720;

    void closeW();
}