#include <windows.h>
#include <unknwn.h>

struct __declspec(novtable) __declspec(uuid("45D64A29-A63E-4CB6-B498-5781D298CB4F")) ICoreWindowInterop : IUnknown {
    virtual HRESULT __stdcall getWndH(HWND* val) = 0;
    virtual HRESULT __stdcall putMsgH(BOOL val) = 0;
};

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include <roapi.h>
#include <winstring.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdarg>
#include <cstdio>
#include <string>

#include <shims/uwphost.hpp>

namespace bsx::uwp {
    namespace wf = winrt::Windows::Foundation;
    namespace wfc = winrt::Windows::Foundation::Collections;
}