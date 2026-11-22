#pragma once
#include <string>

namespace rt::shims {
    struct Entry {
        const char* dll;
        const char* name;
        void* fn;
    };

    struct Table {
        Table(const Entry* e, size_t c); // entires and count
    };
}