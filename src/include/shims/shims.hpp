#pragma once
#include <string>

namespace bsx::shims {
    struct Entry {
        const char* dll;
        const char* name;
        void* fn;
    };

    struct Table {
        Table(const Entry* e, size_t c);
    };

    void* find(const std::string& dll, const std::string& name);
    void* unimplthunk(const std::string& dll, const std::string& name);
    void reportcalledunimpl();
}

#define SHIMTABLE(id, ...) \
    static const bsx::shims::Entry id##_entries[] = { __VA_ARGS__ }; \
    static bsx::shims::Table id##_table(id##_entries, sizeof(id##_entries) / sizeof(id##_entries[0]));