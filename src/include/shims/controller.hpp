#pragma once

#include <windows.h>

namespace bsx::input {
    void initializeControllers();
    void pushKeyboardKey(UINT key, bool down);
    void pushKeyboardText(wchar_t character);
}
