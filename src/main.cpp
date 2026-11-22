#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <extractor/extractor.hpp>
#include <pe/pe.hpp>
#include <shims/process.hpp>
#include <shims/shims.hpp>
#include <shims/uwphost.hpp>

namespace bsx::members {
    const char* bindingName(Binding binding) {
        switch (binding) {
            case Binding::Shim: return "shim";
            case Binding::Host: return "host";
            case Binding::Module: return "module";
            default: return "UNIMPLEMENTED";
        }
    }

    const char* moduleNameFor(const void* address, uintptr_t* baseOut) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)) return "<unknown>";
        static wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(module, path, MAX_PATH)) return "<unknown>";
        static char narrow[MAX_PATH];
        const int length = WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info))) *baseOut = reinterpret_cast<uintptr_t>(info.AllocationBase);
        return length > 0 ? narrow : "<unknown>";
    }

    uintptr_t m_gameBase = 0;
    size_t m_gameSize = 0;

    struct ThrowInfoRva {
        unsigned int attributes;
        DWORD unwind;
        DWORD forwardCompat;
        DWORD catchableTypeArray;
    };

    struct CatchableTypeArrayRva {
        DWORD count;
        DWORD types[1];
    };

    struct CatchableTypeRva {
        unsigned int properties;
        DWORD type;
        int displacement[3];
        int sizeOrOffset;
        DWORD copyFunction;
    };

    struct ExceptionTypeDescriptorRva {
        void* vftable;
        void* spare;
        char name[1];
    };

    void reportCppExceptionType(EXCEPTION_POINTERS* info) {
        if (info->ExceptionRecord->NumberParameters < 3) return;
        const uintptr_t throwInfoAddress = info->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base = 0;
        moduleNameFor(reinterpret_cast<const void*>(throwInfoAddress), &base);
        if (!base && m_gameBase && throwInfoAddress >= m_gameBase && throwInfoAddress < m_gameBase + m_gameSize) base = m_gameBase;
        if (!base) return;
        auto* throwInfo = reinterpret_cast<const ThrowInfoRva*>(throwInfoAddress);
        if (!throwInfo->catchableTypeArray) return;
        auto* types = reinterpret_cast<const CatchableTypeArrayRva*>(base + throwInfo->catchableTypeArray);
        for (DWORD t = 0; t < types->count && t < 8; ++t) {
            auto* type = reinterpret_cast<const CatchableTypeRva*>(base + types->types[t]);
            if (!type->type) continue;
            auto* descriptor = reinterpret_cast<ExceptionTypeDescriptorRva const*>(base + type->type);
            std::fprintf(stderr, "[bsx]   exception type %lu: %s\n", static_cast<unsigned long>(t), descriptor->name);
        }
    }

    void reportStack() {
        void* frames[48];
        const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) {
            uintptr_t base = 0;
            const char* module = moduleNameFor(frames[i], &base);
            std::fprintf(stderr, "[bsx]   frame %02u %s+0x%zx\n", i, module, reinterpret_cast<uintptr_t>(frames[i]) - base);
        }
    }

    void crashLogWrite(const char* text) {
        HANDLE file = CreateFileW(L"bsxbox-crash.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        SetFilePointer(file, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        CloseHandle(file);
    }

    void reportStackFile() {
        void* frames[32];
        const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) {
            uintptr_t base = 0;
            const char* module = moduleNameFor(frames[i], &base);
            char line[512];
            std::snprintf(line, sizeof(line), "    frame %02u %s+0x%zx\r\n", i, module, reinterpret_cast<uintptr_t>(frames[i]) - base);
            crashLogWrite(line);
        }
    }

    void reportGameStack(uintptr_t rsp) {
        if (!m_gameBase) return;
        char line[160];
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(rsp), &info, sizeof(info))) return;
        const auto* stack = reinterpret_cast<const uintptr_t*>(info.BaseAddress);
        const auto* top = reinterpret_cast<const uintptr_t*>(rsp);
        for (const auto* p = top; p < stack + info.RegionSize / sizeof(uintptr_t) && p < top + 512; ++p) {
            const uintptr_t value = *p;
            if (value >= m_gameBase && value < m_gameBase + m_gameSize) {
                std::snprintf(line, sizeof(line), "    game frame %s+0x%zx\r\n", "title", value - m_gameBase);
                crashLogWrite(line);
            }
        }
    }

    LONG WINAPI vectorFilter(EXCEPTION_POINTERS* info) {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != 0xE06D7363 && code != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
        char line[256];
        std::snprintf(line, sizeof(line), "[bsx] exception 0x%08lX thread=%lu address=%p\r\n", static_cast<unsigned long>(code), static_cast<unsigned long>(GetCurrentThreadId()), info->ExceptionRecord->ExceptionAddress);
        crashLogWrite(line);
        if (code == 0xE06D7363) reportCppExceptionType(info);
        reportStackFile();
        if (info->ContextRecord) reportGameStack(info->ContextRecord->Rsp);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI continueFilter(EXCEPTION_POINTERS* info) {
        char line[256];
        std::snprintf(line, sizeof(line), "[bsx] unhandled exception 0x%08lX on thread %lu\r\n", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), static_cast<unsigned long>(GetCurrentThreadId()));
        crashLogWrite(line);
        std::fputs(line, stderr);
        if (info->ExceptionRecord->ExceptionCode == 0xE06D7363) reportCppExceptionType(info);
        reportStack();
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
        std::fprintf(stderr, "[bsx] unhandled exception 0x%08lX", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
        if (info->ExceptionRecord->ExceptionCode == 0xC0000005) std::fprintf(stderr, " at %p access=%I64u", info->ExceptionRecord->ExceptionAddress, static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]));
        std::fputc('\n', stderr);
        if (info->ExceptionRecord->ExceptionCode == 0xE06D7363) reportCppExceptionType(info);
        reportStack();
        std::fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    int usage() {
        std::fprintf(stderr, "usage: bsxbox --dry-run <image> [--game-dir DIR] [--host-all-dlls]\n       bsxbox --init <image> [--game-dir DIR] [--host-all-dlls]\n       bsxbox --run <exe> [--game-dir DIR] [--host-all-dlls] [-- game arguments...]\n       <image> can be a PE image or an MSIX package (extracted before use)\n");
        return 2;
    }
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path image;
    std::filesystem::path gameDir;
    bool dryRun = false;
    bool initialize = false;
    bool run = false;
    bool hostAllDlls = false;
    std::vector<std::string> gameArgs;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--") {
            for (++i; i < argc; ++i) gameArgs.push_back(std::filesystem::path(argv[i]).string());
            break;
        }
        if (arg == L"--dry-run") dryRun = true;
        else if (arg == L"--init") initialize = true;
        else if (arg == L"--run") run = true;
        else if (arg == L"--host-all-dlls") hostAllDlls = true;
        else if (arg == L"--game-dir" && i + 1 < argc) gameDir = argv[++i];
        else if (image.empty()) image = arg;
        else return bsx::members::usage();
    }
    if (image.empty()) return bsx::members::usage();
    if (!dryRun && !initialize && !run) return bsx::members::usage();
    const bool hasGameDir = !gameDir.empty();

    SetUnhandledExceptionFilter(bsx::members::crashFilter);
    AddVectoredContinueHandler(1, bsx::members::continueFilter);
    AddVectoredExceptionHandler(1, bsx::members::vectorFilter);

    try {
        if (bsx::extractor::isPackage(image)) {
            const auto destination = hasGameDir ? gameDir : bsx::extractor::cacheDirectory(image);
            std::printf("BSXbox: extracting %s\n", image.filename().string().c_str());
            const auto package = bsx::extractor::extract(image, destination);
            std::printf("BSXbox: extracted %zu file(s) to %s\n", package.fileCount, destination.string().c_str());
            std::printf("BSXbox: package executable = %s\n", package.executable.filename().string().c_str());
            image = package.executable;
            gameDir = package.root;
        }
        
        if (gameDir.empty()) gameDir = image.parent_path();
        bsx::Loader loader(gameDir, hostAllDlls);
        std::printf("BSXbox: loading %s\n", image.filename().string().c_str());
        std::printf("BSXbox: dependency mode = %s\n", hostAllDlls ? "host-all" : "map-bundled");
        bsx::Module& mainModule = loader.load(image);
        bsx::members::m_gameBase = reinterpret_cast<uintptr_t>(mainModule.base);
        bsx::members::m_gameSize = mainModule.size;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(mainModule.base + reinterpret_cast<IMAGE_DOS_HEADER*>(mainModule.base)->e_lfanew);
        auto& pdataDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        auto* pdata = reinterpret_cast<RUNTIME_FUNCTION*>(mainModule.base + pdataDir.VirtualAddress);
        const DWORD pdataCount = pdataDir.Size / sizeof(RUNTIME_FUNCTION);
        if (pdataCount) {
            std::fprintf(stderr, "[bsx] pdata: %lu entries, first=0x%X-0x%X, last=0x%X-0x%X\n", static_cast<unsigned long>(pdataCount), pdata[0].BeginAddress, pdata[0].EndAddress, pdata[pdataCount - 1].BeginAddress, pdata[pdataCount - 1].EndAddress);
            const DWORD probeRvas[3] = {pdata[0].BeginAddress, mainModule.entryRva, pdata[pdataCount - 1].BeginAddress};
            for (DWORD rva : probeRvas) {
                ULONG64 imageBase = 0;
                void* function = RtlLookupFunctionEntry(bsx::members::m_gameBase + rva, &imageBase, nullptr);
                std::fprintf(stderr, "[bsx] unwind lookup +0x%X -> %p (base 0x%llX)\n", rva, function, static_cast<unsigned long long>(imageBase));
            }
        }
        ULONG64 hostBase = 0;
        void* hostFunction = RtlLookupFunctionEntry(reinterpret_cast<uintptr_t>(&bsx::members::reportStack), &hostBase, nullptr);
        std::fprintf(stderr, "[bsx] unwind lookup host fn -> %p (base 0x%llX)\n", hostFunction, static_cast<unsigned long long>(hostBase));
        std::fflush(stderr);
        if (initialize || run) {
            loader.initialize();
            std::printf("%zu module(s) initialised\n", loader.modules().size());
        }
        int unimplemented = 0;
        for (auto* module : loader.modules()) {
            std::map<bsx::Binding, int> counts;
            for (auto& import : module->imports) counts[import.binding]++;
            std::printf("%-36s base=%p%s size=%u entry=+%#x  shim=%d host=%d module=%d unimplemented=%d\n", module->name.c_str(), module->base, module->relocated ? " (relocated)" : "", module->size, module->entryRva, counts[bsx::Binding::Shim], counts[bsx::Binding::Host], counts[bsx::Binding::Module], counts[bsx::Binding::Unimpl]);
            for (auto& import : module->imports) {
                if (import.binding == bsx::Binding::Unimpl || import.binding == bsx::Binding::Shim) {
                    std::printf("    %-13s %s!%s\n", bsx::members::bindingName(import.binding), import.dll.c_str(), import.name.c_str());
                    unimplemented += import.binding == bsx::Binding::Unimpl;
                }
            }
        }
        std::printf("%zu module(s) mapped, %d unimplemented import(s)\n", loader.modules().size(), unimplemented);
        std::fflush(stdout);
        if (!run) return unimplemented ? 3 : 0;
        if (!mainModule.entryRva) {
            std::fprintf(stderr, "error: %s has no entry point\n", mainModule.name.c_str());
            return 1;
        }
        bsx::process::setCommandLine(image.filename().string(), gameArgs);
        const std::wstring installedPath = gameDir.wstring();
        const std::wstring localStatePath = (gameDir / L"LocalState").wstring();
        bsx::uwp::setPaths(installedPath.c_str(), localStatePath.c_str());
        bsx::uwp::installDxgiSwapChainAdapter();
        if (GetEnvironmentVariableA("BSXBOX_SWAP_CHAIN_PROBE", nullptr, 0) > 0) bsx::uwp::swapChainProbe();
        std::printf("running %s\n", mainModule.name.c_str());
        std::fflush(stdout);
        const int code = bsx::process::runEntry(loader, mainModule);
        std::fflush(stdout);
        std::printf("game exited with code %d\n", code);
        bsx::shims::reportcalledunimpl();
        std::fflush(stdout);
        std::fflush(stderr);
        ExitProcess(static_cast<UINT>(code));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
