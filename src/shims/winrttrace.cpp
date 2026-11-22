#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <shims/shims.hpp>
#include <shims/uwphost.hpp>

namespace bsx::shims::members {
    using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);

    RoGetActivationFactoryFn realRoGetActivationFactory() {
        static RoGetActivationFactoryFn function = [] {
            HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return combase ? reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory")) : nullptr;
        }();
        return function;
    }

    bool traceEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("BSXBOX_TRACE_WINRT");
            return value && *value && *value != '0';
        }();
        return enabled;
    }

    HRESULT traceActivationFactory(HSTRING id, REFIID iid, void** factory) {
        UINT32 length = 0;
        const wchar_t* name = id ? WindowsGetStringRawBuffer(id, &length) : L"<null>";
        if (uwp::getActivationFactory(name, iid, factory)) {
            if (traceEnabled()) {
                wchar_t guid[40] = {};
                StringFromGUID2(iid, guid, 40);
                std::fprintf(stderr, "[bsx] host factory %.*ls %ls\n", static_cast<int>(length), name, guid);
            }
            return S_OK;
        }
        auto real = realRoGetActivationFactory();
        if (!real) return REGDB_E_CLASSNOTREG;
        const HRESULT result = real(id, iid, factory);
        if (traceEnabled()) {
            wchar_t guid[40] = {};
            StringFromGUID2(iid, guid, 40);
            std::fprintf(stderr, "[bsx] RoGetActivationFactory %.*ls %ls -> 0x%08lX\n", static_cast<int>(length), name, guid, static_cast<unsigned long>(result));
        }
        return result;
    }

    HRESULT WINAPI traceRoGetActivationFactory(HSTRING id, REFIID iid, void** factory) {
        return traceActivationFactory(id, iid, factory);
    }

    HRESULT __cdecl traceGetActivationFactoryByPCWSTR(const wchar_t* name, GUID& iid, void** factory) {
        if (!name) return E_POINTER;
        HSTRING id = nullptr;
        const HRESULT createResult = WindowsCreateString(name, static_cast<UINT32>(std::wcslen(name)), &id);
        if (FAILED(createResult)) return createResult;
        const HRESULT result = traceActivationFactory(id, iid, factory);
        WindowsDeleteString(id);
        return result;
    }
}

namespace bsx::shims {
    SHIMTABLE(winrtTrace,
        {"api-ms-win-core-winrt-l1-1-0.dll", "RoGetActivationFactory", reinterpret_cast<void*>(&members::traceRoGetActivationFactory)},
        {"combase.dll", "RoGetActivationFactory", reinterpret_cast<void*>(&members::traceRoGetActivationFactory)},
        {"vccorlib140_app.dll", "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z", reinterpret_cast<void*>(&members::traceGetActivationFactoryByPCWSTR)},
        {"vccorlib140.dll", "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z", reinterpret_cast<void*>(&members::traceGetActivationFactoryByPCWSTR)}
    );
}
