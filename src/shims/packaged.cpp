#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <log.hpp>
#include <pe/pe.hpp>
#include <shims/shims.hpp>

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

    HMODULE __stdcall shimLoadPackagedLibrary(LPCWSTR libraryName, DWORD reserved) {
        (void)reserved;
        if (!libraryName || !*libraryName) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const std::filesystem::path name(libraryName);
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
