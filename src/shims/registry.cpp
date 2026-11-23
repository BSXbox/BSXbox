#include <shims/shims.hpp>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <log.hpp>

namespace bsx::shims {
    namespace members {
        std::mutex m_calledM;
        std::set<std::string> m_called;
        constexpr size_t m_thunkSize = 32;
        /*
         * the thunk layout if anybody is wondering
         * 48 B9 <imm64>    mov rcx, what
         * 48 B8 <imm64>    mov rax, bsxUnimplemented
         * 0F 57 C0         xorps xmm0, xmm0
         * FF E0            jmp rax
        */

        uint8_t* m_pool = nullptr;
        size_t m_poolUsed = 0;
        size_t m_poolSize = 0;
        std::mutex m_poolM;

        std::map<std::string, void*>& table() {
            static std::map<std::string, void*> t;
            return t;
        }
    }

    extern "C" uint64_t bsxUnimplemented(const char* what) {
        std::lock_guard<std::mutex> lock(members::m_calledM);
        if (members::m_called.insert(what).second) {
            bsx::logf("imports", "unimplemented %s", what);
        }

        return 0;
    }

    std::map<std::string, void*>& table() {
        static std::map<std::string, void*> t; // dll!name
        return t;
    }

    Table::Table(const Entry* e, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            table()[std::string(e[i].dll) + "!" + e[i].name] = e[i].fn;
        }
    }

    void* find(const std::string& dll, const std::string& name) {
        auto it = table().find(dll + "!" + name);
        return it == table().end() ? nullptr : it->second;
    }

    void* unimplthunk(const std::string& dll, const std::string& name) {
        std::lock_guard lock(members::m_poolM);
        if (!members::m_pool || members::m_poolUsed + members::m_thunkSize > members::m_poolSize) {
            members::m_poolSize = 1 << 20; // 1 MB
            members::m_pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, members::m_poolSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            members::m_poolUsed = 0;

            if (!members::m_pool) return nullptr;
        }

        const char* what = _strdup((dll + "!" + name).c_str());
        uint8_t* t = members::m_pool + members::m_poolUsed;

        members::m_poolUsed += members::m_thunkSize;
        uint64_t a = reinterpret_cast<uint64_t>(what);
        uint64_t f = reinterpret_cast<uint64_t>(&bsxUnimplemented);

        // me when assembly
        // oh and go check lines 14-20

        uint8_t code[members::m_thunkSize] = {0x48, 0xB9};
        std::memcpy(code + 2, &a, 8);
        code[10] = 0x48;
        code[11] = 0xB8;

        std::memcpy(code + 12, &f, 8);
        code[20] = 0x0F;
        code[21] = 0x57;
        code[22] = 0xC0;
        code[23] = 0xFF;
        code[24] = 0xE0;
        // no more assembly :(

        std::memset(code + 25, 0xCC, members::m_thunkSize - 25);
        std::memcpy(t, code, members::m_thunkSize);

        FlushInstructionCache(GetCurrentProcess(), t, members::m_thunkSize);
        return t;
    }

    void reportcalledunimpl() {
        std::lock_guard lock(members::m_calledM);
        for (auto& s : members::m_called) {
            bsx::logf("imports", "called %s", s.c_str());
        }
    }
}
