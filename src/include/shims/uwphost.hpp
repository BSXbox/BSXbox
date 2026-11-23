#pragma once

#include <windows.h>
#include <unknwn.h>

struct __declspec(novtable) __declspec(uuid("45D64A29-A63E-4CB6-B498-5781D298CB4F")) ICoreWindowInterop : IUnknown {
    virtual HRESULT __stdcall get_WindowHandle(HWND* value) = 0;
    virtual HRESULT __stdcall put_MessageHandled(BOOL value) = 0;
};

namespace bsx::uwp {
    void setPaths(const wchar_t* installedPath, const wchar_t* localStatePath);
    bool getActivationFactory(const wchar_t* classId, REFIID iid, void** out);
    void installDxgiSwapChainAdapter();
    IUnknown* retainCoreWindow();
    void showHostWindow();
    HWND hostWindow();
}
