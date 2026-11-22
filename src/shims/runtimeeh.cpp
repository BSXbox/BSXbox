#include <windows.h>

#include <pe/pe.hpp>
#include <shims/shims.hpp>

namespace bsx::shims::members {
    using RtlPcToFileHeaderFn = PVOID(WINAPI*)(PVOID, PVOID*);

    RtlPcToFileHeaderFn realRtlPcToFileHeader() {
        static RtlPcToFileHeaderFn function = [] {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll ? reinterpret_cast<RtlPcToFileHeaderFn>(GetProcAddress(ntdll, "RtlPcToFileHeader")) : nullptr;
        }();
        return function;
    }

    PVOID WINAPI shimRtlPcToFileHeader(PVOID controlPc, PVOID* base) {
        if (void* mapped = mappedImageFor(controlPc, reinterpret_cast<void**>(base))) return mapped;
        auto real = realRtlPcToFileHeader();
        return real ? real(controlPc, base) : nullptr;
    }
}

namespace bsx::shims {
    SHIMTABLE(runtimeEh,
        {"kernel32.dll", "RtlPcToFileHeader", reinterpret_cast<void*>(&members::shimRtlPcToFileHeader)},
        {"api-ms-win-core-rtlsupport-l1-1-0.dll", "RtlPcToFileHeader", reinterpret_cast<void*>(&members::shimRtlPcToFileHeader)}
    );
}
