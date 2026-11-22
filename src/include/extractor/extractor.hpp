#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace bsx::extractor {
    namespace members {
        inline constexpr std::uint32_t localSignature = 0x04034b50u;
        inline constexpr std::uint32_t centralSignature = 0x02014b50u;
        inline constexpr std::uint32_t endSignature = 0x06054b50u;
        inline constexpr std::uint32_t zip64EndSignature = 0x06064b50u;
        inline constexpr std::uint32_t zip64LocatorSignature = 0x07064b50u;
    }
    struct Package {
        std::filesystem::path root;
        std::filesystem::path executable;
        std::size_t fileCount = 0;
    };

    bool isPackage(const std::filesystem::path& path);
    std::filesystem::path cacheDirectory(const std::filesystem::path& package);
    Package extract(const std::filesystem::path& package, const std::filesystem::path& destination);
}
