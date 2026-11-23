#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <log.hpp>
#include <pe/pe.hpp>
#include <shims/shims.hpp>
#include <shims/uwphost.hpp>

namespace bsx::shims::members {
    bool packagedTraceEnabled() {
        static const bool enabled = [] {
            char value[8] = {};
            return GetEnvironmentVariableA("BSXBOX_TRACE_PACKAGED", value, sizeof(value)) > 0;
        }();
        return enabled;
    }

    HMODULE mapBundled(const std::filesystem::path& path) {
        bsx::Loader* loader = bsx::activeLoader();
        if (!loader) return nullptr;
        try {
            bsx::Module& module = loader->loadRuntime(path);
            return reinterpret_cast<HMODULE>(module.base);
        } catch (const std::exception& e) {
            bsx::logf("packaged", "cannot map %ls: %s", path.c_str(), e.what());
            return nullptr;
        }
    }

    HMODULE m_configuredGlModule = nullptr;
    HDC m_configuredGlFakeDc = nullptr;
    HDC m_configuredGlDc = nullptr;

    void wglTrace(const char* format, ...) {
        static const bool enabled = GetEnvironmentVariableA("BSXBOX_TRACE_SDL", nullptr, 0) > 0;
        if (!enabled) return;
        va_list args;
        va_start(args, format);
        char message[512] = {};
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        bsx::logf("wgl", "%s", message);
    }

    HDC configuredGlDc(HDC fake) {
        if (m_configuredGlDc && m_configuredGlFakeDc == fake) return m_configuredGlDc;
        HWND window = reinterpret_cast<HWND>(fake);
        if (!IsWindow(window)) window = bsx::uwp::hostWindow();
        if (!IsWindow(window)) return fake;
        HDC dc = GetDC(window);
        if (dc) {
            m_configuredGlFakeDc = fake;
            m_configuredGlDc = dc;
        }
        return dc ? dc : fake;
    }

    HGLRC WINAPI configuredWglCreateContext(HDC fake) {
        using Fn = HGLRC(WINAPI*)(HDC);
        static Fn fn = reinterpret_cast<Fn>(GetProcAddress(m_configuredGlModule, "wglCreateContext"));
        HDC dc = configuredGlDc(fake);
        HGLRC result = fn ? fn(dc) : nullptr;
        wglTrace("wglCreateContext(fake=%p dc=%p) -> %p error=%lu", fake, dc, result, GetLastError());
        return result;
    }

    BOOL WINAPI configuredWglMakeCurrent(HDC fake, HGLRC context) {
        using Fn = BOOL(WINAPI*)(HDC, HGLRC);
        static Fn fn = reinterpret_cast<Fn>(GetProcAddress(m_configuredGlModule, "wglMakeCurrent"));
        HDC dc = configuredGlDc(fake);
        const BOOL result = fn ? fn(dc, context) : FALSE;
        wglTrace("wglMakeCurrent(fake=%p dc=%p context=%p) -> %d error=%lu", fake, dc, context, result, GetLastError());
        return result;
    }

    BOOL WINAPI configuredWglSwapBuffers(HDC fake) {
        HDC dc = configuredGlDc(fake);
        const BOOL result = SwapBuffers(dc);
        if (!result) wglTrace("SwapBuffers(fake=%p dc=%p) -> %d error=%lu", fake, dc, result, GetLastError());
        return result;
    }

    int WINAPI configuredWglDescribePixelFormat(HDC fake, int format, UINT size, PIXELFORMATDESCRIPTOR* pfd) {
        return DescribePixelFormat(configuredGlDc(fake), format, size, pfd);
    }

    int WINAPI configuredWglChoosePixelFormat(HDC fake, const PIXELFORMATDESCRIPTOR* pfd) {
        const int result = ChoosePixelFormat(configuredGlDc(fake), pfd);
        wglTrace("ChoosePixelFormat(fake=%p) -> %d error=%lu", fake, result, GetLastError());
        return result;
    }

    int WINAPI configuredWglGetPixelFormat(HDC fake) {
        return GetPixelFormat(configuredGlDc(fake));
    }

    BOOL WINAPI configuredWglSetPixelFormat(HDC fake, int format, const PIXELFORMATDESCRIPTOR* pfd) {
        HDC dc = configuredGlDc(fake);
        const int current = GetPixelFormat(dc);
        if (current > 0) {
            wglTrace("SetPixelFormat(fake=%p format=%d) skipped, current=%d", fake, format, current);
            return TRUE;
        }
        const BOOL result = SetPixelFormat(dc, format, pfd);
        wglTrace("SetPixelFormat(fake=%p format=%d) -> %d error=%lu", fake, format, result, GetLastError());
        return result;
    }

    HGLRC WINAPI configuredWglCreateContextAttribs(HDC fake, HGLRC share, const int* attribs) {
        using Fn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
        static Fn fn = [] {
            using GetProc = PROC(WINAPI*)(LPCSTR);
            static GetProc getProc = reinterpret_cast<GetProc>(GetProcAddress(m_configuredGlModule, "wglGetProcAddress"));
            return getProc ? reinterpret_cast<Fn>(getProc("wglCreateContextAttribsARB")) : nullptr;
        }();
        HDC dc = configuredGlDc(fake);
        HGLRC result = fn ? fn(dc, share, attribs) : nullptr;
        wglTrace("wglCreateContextAttribsARB(fake=%p dc=%p share=%p) -> %p error=%lu", fake, dc, share, result, GetLastError());
        return result;
    }

    BOOL WINAPI configuredWglChoosePixelFormatAttribs(HDC fake, const int* ints, const FLOAT* floats, UINT max, int* formats, UINT* count) {
        using Fn = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
        static Fn fn = [] {
            using GetProc = PROC(WINAPI*)(LPCSTR);
            static GetProc getProc = reinterpret_cast<GetProc>(GetProcAddress(m_configuredGlModule, "wglGetProcAddress"));
            return getProc ? reinterpret_cast<Fn>(getProc("wglChoosePixelFormatARB")) : nullptr;
        }();
        return fn ? fn(configuredGlDc(fake), ints, floats, max, formats, count) : FALSE;
    }

    BOOL WINAPI configuredWglGetPixelFormatAttribs(HDC fake, int format, int layer, UINT count, const int* attribs, int* values) {
        using Fn = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
        static Fn fn = [] {
            using GetProc = PROC(WINAPI*)(LPCSTR);
            static GetProc getProc = reinterpret_cast<GetProc>(GetProcAddress(m_configuredGlModule, "wglGetProcAddress"));
            return getProc ? reinterpret_cast<Fn>(getProc("wglGetPixelFormatAttribivARB")) : nullptr;
        }();
        return fn ? fn(configuredGlDc(fake), format, layer, count, attribs, values) : FALSE;
    }

    const char* WINAPI configuredWglGetExtensionsString(HDC fake) {
        using Fn = const char*(WINAPI*)(HDC);
        static Fn fn = [] {
            using GetProc = PROC(WINAPI*)(LPCSTR);
            static GetProc getProc = reinterpret_cast<GetProc>(GetProcAddress(m_configuredGlModule, "wglGetProcAddress"));
            return getProc ? reinterpret_cast<Fn>(getProc("wglGetExtensionsStringARB")) : nullptr;
        }();
        return fn ? fn(configuredGlDc(fake)) : nullptr;
    }

    void* configuredWglOverride(const char* name) {
        if (!name) return nullptr;
        if (std::strcmp(name, "wglCreateContext") == 0) return reinterpret_cast<void*>(&configuredWglCreateContext);
        if (std::strcmp(name, "wglMakeCurrent") == 0) return reinterpret_cast<void*>(&configuredWglMakeCurrent);
        if (std::strcmp(name, "wglSwapBuffers") == 0) return reinterpret_cast<void*>(&configuredWglSwapBuffers);
        if (std::strcmp(name, "wglDescribePixelFormat") == 0) return reinterpret_cast<void*>(&configuredWglDescribePixelFormat);
        if (std::strcmp(name, "wglChoosePixelFormat") == 0) return reinterpret_cast<void*>(&configuredWglChoosePixelFormat);
        if (std::strcmp(name, "wglGetPixelFormat") == 0) return reinterpret_cast<void*>(&configuredWglGetPixelFormat);
        if (std::strcmp(name, "wglSetPixelFormat") == 0) return reinterpret_cast<void*>(&configuredWglSetPixelFormat);
        if (std::strcmp(name, "wglCreateContextAttribsARB") == 0) return reinterpret_cast<void*>(&configuredWglCreateContextAttribs);
        if (std::strcmp(name, "wglChoosePixelFormatARB") == 0) return reinterpret_cast<void*>(&configuredWglChoosePixelFormatAttribs);
        if (std::strcmp(name, "wglGetPixelFormatAttribivARB") == 0) return reinterpret_cast<void*>(&configuredWglGetPixelFormatAttribs);
        if (std::strcmp(name, "wglGetExtensionsStringARB") == 0) return reinterpret_cast<void*>(&configuredWglGetExtensionsString);
        return nullptr;
    }

    PROC WINAPI configuredWglGetProcAddress(LPCSTR name) {
        if (void* override = configuredWglOverride(name)) return reinterpret_cast<PROC>(override);
        using Fn = PROC(WINAPI*)(LPCSTR);
        static Fn fn = reinterpret_cast<Fn>(GetProcAddress(m_configuredGlModule, "wglGetProcAddress"));
        return fn ? fn(name) : nullptr;
    }

    bool systemGlEnabled() {
        return bsx::systemOpenGL();
    }

    HMODULE systemGlModule(const std::filesystem::path& name) {
        const std::string file = name.filename().string();
        if (_stricmp(file.c_str(), "opengl32.dll") != 0) return nullptr;
        wchar_t overridePath[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(L"BSXBOX_OPENGL32", overridePath, MAX_PATH);
        HMODULE module = nullptr;
        if (length > 0 && length < MAX_PATH) module = LoadLibraryExW(overridePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        else if (systemGlEnabled()) module = LoadLibraryExW(L"opengl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            m_configuredGlModule = module;
            m_configuredGlFakeDc = nullptr;
            m_configuredGlDc = nullptr;
        }
        return module;
    }

    HMODULE __stdcall shimLoadPackagedLibrary(LPCWSTR libraryName, DWORD reserved) {
        (void)reserved;
        if (!libraryName || !*libraryName) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const std::filesystem::path name(libraryName);
        if (HMODULE gl = systemGlModule(name)) {
            bsx::logf("packaged", "LoadPackagedLibrary('%ls') -> configured OpenGL %p", libraryName, gl);
            return gl;
        }
        HMODULE module = nullptr;
        if (name.has_parent_path() || name.is_absolute()) {
            module = mapBundled(name);
            if (!module) module = LoadLibraryExW(libraryName, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        } else {
            const std::filesystem::path& directory = bsx::gameDirectory();
            if (!directory.empty()) {
                std::error_code ec;
                const std::filesystem::path local = directory / name;
                if (std::filesystem::is_regular_file(local, ec)) {
                    module = mapBundled(local);
                    if (!module) module = LoadLibraryExW(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                }
            }
            if (!module) module = LoadLibraryExW(libraryName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (packagedTraceEnabled()) {
            wchar_t resolved[MAX_PATH] = {};
            if (module) GetModuleFileNameW(module, resolved, MAX_PATH);
            bsx::logf("packaged", "LoadPackagedLibrary('%ls') -> %p %ls", libraryName, module, resolved);
        }
        return module;
    }

    FARPROC __stdcall shimGetProcAddress(HMODULE module, LPCSTR name) {
        if (module && module == m_configuredGlModule && reinterpret_cast<uintptr_t>(name) > 0xFFFF) {
            if (std::strcmp(name, "wglGetProcAddress") == 0) return reinterpret_cast<FARPROC>(&configuredWglGetProcAddress);
            if (void* override = configuredWglOverride(name)) return reinterpret_cast<FARPROC>(override);
        }
        void* function = nullptr;
        if (module) {
            if (void* found = bsx::findMappedModuleExport(module, name)) function = found;
            else if (reinterpret_cast<uintptr_t>(name) <= 0xFFFF) {
                char key[16] = {};
                std::snprintf(key, sizeof(key), "#%u", static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)));
                function = bsx::findMappedModuleExport(module, key);
            }
        }
        FARPROC result = function ? reinterpret_cast<FARPROC>(function) : GetProcAddress(module, name);
        if (packagedTraceEnabled() && bsx::findMappedModule(module)) {
            bsx::logf("packaged", "GetProcAddress(%s) -> %p", name, reinterpret_cast<void*>(result));
        }
        return result;
    }

    BOOL __stdcall shimFreeLibrary(HMODULE module) {
        if (bsx::findMappedModule(module)) return TRUE;
        return FreeLibrary(module);
    }
}

namespace bsx::shims {
    SHIMTABLE(packaged,
        {"api-ms-win-core-libraryloader-l2-1-0.dll", "LoadPackagedLibrary", reinterpret_cast<void*>(&members::shimLoadPackagedLibrary)},
        {"api-ms-win-core-libraryloader-l1-2-0.dll", "LoadPackagedLibrary", reinterpret_cast<void*>(&members::shimLoadPackagedLibrary)},
        {"api-ms-win-core-libraryloader-l1-2-1.dll", "LoadPackagedLibrary", reinterpret_cast<void*>(&members::shimLoadPackagedLibrary)},
        {"kernel32.dll", "LoadPackagedLibrary", reinterpret_cast<void*>(&members::shimLoadPackagedLibrary)},
        {"kernelbase.dll", "LoadPackagedLibrary", reinterpret_cast<void*>(&members::shimLoadPackagedLibrary)},

        {"api-ms-win-core-libraryloader-l2-1-0.dll", "GetProcAddress", reinterpret_cast<void*>(&members::shimGetProcAddress)},
        {"api-ms-win-core-libraryloader-l1-2-0.dll", "GetProcAddress", reinterpret_cast<void*>(&members::shimGetProcAddress)},
        {"api-ms-win-core-libraryloader-l1-2-1.dll", "GetProcAddress", reinterpret_cast<void*>(&members::shimGetProcAddress)},
        {"kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&members::shimGetProcAddress)},
        {"kernelbase.dll", "GetProcAddress", reinterpret_cast<void*>(&members::shimGetProcAddress)},

        {"api-ms-win-core-libraryloader-l2-1-0.dll", "FreeLibrary", reinterpret_cast<void*>(&members::shimFreeLibrary)},
        {"api-ms-win-core-libraryloader-l1-2-0.dll", "FreeLibrary", reinterpret_cast<void*>(&members::shimFreeLibrary)},
        {"api-ms-win-core-libraryloader-l1-2-1.dll", "FreeLibrary", reinterpret_cast<void*>(&members::shimFreeLibrary)},
        {"kernel32.dll", "FreeLibrary", reinterpret_cast<void*>(&members::shimFreeLibrary)},
        {"kernelbase.dll", "FreeLibrary", reinterpret_cast<void*>(&members::shimFreeLibrary)}
    );
}
