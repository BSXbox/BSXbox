#include <pe/pe.hpp>
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
#include <minwindef.h>
#include <libloaderapi.h>
#include <processthreadsapi.h>

namespace bsx {
    std::string lower(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(::tolower(c));
        }
        return s;
    }
    namespace members {
        constexpr DWORD maxTlsSlots = 64;
        void** moduleTlsSlots() {return reinterpret_cast<void**>(__readgsqword(0x58));}

        struct TlsHostModule {
            HMODULE module = nullptr;
            DWORD index = TLS_OUT_OF_INDEXES;
        };
    }

    namespace helpers {
        std::vector<uint8_t> readFile(const std::string& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                throw std::runtime_error("couldnt open: " + path);
            }

            return {std::istreambuf_iterator<char>(file), {}};
        }

        template <class T>
        T* at(uint8_t* base, uint32_t rva) {
            return reinterpret_cast<T*>(base + rva);
        }

        void requireRange(const std::string& name, uint64_t rva, uint64_t len, uint32_t imageSize, const char* what) {
            if (rva > imageSize || len > imageSize - rva) {
                throw std::runtime_error(name + ": " + what + " runs past the img");
            }
        }

        std::string readString(const std::string& name, uint8_t* base, uint32_t imageSize, uint32_t rva, const char* what) {
            if (rva >= imageSize) {
                throw std::runtime_error(name + ": " + what + " is outside of the img");
            }

            auto s = reinterpret_cast<const char*>(base + rva);
            auto end = static_cast<const char*>(std::memchr(s, 0, imageSize - rva));

            if (!end) {
                throw std::runtime_error(name + ": " + what + " isnt terminated inside img");
            }

            return {s, end};
        }

        uint64_t rva_in(const Module& m, uint64_t va, const char* what) {
            
        }

        members::TlsHostModule& loadTlsH() {
            members::TlsHostModule host;

            wchar_t exe[MAX_PATH] = {};
            if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return host;
        }
    }
}