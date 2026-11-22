#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include <atomic>

#include <shims/shims.hpp>
#include <shims/uwphost.hpp>

namespace bsx::vccomponent {
    namespace members {
        std::atomic<bool> m_activated{false};
    }

    HRESULT __stdcall shimDllGetActivationFactory(HSTRING classId, REFIID iid, void** factory) {
        if (!classId || !factory) return E_INVALIDARG;
        *factory = nullptr;

        UINT32 length = 0;
        const wchar_t* name = WindowsGetStringRawBuffer(classId, &length);
        if (!name || !length) return E_INVALIDARG;

        if (uwp::getActivationFactory(name, iid, factory)) {
            members::m_activated.store(true);
            return S_OK;
        }

        return REGDB_E_CLASSNOTREG;
    }

    HRESULT __stdcall shimDllCanUnloadNow() {
        return members::m_activated.load() ? S_FALSE : S_OK;
    }
}

namespace bsx::shims {
    SHIMTABLE(gameAssembly,
        {"gameassembly.dll", "DllGetActivationFactory", reinterpret_cast<void*>(&bsx::vccomponent::shimDllGetActivationFactory)},
        {"gameassembly.dll", "DllCanUnloadNow", reinterpret_cast<void*>(&bsx::vccomponent::shimDllCanUnloadNow)}
    );
}
