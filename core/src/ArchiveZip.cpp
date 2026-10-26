// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

// Boost includes
#include <boost/interprocess/sync/file_lock.hpp>

// zlib
#include <zlib.h>

// Euclid includes
#include <euclid/core/ArchiveZip.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace {

        constexpr std::uint32_t kLocalSignature = 0x04034b50;
        constexpr std::uint32_t kCentralSignature = 0x02014b50;
        constexpr std::uint32_t kZip64EndSignature = 0x06064b50;
        constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50;
        constexpr std::uint32_t kEndSignature = 0x06054b50;

        // 4.5, the version that knows Zip64.
        constexpr std::uint16_t kVersion = 45;
        // Made by Unix, so the permissions below mean something to whoever unpacks it.
        constexpr std::uint16_t kMadeBy = (3u << 8) | kVersion;
        constexpr std::uint32_t kRegularFile = 0100644u << 16;
        // Names are UTF-8, which is what an object key is.
        constexpr std::uint16_t kUtf8Names = 0x0800;
        constexpr std::uint32_t kInZip64 = 0xffffffff;

        constexpr std::size_t kLocalHeaderSize = 30;
        constexpr std::size_t kLocalExtraSize = 20;
        constexpr std::size_t kCentralHeaderSize = 46;
        constexpr std::size_t kEndSize = 22;
        constexpr std::size_t kLocatorSize = 20;
        constexpr std::size_t kZip64EndSize = 56;

        constexpr std::size_t kChunk = 64 * 1024;

        void put16(std::string &out, const std::uint16_t value) {
            out.push_back(static_cast<char>(value & 0xffu));
            out.push_back(static_cast<char>((value >> 8) & 0xffu));
        }

        void put32(std::string &out, const std::uint32_t value) {
            put16(out, static_cast<std::uint16_t>(value & 0xffffu));
            put16(out, static_cast<std::uint16_t>(value >> 16));
        }

        void put64(std::string &out, const std::uint64_t value) {
            put32(out, static_cast<std::uint32_t>(value & 0xffffffffu));
            put32(out, static_cast<std::uint32_t>(value >> 32));
        }

        std::uint16_t get16(const std::string &in, const std::size_t at) {
            return static_cast<std::uint16_t>(static_cast<unsigned char>(in[at]) | (static_cast<unsigned char>(in[at + 1]) << 8));
        }

        std::uint32_t get32(const std::string &in, const std::size_t at) {
            return static_cast<std::uint32_t>(get16(in, at)) | (static_cast<std::uint32_t>(get16(in, at + 2)) << 16);
        }

        std::uint64_t get64(const std::string &in, const std::size_t at) {
            return static_cast<std::uint64_t>(get32(in, at)) | (static_cast<std::uint64_t>(get32(in, at + 4)) << 32);
        }

        struct DosTime {
            std::uint16_t time = 0;
            std::uint16_t date = (1u << 5) | 1u;// 1980-01-01, the earliest a zip can say
        };

        DosTime dosNow() {
            using namespace std::chrono;
            const auto now = floor<seconds>(system_clock::now());
            const auto day = floor<days>(now);
            const year_month_day ymd{day};
            const hh_mm_ss hms{now - day};
            DosTime dos;
            const auto year = static_cast<int>(ymd.year());
            if (year < 1980) return dos;
            dos.date = static_cast<std::uint16_t>((static_cast<unsigned>(year - 1980) << 9) | (static_cast<unsigned>(ymd.month()) << 5) | static_cast<unsigned>(ymd.day()));
            dos.time = static_cast<std::uint16_t>((static_cast<unsigned>(hms.hours().count()) << 11) | (static_cast<unsigned>(hms.minutes().count()) << 5) |
                                                  (static_cast<unsigned>(hms.seconds().count()) / 2));
            return dos;
        }

        // A member's local header. Written first with no signature, while its bytes are still
        // arriving, and again with one once they are all there: until then nothing that walks the
        // archive takes it for a member.
        std::string localHeader(const std::string &name, const ZipMember &member, const DosTime dos, const bool complete) {
            std::string out;
            put32(out, complete ? kLocalSignature : 0);
            put16(out, kVersion);
            put16(out, kUtf8Names);
            put16(out, member.method);
            put16(out, dos.time);
            put16(out, dos.date);
            put32(out, member.crc);
            put32(out, kInZip64);
            put32(out, kInZip64);
            put16(out, static_cast<std::uint16_t>(name.size()));
            put16(out, static_cast<std::uint16_t>(kLocalExtraSize));
            out += name;
            put16(out, 1);// Zip64 extended information
            put16(out, 16);
            put64(out, member.size);
            put64(out, member.compressedSize);
            return out;
        }

        std::string centralHeader(const std::string &name, const ZipMember &member, const DosTime dos) {
            std::string out;
            put32(out, kCentralSignature);
            put16(out, kMadeBy);
            put16(out, kVersion);
            put16(out, kUtf8Names);
            put16(out, member.method);
            put16(out, dos.time);
            put16(out, dos.date);
            put32(out, member.crc);
            put32(out, kInZip64);
            put32(out, kInZip64);
            put16(out, static_cast<std::uint16_t>(name.size()));
            put16(out, 28);
            put16(out, 0);// comment
            put16(out, 0);// disk
            put16(out, 0);// internal attributes
            put32(out, kRegularFile);
            put32(out, kInZip64);
            out += name;
            put16(out, 1);
            put16(out, 24);
            put64(out, member.size);
            put64(out, member.compressedSize);
            put64(out, member.offset);
            return out;
        }

        // Zip64 end record, its locator, and the classic end record pointing at both.
        std::string trailer(const std::uint64_t directoryOffset, const std::uint64_t directorySize, const std::uint64_t entries) {
            std::string out;
            put32(out, kZip64EndSignature);
            put64(out, kZip64EndSize - 12);
            put16(out, kMadeBy);
            put16(out, kVersion);
            put32(out, 0);
            put32(out, 0);
            put64(out, entries);
            put64(out, entries);
            put64(out, directorySize);
            put64(out, directoryOffset);

            put32(out, kZip64LocatorSignature);
            put32(out, 0);
            put64(out, directoryOffset + directorySize);
            put32(out, 1);

            put32(out, kEndSignature);
            put16(out, 0);
            put16(out, 0);
            put16(out, 0xffff);
            put16(out, 0xffff);
            put32(out, kInZip64);
            put32(out, kInZip64);
            put16(out, 0);
            return out;
        }

        std::string readAt(std::ifstream &in, const std::uint64_t offset, const std::size_t length) {
            std::string out(length, '\0');
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            in.read(out.data(), static_cast<std::streamsize>(length));
            if (static_cast<std::size_t>(in.gcount()) != length) throw std::runtime_error("short read in zip at " + std::to_string(offset));
            return out;
        }

        struct Local {
            std::string name;
            ZipMember member;
            DosTime dos;
            std::uint64_t dataAt = 0;
        };

        // The member whose local header is at offset, if there is a complete one there.
        std::optional<Local> readLocal(std::ifstream &in, const std::uint64_t offset, const std::uint64_t fileSize) {
            if (offset + kLocalHeaderSize > fileSize) return std::nullopt;
            const auto header = readAt(in, offset, kLocalHeaderSize);
            if (get32(header, 0) != kLocalSignature) return std::nullopt;

            Local local;
            local.member.offset = offset;
            local.member.method = get16(header, 8);
            local.dos.time = get16(header, 10);
            local.dos.date = get16(header, 12);
            local.member.crc = get32(header, 14);
            local.member.compressedSize = get32(header, 18);
            local.member.size = get32(header, 22);
            const std::size_t nameLength = get16(header, 26);
            const std::size_t extraLength = get16(header, 28);
            if (offset + kLocalHeaderSize + nameLength + extraLength > fileSize) return std::nullopt;

            const auto rest = readAt(in, offset + kLocalHeaderSize, nameLength + extraLength);
            local.name = rest.substr(0, nameLength);
            for (std::size_t at = nameLength; at + 4 <= rest.size();) {
                const auto id = get16(rest, at);
                const std::size_t length = get16(rest, at + 2);
                if (at + 4 + length > rest.size()) break;
                if (id == 1) {
                    std::size_t field = at + 4;
                    if (get32(header, 22) == kInZip64 && field + 8 <= at + 4 + length) local.member.size = get64(rest, field), field += 8;
                    if (get32(header, 18) == kInZip64 && field + 8 <= at + 4 + length) local.member.compressedSize = get64(rest, field);
                }
                at += 4 + length;
            }
            local.dataAt = offset + kLocalHeaderSize + nameLength + extraLength;
            if (local.dataAt + local.member.compressedSize > fileSize) return std::nullopt;
            return local;
        }

        // What an archive holds and where the next member goes.
        struct Layout {
            std::uint64_t appendAt = 0;
            std::string directory;
            std::uint64_t entries = 0;
        };

        // Read from the end records, which is how it is found every time but the first after an
        // interrupted append.
        std::optional<Layout> fromDirectory(std::ifstream &in, const std::uint64_t fileSize) {
            if (fileSize < kEndSize + kLocatorSize + kZip64EndSize) return std::nullopt;
            const auto end = readAt(in, fileSize - kEndSize, kEndSize);
            if (get32(end, 0) != kEndSignature) return std::nullopt;
            const auto locator = readAt(in, fileSize - kEndSize - kLocatorSize, kLocatorSize);
            if (get32(locator, 0) != kZip64LocatorSignature) return std::nullopt;
            const auto zip64EndAt = get64(locator, 8);
            if (zip64EndAt + kZip64EndSize > fileSize) return std::nullopt;
            const auto zip64End = readAt(in, zip64EndAt, kZip64EndSize);
            if (get32(zip64End, 0) != kZip64EndSignature) return std::nullopt;

            Layout layout;
            layout.entries = get64(zip64End, 32);
            const auto directorySize = get64(zip64End, 40);
            layout.appendAt = get64(zip64End, 48);
            if (layout.appendAt + directorySize != zip64EndAt) return std::nullopt;
            layout.directory = readAt(in, layout.appendAt, static_cast<std::size_t>(directorySize));
            if (!layout.directory.empty() && get32(layout.directory, 0) != kCentralSignature) return std::nullopt;
            return layout;
        }

        // Rebuilt from the members themselves: every complete one from the start, in order, up to
        // the first that is not - which is where an interrupted append stopped.
        Layout recover(std::ifstream &in, const std::uint64_t fileSize) {
            Layout layout;
            std::uint64_t offset = 0;
            while (const auto local = readLocal(in, offset, fileSize)) {
                layout.directory += centralHeader(local->name, local->member, local->dos);
                ++layout.entries;
                offset = local->dataAt + local->member.compressedSize;
            }
            layout.appendAt = offset;
            if (fileSize > 0) {
                log_warning << "Zip directory recovered from its members, entries: " << layout.entries << ", dropped bytes: " << fileSize - offset;
            }
            return layout;
        }

        Layout layoutOf(const std::filesystem::path &zip) {
            std::error_code ec;
            if (!std::filesystem::exists(zip, ec)) return {};
            std::ifstream in(zip, std::ios::binary);
            if (!in) throw std::runtime_error("cannot open zip " + zip.string());
            const auto fileSize = static_cast<std::uint64_t>(std::filesystem::file_size(zip));
            if (auto layout = fromDirectory(in, fileSize)) return *layout;
            return recover(in, fileSize);
        }

        std::mutex &mutexFor(const std::filesystem::path &zip) {
            static std::mutex guard;
            static std::map<std::string, std::unique_ptr<std::mutex> > mutexes;
            const std::scoped_lock lock(guard);
            auto &entry = mutexes[zip.string()];
            if (!entry) entry = std::make_unique<std::mutex>();
            return *entry;
        }

        std::filesystem::path lockPathOf(const std::filesystem::path &zip) {
            auto path = zip;
            path += ".lock";
            return path;
        }

        // A file lock is held by a process, not a thread, so the threads of one process are kept
        // apart by a mutex of their own first.
        class AppendLock {
          public:
            explicit AppendLock(const std::filesystem::path &zip) : _local(mutexFor(zip)) {
                const auto path = lockPathOf(zip);
                { std::ofstream touch(path, std::ios::app | std::ios::binary); }
                _file = boost::interprocess::file_lock(path.string().c_str());
                _file.lock();
            }
            ~AppendLock() {
                try {
                    _file.unlock();
                } catch (...) {
                }
            }
            AppendLock(const AppendLock &) = delete;
            AppendLock &operator=(const AppendLock &) = delete;

          private:
            std::unique_lock<std::mutex> _local;
            boost::interprocess::file_lock _file;
        };

        // Copies or deflates source into out, from where out is, filling in the member's sizes and CRC.
        void writeData(std::ifstream &source, std::fstream &out, ZipMember &member) {
            std::array<char, kChunk> input{};
            std::array<char, kChunk> output{};

            if (member.method == 0) {
                while (source) {
                    source.read(input.data(), input.size());
                    const auto read = static_cast<std::size_t>(source.gcount());
                    if (read == 0) break;
                    member.crc = static_cast<std::uint32_t>(crc32(member.crc, reinterpret_cast<const Bytef *>(input.data()), static_cast<uInt>(read)));
                    out.write(input.data(), static_cast<std::streamsize>(read));
                    member.size += read;
                }
                member.compressedSize = member.size;
                return;
            }

            z_stream stream{};
            if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                throw std::runtime_error("deflateInit2 failed");
            }
            int flush = Z_NO_FLUSH;
            do {
                source.read(input.data(), input.size());
                const auto read = static_cast<std::size_t>(source.gcount());
                if (source.bad()) {
                    deflateEnd(&stream);
                    throw std::runtime_error("could not read the file being archived");
                }
                member.crc = static_cast<std::uint32_t>(crc32(member.crc, reinterpret_cast<const Bytef *>(input.data()), static_cast<uInt>(read)));
                member.size += read;
                flush = source.eof() ? Z_FINISH : Z_NO_FLUSH;
                stream.next_in = reinterpret_cast<Bytef *>(input.data());
                stream.avail_in = static_cast<uInt>(read);
                do {
                    stream.next_out = reinterpret_cast<Bytef *>(output.data());
                    stream.avail_out = static_cast<uInt>(output.size());
                    deflate(&stream, flush);
                    const auto produced = output.size() - stream.avail_out;
                    out.write(output.data(), static_cast<std::streamsize>(produced));
                    member.compressedSize += produced;
                } while (stream.avail_out == 0);
            } while (flush != Z_FINISH);
            deflateEnd(&stream);
        }

    }// namespace

    ZipMember ArchiveZip::Append(const std::filesystem::path &zip, const std::string &entryName, const std::filesystem::path &source, const bool deflate) {

        if (entryName.empty() || entryName.size() > 0xffff) throw std::runtime_error("zip entry name has to be 1 to 65535 bytes");

        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open " + source.string());

        std::error_code ec;
        if (zip.has_parent_path()) std::filesystem::create_directories(zip.parent_path(), ec);

        const AppendLock lock(zip);
        const auto layout = layoutOf(zip);

        if (!std::filesystem::exists(zip, ec)) {
            std::ofstream create(zip, std::ios::binary);
            if (!create) throw std::runtime_error("cannot create zip " + zip.string());
        }
        std::fstream out(zip, std::ios::in | std::ios::out | std::ios::binary);
        if (!out) throw std::runtime_error("cannot open zip for writing " + zip.string());

        ZipMember member;
        member.offset = layout.appendAt;
        member.method = deflate ? 8 : 0;
        const auto dos = dosNow();

        const auto placeholder = localHeader(entryName, member, dos, false);
        out.seekp(static_cast<std::streamoff>(member.offset));
        out.write(placeholder.data(), static_cast<std::streamsize>(placeholder.size()));
        writeData(input, out, member);
        const auto dataEnd = member.offset + placeholder.size() + member.compressedSize;

        // The header again, now it can say what follows it - and with that, that it is a member.
        const auto header = localHeader(entryName, member, dos, true);
        out.seekp(static_cast<std::streamoff>(member.offset));
        out.write(header.data(), static_cast<std::streamsize>(header.size()));

        const auto directory = layout.directory + centralHeader(entryName, member, dos);
        const auto end = trailer(dataEnd, directory.size(), layout.entries + 1);
        out.seekp(static_cast<std::streamoff>(dataEnd));
        out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
        out.write(end.data(), static_cast<std::streamsize>(end.size()));
        out.flush();
        if (!out) throw std::runtime_error("could not write zip " + zip.string());
        out.close();

        // Anything an interrupted append left beyond the new end goes.
        const auto finalSize = dataEnd + directory.size() + end.size();
        if (std::filesystem::file_size(zip) != finalSize) std::filesystem::resize_file(zip, finalSize);
        return member;
    }

    void ArchiveZip::Extract(const std::filesystem::path &zip, const ZipMember &member, const std::filesystem::path &target) {

        std::ifstream in(zip, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open zip " + zip.string());
        const auto local = readLocal(in, member.offset, static_cast<std::uint64_t>(std::filesystem::file_size(zip)));
        if (!local || local->member.compressedSize != member.compressedSize || local->member.method != member.method) {
            throw std::runtime_error("no such member at offset " + std::to_string(member.offset) + " of " + zip.string());
        }

        // Its own name, so two restores of one object at once do not write into each other.
        auto partial = target;
        partial += "." + std::to_string(std::random_device{}()) + ".partial";
        std::error_code ec;
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);

        try {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + partial.string());

            in.clear();
            in.seekg(static_cast<std::streamoff>(local->dataAt));
            std::array<char, kChunk> input{};
            std::array<char, kChunk> output{};
            std::uint64_t remaining = member.compressedSize, written = 0;
            std::uint32_t crc = 0;

            z_stream stream{};
            if (member.method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) throw std::runtime_error("inflateInit2 failed");
            int status = Z_OK;
            while (remaining > 0 && status != Z_STREAM_END) {
                const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
                in.read(input.data(), static_cast<std::streamsize>(want));
                if (static_cast<std::size_t>(in.gcount()) != want) {
                    if (member.method == 8) inflateEnd(&stream);
                    throw std::runtime_error("zip member is cut short");
                }
                remaining -= want;
                if (member.method == 0) {
                    crc = static_cast<std::uint32_t>(crc32(crc, reinterpret_cast<const Bytef *>(input.data()), static_cast<uInt>(want)));
                    out.write(input.data(), static_cast<std::streamsize>(want));
                    written += want;
                    continue;
                }
                stream.next_in = reinterpret_cast<Bytef *>(input.data());
                stream.avail_in = static_cast<uInt>(want);
                do {
                    stream.next_out = reinterpret_cast<Bytef *>(output.data());
                    stream.avail_out = static_cast<uInt>(output.size());
                    status = inflate(&stream, Z_NO_FLUSH);
                    if (status != Z_OK && status != Z_STREAM_END) {
                        inflateEnd(&stream);
                        throw std::runtime_error("zip member does not inflate");
                    }
                    const auto produced = output.size() - stream.avail_out;
                    crc = static_cast<std::uint32_t>(crc32(crc, reinterpret_cast<const Bytef *>(output.data()), static_cast<uInt>(produced)));
                    out.write(output.data(), static_cast<std::streamsize>(produced));
                    written += produced;
                } while (stream.avail_out == 0 && status != Z_STREAM_END);
            }
            if (member.method == 8) inflateEnd(&stream);
            out.close();
            if (!out) throw std::runtime_error("could not write " + partial.string());
            if (written != member.size || crc != member.crc) throw std::runtime_error("zip member does not match its size or CRC");

            std::filesystem::rename(partial, target);
        } catch (...) {
            std::filesystem::remove(partial, ec);
            throw;
        }
    }

    std::vector<ArchiveZip::Entry> ArchiveZip::List(const std::filesystem::path &zip) {

        const auto layout = layoutOf(zip);
        std::vector<Entry> entries;
        const auto &directory = layout.directory;
        for (std::size_t at = 0; at + kCentralHeaderSize <= directory.size();) {
            if (get32(directory, at) != kCentralSignature) break;
            Entry entry;
            entry.member.method = get16(directory, at + 10);
            entry.member.crc = get32(directory, at + 16);
            entry.member.compressedSize = get32(directory, at + 20);
            entry.member.size = get32(directory, at + 24);
            const std::size_t nameLength = get16(directory, at + 28);
            const std::size_t extraLength = get16(directory, at + 30);
            const std::size_t commentLength = get16(directory, at + 32);
            entry.member.offset = get32(directory, at + 42);
            entry.name = directory.substr(at + kCentralHeaderSize, nameLength);

            const auto extraAt = at + kCentralHeaderSize + nameLength;
            for (std::size_t x = extraAt; x + 4 <= extraAt + extraLength;) {
                const std::size_t length = get16(directory, x + 2);
                if (get16(directory, x) == 1) {
                    std::size_t field = x + 4;
                    if (get32(directory, at + 24) == kInZip64) entry.member.size = get64(directory, field), field += 8;
                    if (get32(directory, at + 20) == kInZip64) entry.member.compressedSize = get64(directory, field), field += 8;
                    if (get32(directory, at + 42) == kInZip64) entry.member.offset = get64(directory, field);
                }
                x += 4 + length;
            }
            entries.push_back(entry);
            at = extraAt + extraLength + commentLength;
        }
        return entries;
    }

    bool ArchiveZip::Remove(const std::filesystem::path &zip) {
        std::error_code ec;
        bool removed = false;
        {
            const AppendLock lock(zip);
            removed = std::filesystem::remove(zip, ec);
        }
        std::filesystem::remove(lockPathOf(zip), ec);
        return removed;
    }

}// namespace Euclid::Core
