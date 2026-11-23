#include <windows.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <pe/pe.hpp>
#include <shims/process.hpp>
#include <shims/shims.hpp>

#include <log.hpp>

namespace bsx::process {
    namespace members {
        std::vector<std::string> m_args;
        std::vector<std::wstring> m_wargs;
        std::vector<char*> m_argv;
        std::vector<wchar_t*> m_wargv;
        std::string m_cmdline;
        std::wstring m_wcmdline;
        int m_argc = 0;
        char** m_argvPtr = nullptr;
        wchar_t** m_wargvPtr = nullptr;
        char** m_env = nullptr;
        wchar_t** m_wenv = nullptr;
        std::atomic<bool> m_exited{false};
        std::atomic<int> m_exitCode{0};
        HANDLE m_done = nullptr;
        int (__cdecl* m_sdlMain)(int, char**) = nullptr;

        void runLogWrite(const char* text) {
            HANDLE file = CreateFileW(L"bsxbox-run.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            SetFilePointer(file, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
            CloseHandle(file);
        }

        const char* moduleNameFor(const void* address, uintptr_t* baseOut) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)) {
                return "<unknown>";
            }
            static wchar_t path[MAX_PATH];
            if (!GetModuleFileNameW(module, path, MAX_PATH)) return "<unknown>";
            static char narrow[MAX_PATH];
            const int length = WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info))) {
                *baseOut = reinterpret_cast<uintptr_t>(info.AllocationBase);
            }
            return length > 0 ? narrow : "<unknown>";
        }

        void logStack() {
            void* frames[16];
            const USHORT count = CaptureStackBackTrace(1, 16, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                uintptr_t base = 0;
                const char* module = moduleNameFor(frames[i], &base);
                char line[512];
                std::snprintf(line, sizeof(line), "    frame %02u %s+0x%zx\r\n", i, module, reinterpret_cast<uintptr_t>(frames[i]) - base);
                runLogWrite(line);
            }
        }

        std::string quote(const std::string& value) {
            if (!value.empty() && value.find_first_of(" \t\"") == std::string::npos) return value;
            std::string result = "\"";
            for (char c : value) {
                if (c == '"') result += '\\';
                result += c;
            }
            return result + "\"";
        }

        std::wstring widen(const std::string& value) {
            const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
            std::wstring result(length > 0 ? length - 1 : 0, L'\0');
            if (length > 1) MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
            return result;
        }

        [[noreturn]] void endRun(int code) {
            char line[128];
            std::snprintf(line, sizeof(line), "[bsx] [process]: endRun code=%d thread=%lu\r\n", code, static_cast<unsigned long>(GetCurrentThreadId()));
            runLogWrite(line);
            logStack();
            bool expected = false;
            if (m_exited.compare_exchange_strong(expected, true)) {
                m_exitCode = code;
                if (m_done) SetEvent(m_done);
                ExitThread(static_cast<DWORD>(code));
            }
            ExitProcess(static_cast<UINT>(m_exitCode.load()));
        }

        void __cdecl gameExit(int code) {
            endRun(code);
        }

        void __stdcall gameExitProcess(UINT code) {
            endRun(static_cast<int>(code));
        }

        void __cdecl gameAbort() {
            bsx::logf("process", "the game called abort()");
            endRun(3);
        }

        int* __cdecl gameArgc() {
            return &m_argc;
        }

        char*** __cdecl gameArgv() {
            return &m_argvPtr;
        }

        wchar_t*** __cdecl gameWargv() {
            return &m_wargvPtr;
        }

        int __cdecl gameConfigureArgv(int) {
            return 0;
        }

        LPSTR __stdcall gameGetCommandLineA() {
            return m_cmdline.data();
        }

        LPWSTR __stdcall gameGetCommandLineW() {
            return m_wcmdline.data();
        }

        char** __cdecl gameCommandLineStorageA() {
            static char* value = nullptr;
            value = m_cmdline.data();
            return &value;
        }

        wchar_t** __cdecl gameCommandLineStorageW() {
            static wchar_t* value = nullptr;
            value = m_wcmdline.data();
            return &value;
        }

        int __cdecl gameGetMainArgs(int* argc, char*** argv, char*** env, int, void*) {
            *argc = m_argc;
            *argv = m_argvPtr;
            if (env) *env = m_env;
            return 0;
        }

        int __cdecl gameGetMainArgsW(int* argc, wchar_t*** argv, wchar_t*** env, int, void*) {
            *argc = m_argc;
            *argv = m_wargvPtr;
            if (env) *env = m_wenv;
            return 0;
        }

        struct EntryCall {
            Loader* loader;
            Module* module;
        };

        DWORD WINAPI gameThread(void* parameter) {
            auto* call = static_cast<EntryCall*>(parameter);
            char line[128];
            std::snprintf(line, sizeof(line), "[bsx] [process]: game thread started entry=%p thread=%lu\r\n", static_cast<void*>(call->module->base + call->module->entryRva), static_cast<unsigned long>(GetCurrentThreadId()));
            runLogWrite(line);
            auto entry = reinterpret_cast<DWORD(WINAPI*)(void*)>(call->module->base + call->module->entryRva);
            DWORD code = 0;
            try {
                call->loader->installTlsForAll();
                bsx::runTlsCallbacksAll(DLL_THREAD_ATTACH);
                code = entry(nullptr);
                bsx::runTlsCallbacksAll(DLL_THREAD_DETACH);
                call->loader->uninstallTlsForAll();
            } catch (const winrt::hresult_error& e) {
                const auto message = winrt::to_string(e.message());
                bsx::logf("process", "unhandled WinRT exception 0x%08lX %s", static_cast<unsigned long>(e.code()), message.c_str());
                endRun(static_cast<int>(e.code()));
            } catch (const std::exception& e) {
                bsx::logf("process", "unhandled C++ exception: %s", e.what());
                endRun(-1);
            } catch (...) {
                bsx::logf("process", "unhandled non-standard C++ exception");
                endRun(-1);
            }
            endRun(static_cast<int>(code));
        }

        struct GameThreadStart {
            unsigned (__stdcall* start)(void*);
            void* argument;
        };

        unsigned __stdcall gameThreadStart(void* parameter) {
            auto* call = static_cast<GameThreadStart*>(parameter);
            auto start = call->start;
            auto argument = call->argument;
            delete call;
            bsx::installTlsForCurrentThread();
            bsx::runTlsCallbacksAll(DLL_THREAD_ATTACH);
            const unsigned result = start(argument);
            bsx::runTlsCallbacksAll(DLL_THREAD_DETACH);
            bsx::uninstallTlsForCurrentThread();
            return result;
        }

        uintptr_t __cdecl beginThreadExShim(void* security, unsigned stackSize, unsigned (__stdcall* start)(void*), void* argument, unsigned initFlag, unsigned* threadId) {
            using BeginThreadExFn = uintptr_t (__cdecl*)(void*, unsigned, unsigned (__stdcall*)(void*), void*, unsigned, unsigned*);
            static BeginThreadExFn real = [] {
                HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
                if (!ucrt) ucrt = LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                return ucrt ? reinterpret_cast<BeginThreadExFn>(GetProcAddress(ucrt, "_beginthreadex")) : nullptr;
            }();
            if (!real) return 0;
            auto* call = new GameThreadStart{start, argument};
            const uintptr_t handle = real(security, stackSize, gameThreadStart, call, initFlag, threadId);
            if (!handle) delete call;
            return handle;
        }

        int __cdecl gameSdlMain(int, char**) {
            return m_sdlMain ? m_sdlMain(m_argc, m_argvPtr) : -1;
        }

        int __cdecl shimSdlWinRtRunApp(int (__cdecl* mainFunction)(int, char**), void* reserved) {
            using RunAppFn = int (__cdecl*)(int (__cdecl*)(int, char**), void*);
            static RunAppFn runApp = reinterpret_cast<RunAppFn>(bsx::findMappedExport("SDL2.dll", "SDL_WinRTRunApp"));
            if (!runApp || !mainFunction) return -1;
            m_sdlMain = mainFunction;
            return runApp(gameSdlMain, reserved);
        }
    }

    void setCommandLine(const std::string& program, const std::vector<std::string>& args) {
        using namespace members;
        m_args.clear();
        m_args.push_back(program);
        m_args.insert(m_args.end(), args.begin(), args.end());
        m_wargs.clear();
        m_cmdline.clear();
        for (auto& arg : m_args) {
            m_wargs.push_back(widen(arg));
            if (!m_cmdline.empty()) m_cmdline += ' ';
            m_cmdline += quote(arg);
        }
        m_wcmdline = widen(m_cmdline);
        m_argv.clear();
        m_wargv.clear();
        for (auto& arg : m_args) m_argv.push_back(arg.data());
        for (auto& arg : m_wargs) m_wargv.push_back(arg.data());
        m_argv.push_back(nullptr);
        m_wargv.push_back(nullptr);
        m_argc = static_cast<int>(m_args.size());
        m_argvPtr = m_argv.data();
        m_wargvPtr = m_wargv.data();
        m_env = _environ;
        m_wenv = _wenviron;
        bsx::logf("process", "game command line: %s", m_cmdline.c_str());
    }

    int runEntry(Loader& loader, Module& module, size_t stackBytes) {
        using namespace members;
        m_exited = false;
        m_exitCode = 0;
        m_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        EntryCall call{&loader, &module};
        HANDLE thread = CreateThread(nullptr, stackBytes, gameThread, &call, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!thread) {
            CloseHandle(m_done);
            m_done = nullptr;
            return -1;
        }
        HANDLE waits[] = {m_done, thread};
        WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        CloseHandle(thread);
        CloseHandle(m_done);
        m_done = nullptr;
        return m_exitCode.load();
    }
}

namespace bsx::process {
#define BSX_UCRT(dll) \
    {dll, "exit", reinterpret_cast<void*>(&members::gameExit)}, \
    {dll, "_exit", reinterpret_cast<void*>(&members::gameExit)}, \
    {dll, "_Exit", reinterpret_cast<void*>(&members::gameExit)}, \
    {dll, "quick_exit", reinterpret_cast<void*>(&members::gameExit)}, \
    {dll, "abort", reinterpret_cast<void*>(&members::gameAbort)}, \
    {dll, "__p___argc", reinterpret_cast<void*>(&members::gameArgc)}, \
    {dll, "__p___argv", reinterpret_cast<void*>(&members::gameArgv)}, \
    {dll, "__p___wargv", reinterpret_cast<void*>(&members::gameWargv)}, \
    {dll, "_configure_narrow_argv", reinterpret_cast<void*>(&members::gameConfigureArgv)}, \
    {dll, "_configure_wide_argv", reinterpret_cast<void*>(&members::gameConfigureArgv)}, \
    {dll, "_get_narrow_winmain_command_line", reinterpret_cast<void*>(&members::gameGetCommandLineA)}, \
    {dll, "_get_wide_winmain_command_line", reinterpret_cast<void*>(&members::gameGetCommandLineW)}, \
    {dll, "__p__acmdln", reinterpret_cast<void*>(&members::gameCommandLineStorageA)}, \
    {dll, "__p__wcmdln", reinterpret_cast<void*>(&members::gameCommandLineStorageW)}

    SHIMTABLE(process,
        BSX_UCRT("api-ms-win-crt-runtime-l1-1-0.dll"),
        BSX_UCRT("ucrtbase.dll"),
        {"msvcrt.dll", "exit", reinterpret_cast<void*>(&members::gameExit)},
        {"msvcrt.dll", "_exit", reinterpret_cast<void*>(&members::gameExit)},
        {"msvcrt.dll", "abort", reinterpret_cast<void*>(&members::gameAbort)},
        {"msvcrt.dll", "__getmainargs", reinterpret_cast<void*>(&members::gameGetMainArgs)},
        {"msvcrt.dll", "__wgetmainargs", reinterpret_cast<void*>(&members::gameGetMainArgsW)},
        {"kernel32.dll", "ExitProcess", reinterpret_cast<void*>(&members::gameExitProcess)},
        {"kernel32.dll", "GetCommandLineA", reinterpret_cast<void*>(&members::gameGetCommandLineA)},
        {"kernel32.dll", "GetCommandLineW", reinterpret_cast<void*>(&members::gameGetCommandLineW)},
        {"ucrtbase.dll", "_beginthreadex", reinterpret_cast<void*>(&members::beginThreadExShim)},
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_beginthreadex", reinterpret_cast<void*>(&members::beginThreadExShim)},
        {"msvcrt.dll", "_beginthreadex", reinterpret_cast<void*>(&members::beginThreadExShim)},
        {"sdl2.dll", "SDL_WinRTRunApp", reinterpret_cast<void*>(&members::shimSdlWinRtRunApp)}
    );
#undef BSX_UCRT
}
