#include <log.hpp>

#include <cstdarg>
#include <cstdio>

namespace bsx {
    void logf(const char* tag, const char* format, ...) {
        std::fprintf(stderr, "[bsx] [%s]: ", tag);
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
        std::fputc('\n', stderr);
    }
}
