//
// Created by vogje01 on 9/2/26.
//

#pragma once

// C++ includes
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace Euclid::Core {

    /**
     * @brief The on-disk format of an object stored encrypted at rest.
     *
     * @par
     * Encrypting a stored object with one AES-GCM operation over the whole thing would be simpler,
     * and would also mean that reading its last megabyte requires decrypting every megabyte before
     * it. A stored object is not read that way: a multipart download asks for one byte range at a
     * time, by part number, and expects to pay for that part and nothing more. So the plaintext is
     * cut into fixed-size chunks and each is encrypted on its own, which keeps a byte range's cost
     * proportional to the range - the chunk size is fixed, so the file offset of the chunk holding
     * any plaintext offset is arithmetic rather than a search.
     *
     * @par
     * The layout is a 20-byte header - the magic "EUCOBJ01", the chunk size, and the plaintext
     * length - followed by one frame per chunk, each of them the IV || ciphertext || tag that
     * CryptoUtils::AesGcmEncrypt() returns. Every frame but the last holds exactly one chunk of
     * plaintext, which is what makes the offsets computable; a zero-length object is a header and
     * no frames at all. The chunk size is recorded rather than assumed, so objects written under
     * one setting stay readable if the default ever changes.
     *
     * @par
     * Each frame is authenticated with its own index as additional authenticated data, so the
     * frames cannot be reordered, duplicated or swapped between objects without the tag check
     * failing - GCM on its own would authenticate each frame's contents while saying nothing about
     * where the frame belongs. The header's plaintext length is what makes a truncation - whole
     * frames cut off the end, which no per-frame tag would notice - detectable.
     *
     * @par
     * The key is raw AES key material (16 or 32 bytes), not a key ERN and not base64: which key an
     * object is under is the caller's business to record and look up, and by the time the bytes
     * get here the decision has been made.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ObjectCipher {
    public:

        /**
         * @brief Plaintext bytes per frame.
         *
         * @par
         * Small enough that a ranged read decrypts little more than it was asked for, large enough
         * that the per-frame overhead is noise: at 64 KiB the 28 bytes a frame adds are 0.04% of
         * the stored size.
         */
        static constexpr std::uint32_t kChunkSize = 64 * 1024;

        /**
         * @brief Magic (8) + chunk size (4) + plaintext length (8).
         */
        static constexpr std::size_t kHeaderSize = 20;

        /**
         * @brief What a frame adds to its chunk: a 12-byte IV and a 16-byte GCM tag.
         */
        static constexpr std::size_t kFrameOverhead = 28;

        /**
         * @brief Encrypts a whole buffer.
         *
         * @param key raw AES key bytes (16 or 32).
         * @param plaintext data to encrypt; may be empty.
         * @return the encrypted object, header and all, ready to be written to disk as-is.
         */
        static std::string Encrypt(const std::string &key, std::string_view plaintext);

        /**
         * @brief Decrypts a whole buffer produced by Encrypt().
         *
         * @param key raw AES key bytes (16 or 32).
         * @param encrypted an encrypted object, header and all.
         * @return the recovered plaintext.
         * @throws std::runtime_error if the header is not one of ours, the file is truncated, or
         * any frame fails to authenticate.
         */
        static std::string Decrypt(const std::string &key, std::string_view encrypted);

        /**
         * @brief Whether a file starts with the encrypted-object magic.
         *
         * @par
         * Whether an object is encrypted is recorded in the database - this exists for the case
         * where that record and the file disagree, so the mismatch can be reported rather than
         * handed to a decryption that would fail with something less informative.
         *
         * @param path file to look at.
         * @return true if the file is an encrypted object.
         */
        static bool IsEncrypted(const std::filesystem::path &path);

        /**
         * @brief Reads the plaintext length recorded in an encrypted object's header.
         *
         * @param path encrypted object file.
         * @return the length, in bytes, of what the file decrypts to.
         * @throws std::runtime_error if the file cannot be opened or is not an encrypted object.
         */
        static std::int64_t PlaintextSize(const std::filesystem::path &path);

        /**
         * @brief Decrypts an entire encrypted object file.
         *
         * @par
         * The whole plaintext ends up in memory, so this is for objects small enough to be served
         * in one response; anything larger is read a range at a time with DecryptRange().
         *
         * @par
         * Named for the "whole" rather than the obvious DecryptFile because winbase.h defines
         * DecryptFile as a macro expanding to DecryptFileA/DecryptFileW. A translation unit that
         * reaches windows.h - which on Windows is most of them - would call DecryptFileA here while
         * ObjectCipher.cpp went on defining DecryptFile, and the two would only fail to meet at
         * link time. It also happens to say what DecryptRange() does not.
         *
         * @param key raw AES key bytes (16 or 32).
         * @param path encrypted object file.
         * @return the recovered plaintext.
         * @throws std::runtime_error if the file cannot be opened or does not authenticate.
         */
        static std::string DecryptWholeFile(const std::string &key, const std::filesystem::path &path);

        /**
         * @brief Decrypts one byte range of an encrypted object file.
         *
         * @par
         * Only the frames the range falls in are read and decrypted, so a part of a large object
         * costs what the part costs. The range is expressed in plaintext bytes - the caller works
         * in the object's own coordinates and never sees the framing.
         *
         * @param key raw AES key bytes (16 or 32).
         * @param path encrypted object file.
         * @param offset plaintext byte offset to start at.
         * @param length number of plaintext bytes wanted; a range running past the end of the
         * object is truncated rather than refused, matching a plain read().
         * @return the recovered plaintext for that range, empty if the offset is at or past the
         * end of the object.
         * @throws std::runtime_error if the file cannot be opened, the offset is negative, or a
         * frame the range falls in does not authenticate.
         */
        static std::string DecryptRange(const std::string &key, const std::filesystem::path &path, std::int64_t offset, std::int64_t length);

        /**
         * @brief Rewrites an object file from one key to another, streaming.
         *
         * @par
         * Either key may be empty, meaning "stored in the clear on that side", so this covers
         * encrypting a plaintext object, decrypting an encrypted one, re-encrypting under a
         * different key, and copying a plaintext object as-is. What it never does is hold the
         * whole object in memory - it is the path a copy between buckets with different encryption
         * takes, and objects are as large as they are.
         *
         * @param sourceKey raw AES key bytes the source is under, or empty if it is plaintext.
         * @param source file to read.
         * @param targetKey raw AES key bytes to write under, or empty to write plaintext.
         * @param target file to write; truncated if it exists.
         * @return the number of plaintext bytes transferred.
         * @throws std::runtime_error if either file cannot be opened, or the source does not
         * authenticate.
         */
        static std::int64_t Transcode(const std::string &sourceKey, const std::filesystem::path &source,
                                      const std::string &targetKey, const std::filesystem::path &target);
    };

    /**
     * @brief Writes an encrypted object file from plaintext handed over in pieces.
     *
     * @par
     * ObjectCipher::Encrypt() needs the whole plaintext at once, which is fine for an object that
     * arrived in a single request and is already in memory. This is for the one that did not: a
     * multipart upload is assembled from staged parts, and the assembled plaintext is never meant
     * to exist as a whole anywhere - not in memory, and above all not on disk, since a file
     * written in the clear and encrypted afterwards has been in the clear.
     *
     * @par
     * The header is written twice: once on construction, with the length still unknown, and once
     * by finish(), which seeks back and fills in what was actually written. So the target has to
     * be a real file, which is what it always is here.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ObjectEncryptor {
    public:

        /**
         * @brief Opens path for writing and lays down the header.
         *
         * @param key raw AES key bytes (16 or 32).
         * @param path file to write; truncated if it exists.
         */
        ObjectEncryptor(std::string key, const std::filesystem::path &path);

        ObjectEncryptor(const ObjectEncryptor &) = delete;
        ObjectEncryptor &operator=(const ObjectEncryptor &) = delete;

        /**
         * @brief Adds the next piece of plaintext.
         *
         * @par
         * Piece boundaries have nothing to do with frame boundaries - data is buffered until a
         * whole chunk is there - so a caller streams in whatever sizes it happens to read.
         *
         * @param data plaintext bytes; may be empty.
         */
        void write(std::string_view data);

        /**
         * @brief Flushes the trailing partial chunk and completes the header.
         *
         * @return the number of plaintext bytes written.
         * @throws std::runtime_error if the file could not be written.
         */
        std::int64_t finish();

        /**
         * @brief Whether every write so far has succeeded.
         */
        [[nodiscard]]
        bool good() const { return _out.good(); }

    private:

        /**
         * @brief Encrypts whatever is buffered as the next frame and writes it out.
         */
        void flushChunk();

        std::string _key;
        std::ofstream _out;
        std::string _buffer;
        std::uint64_t _frameIndex = 0;
        std::int64_t _plaintextSize = 0;
    };

}// namespace Euclid::Core
