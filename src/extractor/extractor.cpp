#include <extractor/extractor.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace bsx::extractor {
    namespace members {
        uint16_t read16(const uint8_t* p) {
            return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
        }

        uint32_t read32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        uint64_t read64(const uint8_t* p) {
            return static_cast<uint64_t>(read32(p)) | (static_cast<uint64_t>(read32(p + 4)) << 32);
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::string slashes(const std::string& name) {
            std::string result = name;
            std::replace(result.begin(), result.end(), '\\', '/');
            return result;
        }

        std::vector<uint8_t> readFile(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("couldnt open: " + path.string());
            return {std::istreambuf_iterator<char>(file), {}};
        }

        struct Entry {
            std::string name;
            uint16_t method = 0;
            uint16_t flags = 0;
            uint64_t compressedSize = 0;
            uint64_t uncompressedSize = 0;
            uint64_t offset = 0;
        };

        class BitReader {
            public:
            BitReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

            uint32_t bits(int count) {
                while (m_bitCount < count) {
                    if (m_pos >= m_size) throw std::runtime_error("deflate: the stream ended early");
                    m_buffer |= static_cast<uint32_t>(m_data[m_pos++]) << m_bitCount;
                    m_bitCount += 8;
                }
                
                const uint32_t value = m_buffer & ((1u << count) - 1u);
                m_buffer >>= count;
                m_bitCount -= count;
                return value;
            }

            void align() {
                m_buffer = 0;
                m_bitCount = 0;
            }

            private:
            const uint8_t* m_data;
            size_t m_size;
            size_t m_pos = 0;
            uint32_t m_buffer = 0;
            int m_bitCount = 0;
        };

        class Huffman {
            public:
            void build(const uint8_t* lengths, size_t count) {
                std::fill(m_count, m_count + 16, 0);
                std::fill(m_used, m_used + 16, 0);
                for (size_t i = 0; i < count; ++i) ++m_count[lengths[i]];
                m_count[0] = 0;

                int code = 0;
                for (int len = 1; len <= 15; ++len) {
                    code = (code + m_count[len - 1]) << 1;
                    m_firstCode[len] = code;
                }

                int next = 0;
                for (int len = 1; len <= 15; ++len) {
                    m_firstSymbol[len] = next;
                    next += m_count[len];
                }

                for (size_t sym = 0; sym < count; ++sym) {
                    const int len = lengths[sym];
                    if (len) m_symbols[m_firstSymbol[len] + m_used[len]++] = static_cast<int>(sym);
                }

                uint32_t total = 0;
                for (int len = 1; len <= 15; ++len) total += static_cast<uint32_t>(m_count[len]) << (15 - len);
                if (total > (1u << 15)) throw std::runtime_error("over subscribed huffman tree");
            }

            int decode(BitReader& reader) const {
                int code = 0;
                for (int len = 1; len <= 15; ++len) {
                    code = (code << 1) | static_cast<int>(reader.bits(1));
                    const int index = code - m_firstCode[len];
                    if (index >= 0 && index < m_count[len]) return m_symbols[m_firstSymbol[len] + index];
                }
                throw std::runtime_error("invalid huffman code");
            }

            private:
            int m_count[16] = {};
            int m_firstCode[16] = {};
            int m_firstSymbol[16] = {};
            int m_used[16] = {};
            int m_symbols[288] = {};
        };

        // i hate myself :3
        constexpr uint16_t lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t distBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr uint8_t distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        constexpr int codeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        void buildFixed(Huffman& lit, Huffman& dist) {
            uint8_t lengths[288];
            for (int i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            lit.build(lengths, 288);
            uint8_t distLengths[32];
            std::fill(distLengths, distLengths + 32, uint8_t(5));
            dist.build(distLengths, 32);
        }

        void buildDynamic(BitReader& reader, Huffman& lit, Huffman& dist) {
            const int litCount = static_cast<int>(reader.bits(5)) + 257;
            const int distCount = static_cast<int>(reader.bits(5)) + 1;
            const int codeCount = static_cast<int>(reader.bits(4)) + 4;

            uint8_t codeLengths[19] = {};
            for (int i = 0; i < codeCount; ++i) codeLengths[codeLengthOrder[i]] = static_cast<uint8_t>(reader.bits(3));
            Huffman codes;
            codes.build(codeLengths, 19);

            const int total = litCount + distCount;
            std::vector<uint8_t> lengths;
            lengths.reserve(total);
            while (static_cast<int>(lengths.size()) < total) {
                const int symbol = codes.decode(reader);
                int count = 1;
                uint8_t value = 0;
                if (symbol < 16) {
                    value = static_cast<uint8_t>(symbol);
                } else if (symbol == 16) {
                    if (lengths.empty()) throw std::runtime_error("a repeat length has nothing to repeat");
                    value = lengths.back();
                    count = 3 + static_cast<int>(reader.bits(2));
                } else if (symbol == 17) {
                    count = 3 + static_cast<int>(reader.bits(3));
                } else {
                    count = 11 + static_cast<int>(reader.bits(7));
                }
                if (count > total - static_cast<int>(lengths.size())) throw std::runtime_error("too many code lengths");
                while (count--) lengths.push_back(value);
            }

            lit.build(lengths.data(), litCount);
            dist.build(lengths.data() + litCount, distCount);
        }

        std::vector<uint8_t> inflate(const uint8_t* data, size_t size, size_t expected) {
            const size_t limit = expected ? expected : (64u << 20);
            BitReader reader(data, size);
            std::vector<uint8_t> out;
            out.reserve(std::min(limit, static_cast<size_t>(1u << 20)));

            while (true) {
                const int final = static_cast<int>(reader.bits(1));
                const int type = static_cast<int>(reader.bits(2));
                if (type == 0) {
                    reader.align();
                    const uint32_t length = reader.bits(16);
                    const uint32_t inverse = reader.bits(16);
                    if ((length ^ 0xFFFFu) != inverse) throw std::runtime_error("deflate: a stored block has a bad length");
                    if (length > limit - out.size()) throw std::runtime_error("deflate: the output is bigger than the entry");
                    for (uint32_t i = 0; i < length; ++i) out.push_back(static_cast<uint8_t>(reader.bits(8)));
                } else if (type == 1 || type == 2) {
                    Huffman lit;
                    Huffman dist;
                    if (type == 1) buildFixed(lit, dist);
                    else buildDynamic(reader, lit, dist);

                    while (true) {
                        const int symbol = lit.decode(reader);
                        if (symbol < 256) {
                            if (out.size() >= limit) throw std::runtime_error("deflate: the output is bigger than the entry");
                            out.push_back(static_cast<uint8_t>(symbol));
                        } else if (symbol == 256) {
                            break;
                        } else {
                            const int lengthIndex = symbol - 257;
                            if (lengthIndex >= 29) throw std::runtime_error("deflate: a bad length code");
                            const uint32_t length = lengthBase[lengthIndex] + reader.bits(lengthExtra[lengthIndex]);
                            const int distIndex = dist.decode(reader);
                            if (distIndex >= 30) throw std::runtime_error("deflate: a bad distance code");
                            const uint32_t distance = distBase[distIndex] + reader.bits(distExtra[distIndex]);
                            if (!distance || distance > out.size() || length > limit - out.size()) throw std::runtime_error("deflate: a bad match");
                            for (uint32_t i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
                        }
                    }
                } else {
                    throw std::runtime_error("deflate: a reserved block type");
                }
                if (final) break;
            }

            if (expected && out.size() != expected) throw std::runtime_error("deflate: the entry has an unexpected size");
            return out;
        }

        size_t findEndRecord(const std::vector<uint8_t>& data) {
            if (data.size() < 22) throw std::runtime_error("zip: the file is too small");
            const size_t floor = data.size() > 65557 ? data.size() - 65557 : 0;
            size_t pos = data.size() - 22;
            while (true) {
                if (read32(data.data() + pos) == endSignature) {
                    const size_t comment = read16(data.data() + pos + 20);
                    if (pos + 22 + comment <= data.size()) return pos;
                }
                if (pos == floor) break;
                --pos;
            }
            throw std::runtime_error("zip: no end of central directory record");
        }

        std::vector<Entry> readDirectory(const std::vector<uint8_t>& data) {
            const size_t end = findEndRecord(data);
            uint64_t count = read16(data.data() + end + 10);
            uint64_t size = read32(data.data() + end + 12);
            uint64_t offset = read32(data.data() + end + 16);

            if (end >= 20 && read32(data.data() + end - 20) == zip64LocatorSignature) {
                const uint64_t zip64 = read64(data.data() + end - 12);
                if (zip64 + 56 <= data.size() && read32(data.data() + zip64) == zip64EndSignature) {
                    count = read64(data.data() + zip64 + 32);
                    size = read64(data.data() + zip64 + 40);
                    offset = read64(data.data() + zip64 + 48);
                }
            }
            if (offset > data.size() || size > data.size() - offset) throw std::runtime_error("zip: the central directory is outside the file");

            std::vector<Entry> entries;
            entries.reserve(static_cast<size_t>(std::min<uint64_t>(count, 1u << 20)));
            size_t pos = static_cast<size_t>(offset);
            for (uint64_t i = 0; i < count; ++i) {
                if (pos > data.size() || data.size() - pos < 46) throw std::runtime_error("zip: a central directory header is truncated");
                const uint8_t* header = data.data() + pos;
                if (read32(header) != centralSignature) throw std::runtime_error("zip: a bad central directory signature");
                Entry entry;
                entry.method = read16(header + 10);
                entry.flags = read16(header + 8);
                entry.compressedSize = read32(header + 20);
                entry.uncompressedSize = read32(header + 24);
                const uint16_t nameLength = read16(header + 28);
                const uint16_t extraLength = read16(header + 30);
                const uint16_t commentLength = read16(header + 32);
                entry.offset = read32(header + 42);

                const size_t record = 46u + nameLength + extraLength + commentLength;
                if (record > data.size() - pos) throw std::runtime_error("zip: a central directory header runs past the end of the file");
                entry.name.assign(reinterpret_cast<const char*>(header + 46), nameLength);
                if (entry.name.find('\0') != std::string::npos) throw std::runtime_error("zip: an entry name contains a nul byte");

                if (entry.compressedSize == 0xFFFFFFFFu || entry.uncompressedSize == 0xFFFFFFFFu || entry.offset == 0xFFFFFFFFu) {
                    const uint8_t* extra = header + 46 + nameLength;
                    size_t left = extraLength;
                    while (left >= 4) {
                        const uint16_t id = read16(extra);
                        const uint16_t length = read16(extra + 2);
                        extra += 4;
                        left -= 4;
                        if (length > left) break;
                        if (id == 0x0001u) {
                            if (entry.uncompressedSize == 0xFFFFFFFFu) { entry.uncompressedSize = read64(extra); extra += 8; }
                            if (entry.compressedSize == 0xFFFFFFFFu) { entry.compressedSize = read64(extra); extra += 8; }
                            if (entry.offset == 0xFFFFFFFFu) entry.offset = read64(extra);
                            break;
                        }
                        extra += length;
                        left -= length;
                    }
                }
                entries.push_back(std::move(entry));
                pos += record;
            }
            return entries;
        }

        std::vector<uint8_t> readEntry(const std::vector<uint8_t>& data, const Entry& entry) {
            if (entry.flags & 1u) throw std::runtime_error("zip: encrypted entries arent supported");
            if (entry.offset > data.size() || data.size() - entry.offset < 30) throw std::runtime_error("zip: a local header is outside the file");
            const uint8_t* local = data.data() + entry.offset;
            if (read32(local) != localSignature) throw std::runtime_error("zip: a bad local header signature");
            const size_t start = static_cast<size_t>(entry.offset) + 30u + read16(local + 26) + read16(local + 28);
            if (start > data.size() || entry.compressedSize > data.size() - start) throw std::runtime_error("zip: an entry runs past the end of the file");
            const uint8_t* source = data.data() + start;
            if (entry.method == 0) return {source, source + entry.compressedSize};
            if (entry.method == 8) return inflate(source, static_cast<size_t>(entry.compressedSize), static_cast<size_t>(entry.uncompressedSize));
            throw std::runtime_error("zip: unsupported compression method " + std::to_string(entry.method));
        }

        const Entry* findEntry(const std::vector<Entry>& entries, const std::string& name) {
            const std::string wanted = lower(slashes(name));
            for (const auto& entry : entries) {
                if (lower(slashes(entry.name)) == wanted) return &entry;
            }
            return nullptr;
        }

        std::string attributeValue(const std::string& xml, const char* tag, const char* attribute) {
            size_t pos = 0;
            while ((pos = xml.find(tag, pos)) != std::string::npos) {
                const size_t end = xml.find('>', pos);
                if (end == std::string::npos) break;
                const size_t name = xml.find(attribute, pos);
                if (name != std::string::npos && name < end) {
                    const size_t equals = xml.find('=', name + std::strlen(attribute));
                    if (equals != std::string::npos && equals < end) {
                        const size_t quote = xml.find_first_of("\"'", equals + 1);
                        if (quote != std::string::npos && quote < end) {
                            const size_t close = xml.find(xml[quote], quote + 1);
                            if (close != std::string::npos && close < end) return xml.substr(quote + 1, close - quote - 1);
                        }
                    }
                }
                pos = end + 1;
            }
            return {};
        }

        std::string chooseExecutable(const std::vector<Entry>& entries, const std::vector<uint8_t>& data, const std::filesystem::path& package) {
            const char* manifests[] = {"AppxManifest.xml", "MicrosoftGame.config"};
            for (const char* manifest : manifests) {
                const Entry* file = findEntry(entries, manifest);
                if (!file) continue;
                const auto bytes = readEntry(data, *file);
                const std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                std::string exe = attributeValue(xml, "<Application", "Executable");
                if (exe.empty() && std::string(manifest) == "MicrosoftGame.config") exe = attributeValue(xml, "<Executable", "Name");
                if (!exe.empty()) {
                    if (const Entry* found = findEntry(entries, exe)) return found->name;
                }
            }

            const std::string stem = lower(package.stem().string()) + ".exe";
            const Entry* best = nullptr;
            int bestScore = 0;
            for (const auto& entry : entries) {
                const std::string name = slashes(entry.name);
                if (lower(std::filesystem::path(name).extension().string()) != ".exe") continue;
                const int depth = static_cast<int>(std::count(name.begin(), name.end(), '/'));
                const bool matches = lower(std::filesystem::path(name).filename().string()) == stem;
                const int score = (matches ? 0 : 1000) + depth;
                if (!best || score < bestScore) {
                    best = &entry;
                    bestScore = score;
                }
            }
            if (!best) throw std::runtime_error(package.string() + ": no executable found in the package");
            return best->name;
        }

        std::filesystem::path safePath(const std::string& name, const std::filesystem::path& destination) {
            const std::string path = slashes(name);
            if (path.empty() || path.front() == '/' || (path.size() >= 2 && path[1] == ':')) throw std::runtime_error("zip: an entry has an absolute path: " + name);

            std::filesystem::path relative;
            size_t start = 0;
            while (start <= path.size()) {
                const size_t end = path.find('/', start);
                const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!part.empty() && part != ".") {
                    if (part == "..") throw std::runtime_error("zip: an entry escapes the destination: " + name);
                    relative /= part;
                }
                if (end == std::string::npos) break;
                start = end + 1;
            }
            if (relative.empty()) throw std::runtime_error("zip: an entry has no file name: " + name);

            const auto base = destination.lexically_normal();
            const auto target = (destination / relative).lexically_normal();
            auto a = base.begin();
            auto b = target.begin();
            for (; a != base.end(); ++a, ++b) {
                if (b == target.end() || *a != *b) throw std::runtime_error("zip: an entry escapes the destination: " + name);
            }
            return relative;
        }
    }

    bool isPackage(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        uint8_t magic[4] = {};
        file.read(reinterpret_cast<char*>(magic), sizeof(magic));
        if (file.gcount() < 4 || magic[0] != 'P' || magic[1] != 'K') return false;
        return (magic[2] == 3 && magic[3] == 4) || (magic[2] == 5 && magic[3] == 6) || (magic[2] == 7 && magic[3] == 8) || (magic[2] == 6 && magic[3] == 6);
    }

    std::filesystem::path cacheDirectory(const std::filesystem::path& package) {
        return std::filesystem::absolute(package).parent_path() / ".bsxbox" / package.stem();
    }

    Package extract(const std::filesystem::path& package, const std::filesystem::path& destination) {
        const auto data = members::readFile(package);
        if (data.size() < 4 || members::read32(data.data()) != members::localSignature) throw std::runtime_error(package.string() + ": not a zip package");
        const auto entries = members::readDirectory(data);
        const std::string executableName = members::chooseExecutable(entries, data, package);
        const auto relativeExe = members::safePath(executableName, destination);

        Package result;
        result.root = destination;
        result.executable = destination / relativeExe;

        std::error_code ec;
        if (std::filesystem::is_directory(destination, ec) && std::filesystem::is_regular_file(result.executable, ec)) {
            return result;
        }

        std::filesystem::create_directories(destination, ec);
        if (ec) throw std::runtime_error("couldnt create: " + destination.string());

        for (const auto& entry : entries) {
            if (!entry.name.empty() && entry.name.back() == '/') continue;
            const auto relative = members::safePath(entry.name, destination);
            const auto target = destination / relative;
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec) throw std::runtime_error("couldnt create: " + target.parent_path().string());
            const auto bytes = members::readEntry(data, entry);
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("couldnt write: " + target.string());
            if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) throw std::runtime_error("couldnt write: " + target.string());
            ++result.fileCount;
        }
        result.executable = destination / relativeExe;
        if (!std::filesystem::is_regular_file(result.executable)) throw std::runtime_error(result.executable.string() + ": the package executable wasnt extracted");
        return result;
    }
}
