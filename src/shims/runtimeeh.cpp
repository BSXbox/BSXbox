#include <windows.h>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <stdexcept>

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

    void* __cdecl runtimeProc(const wchar_t* module, const char* name) {
        HMODULE handle = GetModuleHandleW(module);
        if (!handle) handle = LoadLibraryExW(module, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return handle ? reinterpret_cast<void*>(GetProcAddress(handle, name)) : nullptr;
    }

    void* runtimeProc(const wchar_t* const* modules, size_t count, const char* name) {
        for (size_t i = 0; i < count; ++i) {
            if (void* proc = runtimeProc(modules[i], name)) return proc;
        }
        return nullptr;
    }

    void* runtimeVcruntimeProc(const char* name) {
        const wchar_t* modules[] = {L"vcruntime140.dll", L"vcruntime140_1.dll"};
        return runtimeProc(modules, 2, name);
    }

    void* runtimeMsvcpProc(const char* name) {
        const wchar_t* modules[] = {L"msvcp140.dll", L"msvcp140_app.dll"};
        return runtimeProc(modules, 2, name);
    }

    void* runtimeMsvcpProc(const char* name, void* fallback) {
        if (void* proc = runtimeMsvcpProc(name)) return proc;
        return fallback;
    }

    void* runtimeCxxThrowException() {
        return runtimeVcruntimeProc("_CxxThrowException");
    }

    void* runtimeCSpecificHandler() {
        return runtimeVcruntimeProc("__C_specific_handler");
    }

    void* runtimeCxxFrameHandler4() {
        if (void* proc = runtimeProc(L"vcruntime140_1.dll", "__CxxFrameHandler4")) return proc;
        return runtimeProc(L"vcruntime140.dll", "__CxxFrameHandler4");
    }

    void* __cdecl shimMemcpy(void* destination, const void* source, size_t count) {
        return std::memcpy(destination, source, count);
    }

    void* __cdecl shimMemset(void* destination, int value, size_t count) {
        return std::memset(destination, value, count);
    }

    void* __cdecl shimMemchr(const void* text, int value, size_t count) {
        return const_cast<void*>(std::memchr(text, value, count));
    }

    int __cdecl shimMemcmp(const void* left, const void* right, size_t count) {
        return std::memcmp(left, right, count);
    }

    wchar_t* __cdecl shimWcsrchr(const wchar_t* text, wchar_t character) {
        return const_cast<wchar_t*>(std::wcsrchr(text, character));
    }

    [[noreturn]] void __cdecl shimXlengthError(const char* message) {
        throw std::length_error(message ? message : "length_error");
    }

    [[noreturn]] void __cdecl shimXinvalidArgument(const char* message) {
        throw std::invalid_argument(message ? message : "invalid_argument");
    }

    [[noreturn]] void __cdecl shimXoutOfRange(const char* message) {
        throw std::out_of_range(message ? message : "out_of_range");
    }
}

namespace bsx::shims {
    SHIMTABLE(runtimeEh,
        {"kernel32.dll", "RtlPcToFileHeader", reinterpret_cast<void*>(&members::shimRtlPcToFileHeader)},
        {"api-ms-win-core-rtlsupport-l1-1-0.dll", "RtlPcToFileHeader", reinterpret_cast<void*>(&members::shimRtlPcToFileHeader)},
        {"vcruntime140_app.dll", "memcpy", reinterpret_cast<void*>(&members::shimMemcpy)},
        {"vcruntime140_app.dll", "memset", reinterpret_cast<void*>(&members::shimMemset)},
        {"vcruntime140_app.dll", "memchr", reinterpret_cast<void*>(&members::shimMemchr)},
        {"vcruntime140_app.dll", "memcmp", reinterpret_cast<void*>(&members::shimMemcmp)},
        {"vcruntime140_app.dll", "wcsrchr", reinterpret_cast<void*>(&members::shimWcsrchr)},
        {"vcruntime140_app.dll", "_CxxThrowException", reinterpret_cast<void*>(members::runtimeCxxThrowException())},
        {"vcruntime140_app.dll", "__C_specific_handler", reinterpret_cast<void*>(members::runtimeCSpecificHandler())},
        {"vcruntime140_app.dll", "_CxxFrameHandler4", reinterpret_cast<void*>(members::runtimeCxxFrameHandler4())},
        {"vcruntime140_app.dll", "__CxxFrameHandler4", reinterpret_cast<void*>(members::runtimeCxxFrameHandler4())},
        {"vcruntime140_1_app.dll", "_CxxFrameHandler4", reinterpret_cast<void*>(members::runtimeCxxFrameHandler4())},
        {"vcruntime140_1_app.dll", "__CxxFrameHandler4", reinterpret_cast<void*>(members::runtimeCxxFrameHandler4())},
        {"vcruntime140.dll", "memcpy", reinterpret_cast<void*>(&members::shimMemcpy)},
        {"vcruntime140.dll", "memset", reinterpret_cast<void*>(&members::shimMemset)},
        {"vcruntime140.dll", "memchr", reinterpret_cast<void*>(&members::shimMemchr)},
        {"vcruntime140.dll", "memcmp", reinterpret_cast<void*>(&members::shimMemcmp)},
        {"vcruntime140.dll", "_CxxThrowException", reinterpret_cast<void*>(members::runtimeCxxThrowException())},
        {"vcruntime140.dll", "__C_specific_handler", reinterpret_cast<void*>(members::runtimeCSpecificHandler())},
        {"ucrtbase.dll", "wcsrchr", reinterpret_cast<void*>(&members::shimWcsrchr)},
        {"msvcp140_app.dll", "_Xlength_error", reinterpret_cast<void*>(members::runtimeMsvcpProc("_Xlength_error", reinterpret_cast<void*>(&members::shimXlengthError)))},
        {"msvcp140_app.dll", "_Xinvalid_argument", reinterpret_cast<void*>(members::runtimeMsvcpProc("_Xinvalid_argument", reinterpret_cast<void*>(&members::shimXinvalidArgument)))},
        {"msvcp140_app.dll", "_Xout_of_range", reinterpret_cast<void*>(members::runtimeMsvcpProc("_Xout_of_range", reinterpret_cast<void*>(&members::shimXoutOfRange)))},
        {"msvcp140_app.dll", "?_Xlength_error@std@@YAXPEBD@Z", reinterpret_cast<void*>(members::runtimeMsvcpProc("?_Xlength_error@std@@YAXPEBD@Z", reinterpret_cast<void*>(&members::shimXlengthError)))},
        {"msvcp140_app.dll", "?_Xinvalid_argument@std@@YAXPEBD@Z", reinterpret_cast<void*>(members::runtimeMsvcpProc("?_Xinvalid_argument@std@@YAXPEBD@Z", reinterpret_cast<void*>(&members::shimXinvalidArgument)))},
        {"msvcp140_app.dll", "?_Xout_of_range@std@@YAXPEBD@Z", reinterpret_cast<void*>(members::runtimeMsvcpProc("?_Xout_of_range@std@@YAXPEBD@Z", reinterpret_cast<void*>(&members::shimXoutOfRange)))},
        {"msvcp140.dll", "_Xlength_error", reinterpret_cast<void*>(members::runtimeMsvcpProc("_Xlength_error"))},
        {"msvcp140.dll", "_Xinvalid_argument", reinterpret_cast<void*>(members::runtimeMsvcpProc("_Xinvalid_argument"))},
        {"msvcp140.dll", "_Xout_of_range", reinterpret_cast<void*>(members::runtimeMsvcpProc("_Xout_of_range"))},
        {"msvcp140.dll", "?_Xlength_error@std@@YAXPEBD@Z", reinterpret_cast<void*>(members::runtimeMsvcpProc("?_Xlength_error@std@@YAXPEBD@Z"))},
        {"msvcp140.dll", "?_Xinvalid_argument@std@@YAXPEBD@Z", reinterpret_cast<void*>(members::runtimeMsvcpProc("?_Xinvalid_argument@std@@YAXPEBD@Z"))},
        {"msvcp140.dll", "?_Xout_of_range@std@@YAXPEBD@Z", reinterpret_cast<void*>(members::runtimeMsvcpProc("?_Xout_of_range@std@@YAXPEBD@Z"))},
        {"vcruntime140_app.dll", "__std_type_info_destroy_list", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_type_info_destroy_list"))},
        {"vcruntime140_app.dll", "__std_exception_copy", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_exception_copy"))},
        {"vcruntime140_app.dll", "__std_exception_destroy", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_exception_destroy"))},
        {"vcruntime140_app.dll", "__std_terminate", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_terminate"))},
        {"vcruntime140.dll", "__std_type_info_destroy_list", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_type_info_destroy_list"))},
        {"vcruntime140.dll", "__std_exception_copy", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_exception_copy"))},
        {"vcruntime140.dll", "__std_exception_destroy", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_exception_destroy"))},
        {"vcruntime140.dll", "__std_terminate", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__std_terminate"))},
        {"vcruntime140.dll", "_CxxFrameHandler4", reinterpret_cast<void*>(members::runtimeVcruntimeProc("__CxxFrameHandler4"))}
    );
}
