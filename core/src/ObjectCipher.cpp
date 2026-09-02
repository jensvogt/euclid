//
// Created by vogje01 on 9/2/26.
//

// C++ includes
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <vector>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/ObjectCipher.h>

namespace Euclid::Core {

    namespace {

        // Identifies a file as one of ours and says which version of the layout it is in, so a
        // format change stays distinguishable from a file that was never encrypted at all.
        constexpr std::string_view kMagic = "EUCOBJ01";

        void appendLe32(std::string &out, const std::uint32_t value) {
            for (int i = 0; i < 4; ++i) out += static_cast<char>(value >> (8 * i) & 0xFF);
        }

        void appendLe64(std::string &out, const std::uint64_t value) {
            for (int i = 0; i < 8; ++i) out += static_cast<char>(value >> (8 * i) & 0xFF);
        }

        std::uint32_t readLe32(const char *data) {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << (8 * i);
            return value;
        }

        std::uint64_t readLe64(const char *data) {
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i);
            return value;
        }

        // What binds a frame to its position: the index is authenticated but not stored, so a
        // frame lifted out of one place in the file and dropped into another no longer verifies.
        std::string frameAad(const std::uint64_t index) {
            std::string aad;
            appendLe64(aad, index);
            return aad;
        }

        std::string makeHeader(const std::uint32_t chunkSize, const std::uint64_t plaintextSize) {
            std::string header;
            header.reserve(ObjectCipher::kHeaderSize);
            header.append(kMagic);
            appendLe32(header, chunkSize);
            appendLe64(header, plaintextSize);
            return header;
        }

        struct Header {
            std::uint32_t chunkSize;
            std::int64_t plaintextSize;
        };

        Header parseHeader(const std::string_view bytes, const std::string &what) {
            if (bytes.size() < ObjectCipher::kHeaderSize || bytes.substr(0, kMagic.size()) != kMagic) {
                throw std::runtime_error("Not an encrypted object: " + what);
            }
            const auto chunkSize = readLe32(bytes.data() + kMagic.size());
            if (chunkSize == 0) throw std::runtime_error("Encrypted object has a zero chunk size: " + what);

            return {.chunkSize = chunkSize,
                    .plaintextSize = static_cast<std::int64_t>(readLe64(bytes.data() + kMagic.size() + 4))};
        }

        Header readHeader(std::ifstream &in, const std::filesystem::path &path) {
            std::string bytes(ObjectCipher::kHeaderSize, '\0');
            in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(in.gcount()));
            return parseHeader(bytes, path.string());
        }

        // How many plaintext bytes the frame at this index holds - every frame but the last holds
        // a full chunk, which is what makes a frame's file offset arithmetic.
        std::int64_t chunkLengthAt(const Header &header, const std::uint64_t index) {
            const auto consumed = static_cast<std::int64_t>(index) * header.chunkSize;
            return std::min<std::int64_t>(header.chunkSize, header.plaintextSize - consumed);
        }

        std::int64_t frameOffsetAt(const Header &header, const std::uint64_t index) {
            return static_cast<std::int64_t>(ObjectCipher::kHeaderSize) +
                   static_cast<std::int64_t>(index) * (header.chunkSize + static_cast<std::int64_t>(ObjectCipher::kFrameOverhead));
        }

        std::ifstream openForRead(const std::filesystem::path &path) {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) throw std::runtime_error("Could not open object file: " + path.string());
            return in;
        }

        // Reads one frame and hands back what it decrypts to.
        std::string readFrame(std::ifstream &in, const std::string &key, const Header &header,
                              const std::uint64_t index, const std::filesystem::path &path) {

            const auto frameLength = chunkLengthAt(header, index) + static_cast<std::int64_t>(ObjectCipher::kFrameOverhead);

            std::string frame(static_cast<std::size_t>(frameLength), '\0');
            in.read(frame.data(), frameLength);
            if (in.gcount() != frameLength) {
                throw std::runtime_error("Encrypted object is truncated: " + path.string());
            }
            return CryptoUtils::AesGcmDecrypt(key, frame, frameAad(index));
        }

        // Streams a stored object's plaintext past `sink` a chunk at a time, whether it is stored
        // encrypted (key non-empty) or in the clear. The one place that knows both cases, so
        // everything reading a stored object reads it the same way.
        std::int64_t streamPlaintext(const std::string &key, const std::filesystem::path &path,
                                     const std::function<void(std::string_view)> &sink) {

            auto in = openForRead(path);

            if (key.empty()) {
                std::string buffer(ObjectCipher::kChunkSize, '\0');
                std::int64_t total = 0;
                while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
                    const auto read = static_cast<std::size_t>(in.gcount());
                    sink(std::string_view(buffer.data(), read));
                    total += static_cast<std::int64_t>(read);
                }
                return total;
            }

            const auto header = readHeader(in, path);
            std::int64_t delivered = 0;
            for (std::uint64_t index = 0; delivered < header.plaintextSize; ++index) {
                const auto chunk = readFrame(in, key, header, index, path);
                delivered += static_cast<std::int64_t>(chunk.size());
                sink(chunk);
            }
            return delivered;
        }

    }// namespace

    std::string ObjectCipher::Encrypt(const std::string &key, const std::string_view plaintext) {

        std::string out = makeHeader(kChunkSize, plaintext.size());
        out.reserve(kHeaderSize + plaintext.size() + (plaintext.size() / kChunkSize + 1) * kFrameOverhead);

        std::uint64_t index = 0;
        for (std::size_t offset = 0; offset < plaintext.size(); offset += kChunkSize, ++index) {
            const auto length = std::min<std::size_t>(kChunkSize, plaintext.size() - offset);
            out += CryptoUtils::AesGcmEncrypt(key, std::string(plaintext.substr(offset, length)), frameAad(index));
        }
        return out;
    }

    std::string ObjectCipher::Decrypt(const std::string &key, const std::string_view encrypted) {

        const auto header = parseHeader(encrypted, "buffer");

        std::string plaintext;
        plaintext.reserve(static_cast<std::size_t>(header.plaintextSize));

        std::size_t position = kHeaderSize;
        for (std::uint64_t index = 0; static_cast<std::int64_t>(plaintext.size()) < header.plaintextSize; ++index) {
            const auto frameLength = static_cast<std::size_t>(chunkLengthAt(header, index)) + kFrameOverhead;
            if (position + frameLength > encrypted.size()) {
                throw std::runtime_error("Encrypted object is truncated");
            }
            plaintext += CryptoUtils::AesGcmDecrypt(key, std::string(encrypted.substr(position, frameLength)), frameAad(index));
            position += frameLength;
        }
        return plaintext;
    }

    bool ObjectCipher::IsEncrypted(const std::filesystem::path &path) {

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        std::string magic(kMagic.size(), '\0');
        in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        return static_cast<std::size_t>(in.gcount()) == kMagic.size() && magic == kMagic;
    }

    std::int64_t ObjectCipher::PlaintextSize(const std::filesystem::path &path) {
        auto in = openForRead(path);
        return readHeader(in, path).plaintextSize;
    }

    std::string ObjectCipher::DecryptFile(const std::string &key, const std::filesystem::path &path) {

        std::string plaintext;
        streamPlaintext(key, path, [&plaintext](const std::string_view chunk) { plaintext.append(chunk); });
        return plaintext;
    }

    std::string ObjectCipher::DecryptRange(const std::string &key, const std::filesystem::path &path,
                                           const std::int64_t offset, const std::int64_t length) {

        if (offset < 0) throw std::runtime_error("Negative offset into object: " + path.string());
        if (length <= 0) return {};

        auto in = openForRead(path);
        const auto header = readHeader(in, path);

        if (offset >= header.plaintextSize) return {};
        const auto wanted = std::min(length, header.plaintextSize - offset);

        // Only the frames the range falls in are touched: the first is the one holding its first
        // byte, the last the one holding its last.
        const auto firstFrame = static_cast<std::uint64_t>(offset / header.chunkSize);
        const auto lastFrame = static_cast<std::uint64_t>((offset + wanted - 1) / header.chunkSize);

        in.seekg(frameOffsetAt(header, firstFrame));

        std::string decoded;
        decoded.reserve(static_cast<std::size_t>((lastFrame - firstFrame + 1) * header.chunkSize));
        for (auto index = firstFrame; index <= lastFrame; ++index) {
            decoded += readFrame(in, key, header, index, path);
        }

        // The frames were whole; the caller asked for a range that starts somewhere inside the
        // first of them and ends somewhere inside the last.
        const auto within = static_cast<std::size_t>(offset - static_cast<std::int64_t>(firstFrame) * header.chunkSize);
        return decoded.substr(within, static_cast<std::size_t>(wanted));
    }

    std::int64_t ObjectCipher::Transcode(const std::string &sourceKey, const std::filesystem::path &source,
                                         const std::string &targetKey, const std::filesystem::path &target) {

        if (targetKey.empty()) {
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) throw std::runtime_error("Could not write object file: " + target.string());

            const auto written = streamPlaintext(sourceKey, source, [&out](const std::string_view chunk) {
                out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            });
            if (!out.good()) throw std::runtime_error("Could not write object file: " + target.string());
            return written;
        }

        ObjectEncryptor encryptor(targetKey, target);
        streamPlaintext(sourceKey, source, [&encryptor](const std::string_view chunk) { encryptor.write(chunk); });
        return encryptor.finish();
    }

    // ── ObjectEncryptor ──────────────────────────────────────────────────────

    ObjectEncryptor::ObjectEncryptor(std::string key, const std::filesystem::path &path)
        : _key(std::move(key)), _out(path, std::ios::binary | std::ios::trunc) {

        if (!_out.is_open()) throw std::runtime_error("Could not write object file: " + path.string());

        // Length still unknown - finish() comes back and overwrites this with what was written.
        const auto header = makeHeader(ObjectCipher::kChunkSize, 0);
        _out.write(header.data(), static_cast<std::streamsize>(header.size()));

        _buffer.reserve(ObjectCipher::kChunkSize);
    }

    void ObjectEncryptor::write(const std::string_view data) {

        auto remaining = data;
        while (!remaining.empty()) {
            const auto take = std::min<std::size_t>(remaining.size(), ObjectCipher::kChunkSize - _buffer.size());
            _buffer.append(remaining.substr(0, take));
            remaining.remove_prefix(take);

            // Flushed only when the chunk is full, never on a short one: a frame shorter than the
            // chunk size is by definition the last one, and the offset arithmetic on the reading
            // side depends on that being true.
            if (_buffer.size() == ObjectCipher::kChunkSize) flushChunk();
        }
    }

    void ObjectEncryptor::flushChunk() {

        const auto frame = CryptoUtils::AesGcmEncrypt(_key, _buffer, frameAad(_frameIndex));
        _out.write(frame.data(), static_cast<std::streamsize>(frame.size()));

        _plaintextSize += static_cast<std::int64_t>(_buffer.size());
        _buffer.clear();
        ++_frameIndex;
    }

    std::int64_t ObjectEncryptor::finish() {

        if (!_buffer.empty()) flushChunk();

        _out.seekp(0);
        const auto header = makeHeader(ObjectCipher::kChunkSize, static_cast<std::uint64_t>(_plaintextSize));
        _out.write(header.data(), static_cast<std::streamsize>(header.size()));
        _out.flush();

        if (!_out.good()) throw std::runtime_error("Could not write encrypted object");

        _out.close();
        return _plaintextSize;
    }

}// namespace Euclid::Core
