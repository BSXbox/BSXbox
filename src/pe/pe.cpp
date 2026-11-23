#include <pe/pe.hpp>
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <shims/shims.hpp>
#include <log.hpp>

namespace bsx {
    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    namespace members {
        constexpr DWORD maxStaticTlsIndex = 4096;
        constexpr size_t tlsProviderSlots = 8;
        void** moduleTlsSlots() { return reinterpret_cast<void**>(__readgsqword(0x58)); }

        struct TlsProvider {
            HMODULE module = nullptr;
            DWORD index = TLS_OUT_OF_INDEXES;
            uint32_t capacity = 0;
            bool assigned = false;
        };

        std::vector<TlsProvider> tlsProviders;
        size_t nextTlsProvider = 0;
        std::mutex tlsProviderMutex;

        std::vector<Module*> m_mappedImages;
        std::mutex m_mappedImagesMutex;
        std::filesystem::path m_gameDir;
        Loader* m_activeLoader = nullptr;
    }

    namespace helpers {
        std::vector<uint8_t> readFile(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("couldnt open: " + path.string());
            return {std::istreambuf_iterator<char>(file), {}};
        }

        template <class T>
        T* at(uint8_t* base, uint32_t rva) {
            return reinterpret_cast<T*>(base + rva);
        }

        void requireRange(const std::string& name, uint64_t rva, uint64_t len, uint32_t imageSize, const char* what) {
            if (rva > imageSize || len > imageSize - rva) throw std::runtime_error(name + ": " + what + " runs past the img");
        }

        std::string readString(const std::string& name, uint8_t* base, uint32_t imageSize, uint32_t rva, const char* what) {
            if (rva >= imageSize) throw std::runtime_error(name + ": " + what + " is outside of the img");
            auto s = reinterpret_cast<const char*>(base + rva);
            auto end = static_cast<const char*>(std::memchr(s, 0, imageSize - rva));
            if (!end) throw std::runtime_error(name + ": " + what + " isnt terminated inside img");
            return {s, end};
        }

        uint64_t rvaIn(const Module& module, uint64_t va, const char* what) {
            const uint64_t base = reinterpret_cast<uint64_t>(module.base);
            if (va < base || va - base >= module.size) throw std::runtime_error(module.name + ": " + what + " is outside the img");
            return va - base;
        }

        IMAGE_TLS_DIRECTORY64* tlsDirectory(Module& module) {
            auto nt = at<IMAGE_NT_HEADERS64>(module.base, reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            if (!dir.Size || !dir.VirtualAddress) return nullptr;
            requireRange(module.name, dir.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64), module.size, "the TLS directory");
            return at<IMAGE_TLS_DIRECTORY64>(module.base, dir.VirtualAddress);
        }

        bool loadTlsProvider(size_t slot, members::TlsProvider& provider) {
            wchar_t exe[MAX_PATH] = {};
            if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
            wchar_t file[64] = {};
            if (swprintf_s(file, L"bsxtls%zu.dll", slot) < 0) return false;
            const std::filesystem::path path = std::filesystem::path(exe).parent_path() / file;
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) return false;
            auto base = reinterpret_cast<uint8_t*>(module);
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                FreeLibrary(module);
                return false;
            }
            auto nt = at<IMAGE_NT_HEADERS64>(base, dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) {
                FreeLibrary(module);
                return false;
            }
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            if (!dir.Size || !dir.VirtualAddress) {
                FreeLibrary(module);
                return false;
            }
            auto tls = at<IMAGE_TLS_DIRECTORY64>(base, dir.VirtualAddress);
            if (!tls->AddressOfIndex || tls->EndAddressOfRawData < tls->StartAddressOfRawData) {
                FreeLibrary(module);
                return false;
            }
            provider.module = module;
            provider.index = *reinterpret_cast<DWORD*>(tls->AddressOfIndex);
            provider.capacity = static_cast<uint32_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData + tls->SizeOfZeroFill);
            provider.assigned = false;
            return true;
        }
        
        bool acquireTlsProvider(uint32_t required, DWORD& index, uint32_t& capacity) {
            std::lock_guard lock(members::tlsProviderMutex);
            for (auto& provider : members::tlsProviders) {
                if (!provider.assigned && provider.capacity >= required) {
                    provider.assigned = true;
                    index = provider.index;
                    capacity = provider.capacity;
                    return true;
                }
            }
            while (members::nextTlsProvider < members::tlsProviderSlots) {
                const size_t slot = members::nextTlsProvider++;
                members::TlsProvider provider;
                if (!loadTlsProvider(slot, provider)) continue;
                members::tlsProviders.push_back(provider);
                if (provider.capacity >= required) {
                    members::tlsProviders.back().assigned = true;
                    index = provider.index;
                    capacity = provider.capacity;
                    return true;
                }
            }
            return false;
        }

        bool preferHostCopy(const std::string& dll) {
            static const char* names[] = {
                "vcruntime140.dll", "vcruntime140_app.dll", "vcruntime140_1.dll", "vcruntime140_1_app.dll",
                "msvcp140.dll", "msvcp140_app.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "concrt140.dll",
                "vccorlib140.dll", "vccorlib140_app.dll", "vcomp140.dll"
            };
            for (auto name : names) {
                if (dll == name) return true;
            }
            return false;
        }

        std::string hostRuntimeName(const std::string& dll) {
            if (lower(dll) == "vcruntime140_app.dll") return "vcruntime140.dll";
            if (lower(dll) == "vcruntime140_1_app.dll") return "vcruntime140_1.dll";
            if (lower(dll) == "msvcp140_app.dll") return "msvcp140.dll";
            if (lower(dll) == "vccorlib140_app.dll") return "vccorlib140.dll";
            return lower(dll);
        }

        HMODULE loadHostModule(const std::filesystem::path& gameDir, const std::string& dll) {
            const std::string hostDll = hostRuntimeName(dll);
            std::error_code ec;
            const std::filesystem::path bundled = gameDir / hostDll;
            if (std::filesystem::is_regular_file(bundled, ec)) {
                if (HMODULE module = LoadLibraryExW(bundled.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return module;
            }
            return LoadLibraryExA(hostDll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }

    using namespace helpers;

    Loader::~Loader() {
        {
            std::lock_guard lock(members::m_mappedImagesMutex);
            members::m_mappedImages.clear();
        }
        for (auto module : m_order) {
            module->tlsIndex = noTlsIndex;
            if (module->functionTable) RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(module->functionTable));
            if (module->base) VirtualFree(module->base, 0, MEM_RELEASE);
            delete module;
        }
    }

    Module& Loader::load(const std::filesystem::path& image) {
        const std::string key = lower(image.filename().string());
        if (auto it = m_byName.find(key); it != m_byName.end()) return *it->second;
        Module& module = mapImage(image);
        bindImports(module);
        protectSections(module);
        registerUnwind(module);
        m_initOrder.push_back(&module);
        return module;
    }

    Module& Loader::loadRuntime(const std::filesystem::path& image) {
        Module& module = load(image);
        for (size_t i = 0; i < m_initOrder.size(); ++i) {
            Module* pending = m_initOrder[i];
            if (pending->initialized) continue;
            installTlsForThread(*pending);
            initialize(*pending);
        }
        if (std::getenv("BSXBOX_TRACE_PACKAGED")) {
            int unimplemented = 0;
            for (auto& import : module.imports) unimplemented += import.binding == Binding::Unimpl;
            bsx::logf("loader", "loaded %s (base %p, size %u, imports %zu, unimplemented %d)",
                module.name.c_str(), module.base, module.size, module.imports.size(), unimplemented);
            for (auto& import : module.imports) {
                if (import.binding == Binding::Unimpl) bsx::logf("loader", "  missing %s!%s", import.dll.c_str(), import.name.c_str());
            }
        }
        return module;
    }

    void Loader::registerUnwind(Module& module) {
        auto nt = at<IMAGE_NT_HEADERS64>(module.base, reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
        if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return;
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        const uint32_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
        if (!count) return;
        requireRange(module.name, dir.VirtualAddress, static_cast<uint64_t>(count) * sizeof(RUNTIME_FUNCTION), module.size, "the exception directory");
        auto table = at<RUNTIME_FUNCTION>(module.base, dir.VirtualAddress);
        auto reject = [&](uint32_t i, const char* why) { throw std::runtime_error(module.name + ": unwind entry " + std::to_string(i) + " " + why); };
        uint32_t previousEnd = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const RUNTIME_FUNCTION& function = table[i];
            if (function.EndAddress < function.BeginAddress || function.EndAddress > module.size) reject(i, "covers code outside the img");
            if (function.BeginAddress < previousEnd) reject(i, "is out of order or overlaps the one before it");
            const uint32_t info = function.UnwindData & ~1u;
            if (info > module.size || module.size - info < 4) reject(i, "has unwind data outside the img");
            previousEnd = function.EndAddress;
        }
        if (!RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(module.base))) throw std::runtime_error(module.name + ": RtlAddFunctionTable failed");
        module.functionTable = table;
        module.functionCount = count;
    }

    void Loader::initialize() {
        for (auto module : m_initOrder) initialize(*module);
    }

    void Loader::initialize(Module& module) {
        if (module.initialized) return;
        runTlsCallbacks(module, DLL_PROCESS_ATTACH);
        auto nt = at<IMAGE_NT_HEADERS64>(module.base, reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
        if ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && module.entryRva) {
            if (module.entryRva >= module.size) throw std::runtime_error(module.name + ": the entry point is outside the img");
            auto entry = reinterpret_cast<BOOL(NTAPI*)(void*, DWORD, void*)>(module.base + module.entryRva);
            if (!entry(module.base, DLL_PROCESS_ATTACH, nullptr)) {
                module.initialized = true;
                throw std::runtime_error(module.name + ": DllMain returned FALSE");
            }
        }
        module.initialized = true;
    }

    Module& Loader::mapImage(const std::filesystem::path& path) {
        auto file = readFile(path);
        const std::string name = path.filename().string();
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
        if (file.size() < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE) throw std::runtime_error(path.string() + ": not an MZ image");
        if (dos->e_lfanew < 0 || static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size()) throw std::runtime_error(name + ": the NT headers are outside the file");
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) throw std::runtime_error(name + ": not an x86-64 PE");
        if (nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) throw std::runtime_error(name + ": not a PE32+ image");
        auto& optional = nt->OptionalHeader;
        uint64_t sections = static_cast<uint64_t>(dos->e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
        if (sections + static_cast<uint64_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > file.size()) throw std::runtime_error(name + ": the section table runs past the end of the file");
        if (!optional.SizeOfImage || optional.SizeOfHeaders > optional.SizeOfImage || optional.SizeOfHeaders > file.size()) throw std::runtime_error(name + ": SizeOfHeaders and SizeOfImage dont fit the file");

        auto module = new Module;
        module->name = name;
        module->path = path;
        module->preferredBase = optional.ImageBase;
        module->size = optional.SizeOfImage;
        module->entryRva = optional.AddressOfEntryPoint;
        m_byName[lower(module->name)] = module;
        m_order.push_back(module);
        module->base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(optional.ImageBase), optional.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!module->base) {
            auto& relocs = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (!relocs.Size) throw std::runtime_error(module->name + ": preferred base is taken and the image has no relocations");
            module->base = static_cast<uint8_t*>(VirtualAlloc(nullptr, optional.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!module->base) throw std::runtime_error(module->name + ": VirtualAlloc failed");
        }

        std::memcpy(module->base, file.data(), optional.SizeOfHeaders);
        auto section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            uint32_t size = std::min(section->SizeOfRawData, section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData);
            if (!size) continue;
            if (section->PointerToRawData > file.size() || size > file.size() - section->PointerToRawData) throw std::runtime_error(module->name + ": section " + std::to_string(i) + " reads past the end of the file");
            requireRange(module->name, section->VirtualAddress, size, module->size, "a sections data");
            std::memcpy(module->base + section->VirtualAddress, file.data() + section->PointerToRawData, size);
        }

        int64_t delta = reinterpret_cast<int64_t>(module->base) - static_cast<int64_t>(optional.ImageBase);
        if (delta) {
            module->relocated = true;
            auto& dir = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (!dir.Size) throw std::runtime_error(module->name + ": the image has no relocations and isnt at its base");
            requireRange(module->name, dir.VirtualAddress, dir.Size, module->size, "the relocation table");
            auto block = at<IMAGE_BASE_RELOCATION>(module->base, dir.VirtualAddress);
            auto end = reinterpret_cast<uint8_t*>(block) + dir.Size;
            while (reinterpret_cast<uint8_t*>(block) < end) {
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock > end) throw std::runtime_error(module->name + ": a relocation block doesnt fit the relocation table");
                auto entries = reinterpret_cast<uint16_t*>(block + 1);
                size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                for (size_t i = 0; i < count; ++i) {
                    uint16_t type = entries[i] >> 12;
                    uint16_t offset = entries[i] & 0xFFF;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        requireRange(module->name, static_cast<uint64_t>(block->VirtualAddress) + offset, sizeof(uint64_t), module->size, "a relocation");
                        *at<uint64_t>(module->base, block->VirtualAddress + offset) += delta;
                    } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                        throw std::runtime_error(module->name + ": unsupported relocation type " + std::to_string(type));
                    }
                }
                block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock);
            }
        }
        initializeTls(*module);
        parseExports(*module);
        {
            std::lock_guard lock(members::m_mappedImagesMutex);
            members::m_mappedImages.push_back(module);
        }
        return *module;
    }

    void Loader::initializeTls(Module& module) {
        auto tls = tlsDirectory(module);
        if (!tls) return;
        if (!tls->StartAddressOfRawData && !tls->EndAddressOfRawData && !tls->AddressOfIndex && !tls->AddressOfCallBacks && !tls->SizeOfZeroFill) return;
        if (tls->EndAddressOfRawData < tls->StartAddressOfRawData) throw std::runtime_error(module.name + ": TLS template end precedes its start");
        uint64_t raw = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
        uint64_t total = raw + tls->SizeOfZeroFill;
        if (total > 0xFFFFFFFFull) throw std::runtime_error(module.name + ": TLS block is too large");
        const uint32_t rawRva = raw ? static_cast<uint32_t>(rvaIn(module, tls->StartAddressOfRawData, "the TLS template")) : 0;
        if (raw) requireRange(module.name, rawRva, raw, module.size, "the TLS template");
        uint64_t indexRva = 0;
        if (tls->AddressOfIndex) {
            indexRva = rvaIn(module, tls->AddressOfIndex, "the TLS index");
            requireRange(module.name, indexRva, sizeof(uint32_t), module.size, "the TLS index");
        }
        if (!members::moduleTlsSlots()) throw std::runtime_error(module.name + ": this thread has no TLS array");
        DWORD index = TLS_OUT_OF_INDEXES;
        uint32_t capacity = 0;
        if (!acquireTlsProvider(static_cast<uint32_t>(total), index, capacity)) {
            throw std::runtime_error(module.name + ": no static TLS provider is large enough for " + std::to_string(total) + " bytes");
        }
        module.tlsIndex = index;
        module.tlsTemplate = raw ? module.base + rawRva : nullptr;
        module.tlsRawSize = static_cast<uint32_t>(raw);
        module.tlsZeroFill = tls->SizeOfZeroFill;
        module.tlsSize = static_cast<uint32_t>(total);
        if (indexRva) *reinterpret_cast<uint32_t*>(module.base + indexRva) = index;
        installTlsForThread(module);
    }

    static thread_local DWORD g_installedTlsSlots[16];
    static thread_local unsigned g_installedTlsCount = 0;

    static bool tlsInstalledInThisThread(DWORD index) {
        for (unsigned i = 0; i < g_installedTlsCount; ++i) {
            if (g_installedTlsSlots[i] == index) return true;
        }
        return false;
    }

    static void markTlsInstalledInThisThread(DWORD index) {
        if (g_installedTlsCount < _countof(g_installedTlsSlots)) g_installedTlsSlots[g_installedTlsCount++] = index;
    }

    void installModuleTls(Module& module) {
        if (module.tlsIndex == noTlsIndex) return;
        const uint64_t total = static_cast<uint64_t>(module.tlsRawSize) + module.tlsZeroFill;
        if (!total) return;
        void** slots = members::moduleTlsSlots();
        if (!slots || module.tlsIndex >= members::maxStaticTlsIndex) throw std::runtime_error(module.name + ": cannot install TLS in this thread");
        void* block = slots[module.tlsIndex];
        if (!block) throw std::runtime_error(module.name + ": the provider TLS block is missing in this thread");
        if (tlsInstalledInThisThread(module.tlsIndex)) return;
        if (module.tlsRawSize) std::memcpy(block, module.tlsTemplate, module.tlsRawSize);
        if (module.tlsZeroFill) std::memset(static_cast<uint8_t*>(block) + module.tlsRawSize, 0, module.tlsZeroFill);
        markTlsInstalledInThisThread(module.tlsIndex);
        if (std::getenv("BSXBOX_TRACE_TLS")) {
            const uint32_t installedEpoch = total > 5056 ? *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(block) + 5056) : 0;
            bsx::logf("tls", "installed %s (index %lu, %u bytes, epoch %08x, thread %lu)",
                module.name.c_str(), static_cast<unsigned long>(module.tlsIndex), module.tlsRawSize,
                installedEpoch, static_cast<unsigned long>(GetCurrentThreadId()));
        }
    }

    void uninstallModuleTls(Module& module) {
        (void)module;
    }

    void runModuleTlsCallbacks(Module& module, uint32_t reason) {
        auto tls = tlsDirectory(module);
        if (!tls || !tls->AddressOfCallBacks) return;
        const uint64_t tableRva = rvaIn(module, tls->AddressOfCallBacks, "the TLS callback table");
        for (uint64_t i = 0;; ++i) {
            const uint64_t entryRva = tableRva + i * sizeof(uint64_t);
            requireRange(module.name, entryRva, sizeof(uint64_t), module.size, "a TLS callback table entry");
            const uint64_t function = *at<uint64_t>(module.base, static_cast<uint32_t>(entryRva));
            if (!function) break;
            auto callback = reinterpret_cast<PIMAGE_TLS_CALLBACK>(module.base + rvaIn(module, function, "a TLS callback"));
            callback(module.base, static_cast<DWORD>(reason), nullptr);
        }
    }

    void installTlsForCurrentThread() {
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* module : members::m_mappedImages) {
            try { installModuleTls(*module); } catch (...) {}
        }
    }

    void uninstallTlsForCurrentThread() {
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* module : members::m_mappedImages) uninstallModuleTls(*module);
    }

    void runTlsCallbacksAll(uint32_t reason) {
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* module : members::m_mappedImages) {
            try { runModuleTlsCallbacks(*module, reason); } catch (...) {}
        }
    }

    void Loader::installTlsForThread(Module& module) {
        installModuleTls(module);
    }

    void Loader::installTlsForAll() {
        for (auto module : m_order) installModuleTls(*module);
    }

    void Loader::uninstallTlsForAll() {
        for (auto module : m_order) uninstallModuleTls(*module);
    }

    void Loader::uninstallTlsForThread(Module& module) {
        uninstallModuleTls(module);
    }

    void Loader::runTlsCallbacks(Module& module, uint32_t reason) {
        runModuleTlsCallbacks(module, reason);
    }

    void Loader::parseExports(Module& module) {
        auto nt = at<IMAGE_NT_HEADERS64>(module.base, reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.Size) return;
        requireRange(module.name, dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), module.size, "the export directory");
        auto exports = at<IMAGE_EXPORT_DIRECTORY>(module.base, dir.VirtualAddress);
        requireRange(module.name, exports->AddressOfFunctions, static_cast<uint64_t>(exports->NumberOfFunctions) * sizeof(uint32_t), module.size, "the export address table");
        requireRange(module.name, exports->AddressOfNames, static_cast<uint64_t>(exports->NumberOfNames) * sizeof(uint32_t), module.size, "the export name table");
        requireRange(module.name, exports->AddressOfNameOrdinals, static_cast<uint64_t>(exports->NumberOfNames) * sizeof(uint16_t), module.size, "the export ordinal table");
        auto functions = at<uint32_t>(module.base, exports->AddressOfFunctions);
        auto names = at<uint32_t>(module.base, exports->AddressOfNames);
        auto ordinals = at<uint16_t>(module.base, exports->AddressOfNameOrdinals);
        const uint64_t dirBegin = dir.VirtualAddress;
        const uint64_t dirEnd = dirBegin + dir.Size;
        auto addExport = [&](const std::string& key, uint32_t rva) {
            if (rva >= dirBegin && rva < dirEnd) module.forwards[key] = readString(module.name, module.base, module.size, rva, "a forwarded export");
            else module.exports[key] = module.base + rva;
        };
        for (uint32_t i = 0; i < exports->NumberOfFunctions; ++i) {
            if (functions[i]) addExport("#" + std::to_string(exports->Base + i), functions[i]);
        }
        for (uint32_t i = 0; i < exports->NumberOfNames; ++i) {
            if (ordinals[i] >= exports->NumberOfFunctions) throw std::runtime_error(module.name + ": an export name points past the address table");
            addExport(readString(module.name, module.base, module.size, names[i], "an export name"), functions[ordinals[i]]);
        }
    }

    void* Loader::resolveForwarder(const std::string& spec, Binding& binding, int depth) {
        if (depth >= 8) return nullptr;
        auto dot = spec.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= spec.size()) return nullptr;
        const std::string moduleName = spec.substr(0, dot);
        const std::string symbol = spec.substr(dot + 1);
        std::string shimKey = lower(moduleName);
        if (shimKey.size() < 4 || shimKey.compare(shimKey.size() - 4, 4, ".dll") != 0) shimKey += ".dll";
        if (void* function = shims::find(shimKey, symbol)) {
            binding = Binding::Shim;
            return function;
        }
        const bool byOrdinal = symbol.size() > 1 && symbol[0] == '#';
        uint32_t ordinal = 0;
        if (byOrdinal) {
            for (size_t i = 1; i < symbol.size(); ++i) {
                if (symbol[i] < '0' || symbol[i] > '9' || ordinal > 0xFFFF) return nullptr;
                ordinal = ordinal * 10 + static_cast<uint32_t>(symbol[i] - '0');
            }
        }
        const std::filesystem::path candidates[] = {m_gameDir / moduleName, m_gameDir / (moduleName + ".dll")};
        for (const auto& candidate : candidates) {
            std::error_code ec;
            if (m_hostAllDlls || preferHostCopy(lower(moduleName)) || !std::filesystem::exists(candidate, ec)) continue;
            Module& dependency = load(candidate);
            if (auto it = dependency.exports.find(symbol); it != dependency.exports.end()) {
                binding = Binding::Module;
                return it->second;
            }
            if (auto it = dependency.forwards.find(symbol); it != dependency.forwards.end()) return resolveForwarder(it->second, binding, depth + 1);
            return nullptr;
        }
        HMODULE host = loadHostModule(m_gameDir, moduleName);
        if (!host) return nullptr;
        FARPROC function = GetProcAddress(host, byOrdinal ? MAKEINTRESOURCEA(static_cast<WORD>(ordinal)) : symbol.c_str());
        if (!function) return nullptr;
        binding = Binding::Host;
        return reinterpret_cast<void*>(function);
    }

    void* Loader::resolve(Module& module, const std::string& dll, const std::string& name, uint16_t ordinal, bool byOrdinal) {
        std::string key = lower(dll);
        std::string symbol = byOrdinal ? "#" + std::to_string(ordinal) : name;

        struct BundledImport { void* function; Binding binding; };
        auto tryBundled = [&]() -> BundledImport {
            auto bundled = m_gameDir / dll;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(bundled, ec)) return {nullptr, Binding::Unimpl};
            Module& dependency = load(bundled);
            if (auto it = dependency.exports.find(symbol); it != dependency.exports.end()) {
                return {it->second, Binding::Module};
            }
            if (auto it = dependency.forwards.find(symbol); it != dependency.forwards.end()) {
                Binding binding = Binding::Unimpl;
                if (void* function = resolveForwarder(it->second, binding, 0)) return {function, binding};
            }
            return {nullptr, Binding::Unimpl};
        };
        auto useBundled = [&](const BundledImport& found) {
            module.imports.push_back({dll, symbol, found.binding});
            return found.function;
        };

        if (void* function = shims::find(key, symbol)) {
            std::error_code ec;
            if (!preferHostCopy(key) && std::filesystem::is_regular_file(m_gameDir / dll, ec)) {
                try {
                    (void)tryBundled();
                } catch (const std::exception& e) {
                    bsx::logf("loader", "%s: cannot map bundled %s for shim: %s", module.name.c_str(), dll.c_str(), e.what());
                }
            }
            module.imports.push_back({dll, symbol, Binding::Shim});
            return function;
        }

        if (!m_hostAllDlls && !preferHostCopy(key)) {
            auto found = tryBundled();
            if (found.function) return useBundled(found);
        }
        HMODULE host = loadHostModule(m_gameDir, dll);
        if (host) {
            FARPROC function = GetProcAddress(host, byOrdinal ? MAKEINTRESOURCEA(ordinal) : name.c_str());
            if (function) {
                module.imports.push_back({dll, symbol, Binding::Host});
                return reinterpret_cast<void*>(function);
            }
        }
        if (!preferHostCopy(key)) {
            try {
                auto found = tryBundled();
                if (found.function) return useBundled(found);
            } catch (const std::exception& e) {
                bsx::logf("loader", "%s: cannot map bundled %s: %s", module.name.c_str(), dll.c_str(), e.what());
            }
        }
        module.imports.push_back({dll, symbol, Binding::Unimpl});
        return shims::unimplthunk(dll, symbol);
    }

    void Loader::bindImports(Module& module) {
        auto nt = at<IMAGE_NT_HEADERS64>(module.base, reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
        auto bindThunks = [&](const std::string& dll, uint64_t lookupRva, uint64_t iatRva) {
            for (;; lookupRva += sizeof(IMAGE_THUNK_DATA64), iatRva += sizeof(IMAGE_THUNK_DATA64)) {
                requireRange(module.name, lookupRva, sizeof(IMAGE_THUNK_DATA64), module.size, "an import lookup table");
                requireRange(module.name, iatRva, sizeof(IMAGE_THUNK_DATA64), module.size, "an import address table");
                auto lookup = at<IMAGE_THUNK_DATA64>(module.base, static_cast<uint32_t>(lookupRva));
                if (!lookup->u1.AddressOfData) return;
                bool byOrdinal = IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal);
                uint16_t ordinal = byOrdinal ? static_cast<uint16_t>(IMAGE_ORDINAL64(lookup->u1.Ordinal)) : 0;
                std::string name;
                if (!byOrdinal) {
                    uint32_t hint = static_cast<uint32_t>(lookup->u1.AddressOfData);
                    requireRange(module.name, hint, sizeof(uint16_t), module.size, "an import name");
                    name = readString(module.name, module.base, module.size, hint + sizeof(uint16_t), "an import name");
                }
                at<IMAGE_THUNK_DATA64>(module.base, static_cast<uint32_t>(iatRva))->u1.Function = reinterpret_cast<uint64_t>(resolve(module, dll, name, ordinal, byOrdinal));
            }
        };
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.Size) {
            for (uint64_t i = 0;; ++i) {
                uint64_t rva = dir.VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
                requireRange(module.name, rva, sizeof(IMAGE_IMPORT_DESCRIPTOR), module.size, "the import table");
                auto descriptor = at<IMAGE_IMPORT_DESCRIPTOR>(module.base, static_cast<uint32_t>(rva));
                if (!descriptor->Name) break;
                std::string dll = readString(module.name, module.base, module.size, descriptor->Name, "an imported DLL name");
                uint32_t lookup = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
                bindThunks(dll, lookup, descriptor->FirstThunk);
            }
        }
        auto& delayDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delayDir.Size) {
            for (uint64_t i = 0;; ++i) {
                uint64_t rva = delayDir.VirtualAddress + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
                requireRange(module.name, rva, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR), module.size, "the delay-import table");
                auto descriptor = at<IMAGE_DELAYLOAD_DESCRIPTOR>(module.base, static_cast<uint32_t>(rva));
                if (!descriptor->DllNameRVA) break;
                if (!descriptor->Attributes.RvaBased) throw std::runtime_error(module.name + ": a delay-import descriptor without the RVA attribute");
                if (!descriptor->ImportAddressTableRVA) throw std::runtime_error(module.name + ": a delay-import descriptor has no address table");
                std::string dll = readString(module.name, module.base, module.size, descriptor->DllNameRVA, "a delay-imported DLL name");
                if (!descriptor->ImportNameTableRVA) throw std::runtime_error(module.name + ": a delay-import descriptor has no name table");
                bindThunks(dll, descriptor->ImportNameTableRVA, descriptor->ImportAddressTableRVA);
            }
        }
    }

    void Loader::protectSections(Module& module) {
        auto nt = at<IMAGE_NT_HEADERS64>(module.base, reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
        DWORD old;
        VirtualProtect(module.base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
        auto section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            bool executable = section->Characteristics & IMAGE_SCN_MEM_EXECUTE;
            bool writable = section->Characteristics & IMAGE_SCN_MEM_WRITE;
            DWORD protection = executable ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (writable ? PAGE_READWRITE : PAGE_READONLY);
            uint64_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (section->VirtualAddress >= module.size) continue;
            size = std::min<uint64_t>(size, module.size - section->VirtualAddress);
            if (size) VirtualProtect(module.base + section->VirtualAddress, static_cast<SIZE_T>(size), protection, &old);
        }
        FlushInstructionCache(GetCurrentProcess(), module.base, module.size);
    }

    void* mappedImageFor(const void* address, void** base) {
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* module : members::m_mappedImages) {
            if (address >= module->base && address < module->base + module->size) {
                if (base) *base = module->base;
                return module->base;
            }
        }
        return nullptr;
    }

    void* findMappedExport(const std::string& dll, const std::string& symbol) {
        const std::string key = lower(dll);
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* module : members::m_mappedImages) {
            if (lower(module->name) != key) continue;
            auto it = module->exports.find(symbol);
            if (it != module->exports.end()) return it->second;
        }
        return nullptr;
    }

    void setGameDirectory(const std::filesystem::path& path) {
        members::m_gameDir = path;
    }

    void setActiveLoader(Loader* loader) {
        members::m_activeLoader = loader;
    }

    Loader* activeLoader() {
        return members::m_activeLoader;
    }

    void* findMappedModule(void* module) {
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* candidate : members::m_mappedImages) {
            if (module && module == candidate->base) return candidate->base;
        }
        return nullptr;
    }

    void* findMappedModuleExport(void* module, const char* name) {
        if (!module || !name) return nullptr;
        std::lock_guard lock(members::m_mappedImagesMutex);
        for (Module* candidate : members::m_mappedImages) {
            if (module != candidate->base) continue;
            if (auto it = candidate->exports.find(name); it != candidate->exports.end()) return it->second;
            return nullptr;
        }
        return nullptr;
    }

    const std::filesystem::path& gameDirectory() {
        return members::m_gameDir;
    }
}
