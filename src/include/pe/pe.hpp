#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bsx {
    enum class Binding {
        Shim,
        Host,
        Module,
        Unimpl
    };

    inline constexpr uint32_t noTlsIndex = 0xFFFFFFFFu;

    struct ImportRecord {
        std::string dll;
        std::string name;
        Binding binding;
    };

    struct Module {
        std::string name;
        std::filesystem::path path;
        uint8_t* base = nullptr;
        uint64_t preferredBase = 0;
        uint32_t size = 0;
        uint32_t entryRva = 0;

        bool relocated = false;
        bool initialized = false;

        void* functionTable = nullptr;
        uint32_t functionCount = 0;
        std::vector<ImportRecord> imports;
        std::map<std::string, void*> exports;
        std::map<std::string, std::string> forwards;

        // tls bullshit
        uint32_t tlsIndex = noTlsIndex;
        void* tlsBlock = nullptr;
        void* tlsSavedBlock = nullptr;
        uint32_t tlsSize = 0;
        const uint8_t* tlsTemplate = nullptr;
        uint32_t tlsRawSize = 0;
        uint32_t tlsZeroFill = 0;
    };

    class Loader {
        public:
        explicit Loader(std::filesystem::path gameDir, bool hostAllDlls = false) : m_gameDir(std::move(gameDir)), m_hostAllDlls(hostAllDlls) {}
        ~Loader();

        Module& load(const std::filesystem::path& image);
        Module& loadRuntime(const std::filesystem::path& image);
        const std::vector<Module*>& modules() const { return m_order; }
        const std::vector<Module*>& initializationOrder() const { return m_initOrder; }
        void initialize();
        void runTlsCallbacks(Module& module, uint32_t reason);
        void installTlsForThread(Module& module);
        void installTlsForAll();
        void uninstallTlsForAll();
        void uninstallTlsForThread(Module& module);

        private:
        void initialize(Module& module);
        Module& mapImage(const std::filesystem::path& path);
        void initializeTls(Module& module);
        void bindImports(Module& module);
        void* resolve(Module& module, const std::string& dll, const std::string& name, uint16_t ordinal, bool byOrdinal);
        void* resolveForwarder(const std::string& spec, Binding& binding, int depth);
        void parseExports(Module& module);
        void protectSections(Module& module);
        void registerUnwind(Module& module);

        std::filesystem::path m_gameDir;
        bool m_hostAllDlls = false;
        std::map<std::string, Module*> m_byName;
        std::vector<Module*> m_order;
        std::vector<Module*> m_initOrder;
    };

    std::string lower(std::string s);
    void* mappedImageFor(const void* address, void** base);
    void* findMappedExport(const std::string& dll, const std::string& symbol);
    void* findMappedModule(void* module);
    void* findMappedModuleExport(void* module, const char* name);
    void setActiveLoader(Loader* loader);
    Loader* activeLoader();
    void setGameDirectory(const std::filesystem::path& path);
    const std::filesystem::path& gameDirectory();
    
    void installTlsForCurrentThread();
    void uninstallTlsForCurrentThread();
    void runTlsCallbacksAll(uint32_t reason);
}
