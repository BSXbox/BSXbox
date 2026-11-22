#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bsx {
    class Loader;
    struct Module;
}

namespace bsx::process {
    void setCommandLine(const std::string& program, const std::vector<std::string>& args);
    int runEntry(Loader& loader, Module& module, std::size_t stackBytes = 8u << 20);
}
