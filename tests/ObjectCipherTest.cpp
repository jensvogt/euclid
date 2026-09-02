#define BOOST_TEST_MODULE ObjectCipherTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/ObjectCipher.h>

using Euclid::Core::CryptoUtils;
using Euclid::Core::ObjectCipher;
using Euclid::Core::ObjectEncryptor;

// An object stored with encryption at rest is cut into fixed-size chunks so that a multipart
// download can read one byte range without decrypting everything before it. That framing is the
// thing worth testing: what a client stored has to come back byte for byte, whether it is read
// whole or a range at a time, whether it went to disk in one piece or was streamed in - and it has
// to stop coming back at all once anyone has moved the frames around.

namespace {

    constexpr auto kChunk = static_cast<std::size_t>(ObjectCipher::kChunkSize);

    std::string key() {
        return CryptoUtils::Base64Decode(CryptoUtils::GenerateAes256Key());
    }

    // Deterministic filler with no repeating byte pattern at chunk boundaries, so a frame served
    // from the wrong offset cannot pass for the right one.
    std::string data(const std::size_t length) {
        std::string out;
        out.reserve(length);
        for (std::size_t i = 0; i < length; ++i) out += static_cast<char>((i * 31 + i / 251) & 0xFF);
        return out;
    }

    // A path under the system temp directory, removed by the fixture that owns it.
    struct TempFile {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("euclid-object-cipher-" + CryptoUtils::md5Sum(std::to_string(std::rand())).substr(0, 16));
        ~TempFile() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    void writeFile(const std::filesystem::path &path, const std::string &content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::string readFile(const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator(in), std::istreambuf_iterator<char>()};
    }

}// namespace

BOOST_AUTO_TEST_CASE(RoundTripsEveryShapeOfObject) {

    const auto aesKey = key();

    // Empty, well under one chunk, exactly one chunk, one chunk plus a byte, and several chunks
    // with a partial one at the end - the sizes at which the framing decides how many frames there
    // are and how long the last one is.
    for (const std::size_t size: {std::size_t{0}, std::size_t{1}, kChunk - 1, kChunk, kChunk + 1, 3 * kChunk + 17}) {
        const auto plaintext = data(size);
        const auto encrypted = ObjectCipher::Encrypt(aesKey, plaintext);

        BOOST_TEST_INFO("size " << size);
        BOOST_TEST(ObjectCipher::Decrypt(aesKey, encrypted) == plaintext);
    }
}

BOOST_AUTO_TEST_CASE(StoredObjectIsNotThePlaintext) {

    const auto aesKey = key();
    const auto plaintext = data(4096);
    const auto encrypted = ObjectCipher::Encrypt(aesKey, plaintext);

    BOOST_TEST(encrypted.find(plaintext) == std::string::npos);
    BOOST_TEST(encrypted.size() == ObjectCipher::kHeaderSize + plaintext.size() + ObjectCipher::kFrameOverhead);
}

BOOST_AUTO_TEST_CASE(AnotherKeyDoesNotOpenIt) {

    const auto encrypted = ObjectCipher::Encrypt(key(), data(1000));

    BOOST_CHECK_THROW(auto opened = ObjectCipher::Decrypt(key(), encrypted), std::exception);
}

BOOST_AUTO_TEST_CASE(FramesCannotBeReordered) {

    // Two frames of identical length, encrypted under one key: without the frame index bound into
    // each frame's authentication, swapping them would decrypt cleanly and hand back the object
    // with its halves exchanged.
    const auto aesKey = key();
    const auto plaintext = data(2 * kChunk);
    const auto encrypted = ObjectCipher::Encrypt(aesKey, plaintext);

    constexpr auto frameLength = kChunk + ObjectCipher::kFrameOverhead;
    std::string swapped = encrypted.substr(0, ObjectCipher::kHeaderSize);
    swapped += encrypted.substr(ObjectCipher::kHeaderSize + frameLength, frameLength);
    swapped += encrypted.substr(ObjectCipher::kHeaderSize, frameLength);

    BOOST_TEST_REQUIRE(swapped.size() == encrypted.size());
    BOOST_CHECK_THROW(auto opened = ObjectCipher::Decrypt(aesKey, swapped), std::exception);
}

BOOST_AUTO_TEST_CASE(TruncationIsNoticed) {

    const auto aesKey = key();
    const auto encrypted = ObjectCipher::Encrypt(aesKey, data(2 * kChunk));

    // A whole frame cut off the end: every remaining frame still authenticates, and only the
    // plaintext length in the header says the object is short.
    const auto truncated = encrypted.substr(0, encrypted.size() - (kChunk + ObjectCipher::kFrameOverhead));

    BOOST_CHECK_THROW(auto opened = ObjectCipher::Decrypt(aesKey, truncated), std::exception);
}

BOOST_AUTO_TEST_CASE(APlainFileIsNotMistakenForAnEncryptedOne) {

    const TempFile file;
    writeFile(file.path, data(100));

    BOOST_TEST(!ObjectCipher::IsEncrypted(file.path));

    writeFile(file.path, ObjectCipher::Encrypt(key(), data(100)));
    BOOST_TEST(ObjectCipher::IsEncrypted(file.path));
    BOOST_TEST(ObjectCipher::PlaintextSize(file.path) == 100);
}

BOOST_AUTO_TEST_CASE(ReadsAnyRangeOfAStoredObject) {

    const auto aesKey = key();
    const auto plaintext = data(3 * kChunk + 1234);

    const TempFile file;
    writeFile(file.path, ObjectCipher::Encrypt(aesKey, plaintext));

    // Ranges inside one frame, spanning a boundary, spanning several frames, starting at zero and
    // ending at the last byte - a multipart download asks for all of these depending on its part
    // size, and none of them line up with the framing on purpose.
    const std::vector<std::pair<std::int64_t, std::int64_t>> ranges = {
            {0, 10},
            {0, static_cast<std::int64_t>(plaintext.size())},
            {5, static_cast<std::int64_t>(kChunk)},
            {static_cast<std::int64_t>(kChunk) - 3, 7},
            {static_cast<std::int64_t>(kChunk), static_cast<std::int64_t>(kChunk)},
            {static_cast<std::int64_t>(2 * kChunk) + 11, static_cast<std::int64_t>(kChunk) + 500},
            {static_cast<std::int64_t>(plaintext.size()) - 1, 1},
    };

    for (const auto &[offset, length]: ranges) {
        BOOST_TEST_INFO("offset " << offset << " length " << length);
        BOOST_TEST(ObjectCipher::DecryptRange(aesKey, file.path, offset, length) == plaintext.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)));
    }

    // A range running past the end is truncated to what is there, and one starting past it is
    // empty - the same answer a plain read() gives.
    const auto tail = ObjectCipher::DecryptRange(aesKey, file.path, static_cast<std::int64_t>(plaintext.size()) - 5, 100);
    BOOST_TEST(tail == plaintext.substr(plaintext.size() - 5));
    BOOST_TEST(ObjectCipher::DecryptRange(aesKey, file.path, static_cast<std::int64_t>(plaintext.size()), 10).empty());
}

BOOST_AUTO_TEST_CASE(StreamedInPiecesIsTheSameObject) {

    // What complete-upload does: the plaintext arrives in whatever sizes the parts happen to be
    // read in, never as a whole, and the result has to be exactly what encrypting it in one go
    // would have produced - byte-identical is too strong (every frame carries a fresh IV), so what
    // has to hold is that it decrypts to the same thing.
    const auto aesKey = key();
    const auto plaintext = data(2 * kChunk + 999);

    const TempFile file;
    {
        ObjectEncryptor encryptor(aesKey, file.path);
        for (std::size_t offset = 0; offset < plaintext.size(); offset += 7777) {
            encryptor.write(std::string_view(plaintext).substr(offset, std::min<std::size_t>(7777, plaintext.size() - offset)));
        }
        BOOST_TEST(encryptor.finish() == static_cast<std::int64_t>(plaintext.size()));
    }

    BOOST_TEST(ObjectCipher::PlaintextSize(file.path) == static_cast<std::int64_t>(plaintext.size()));
    BOOST_TEST(ObjectCipher::DecryptFile(aesKey, file.path) == plaintext);
    BOOST_TEST(ObjectCipher::DecryptRange(aesKey, file.path, static_cast<std::int64_t>(kChunk) + 3, 2000) == plaintext.substr(kChunk + 3, 2000));
}

BOOST_AUTO_TEST_CASE(StreamsAnEmptyObject) {

    const auto aesKey = key();

    const TempFile file;
    {
        ObjectEncryptor encryptor(aesKey, file.path);
        BOOST_TEST(encryptor.finish() == 0);
    }

    BOOST_TEST(std::filesystem::file_size(file.path) == ObjectCipher::kHeaderSize);
    BOOST_TEST(ObjectCipher::DecryptFile(aesKey, file.path).empty());
}

BOOST_AUTO_TEST_CASE(TranscodesBetweenEveryCombinationOfKeys) {

    // The four ways a copy or a move can cross an encryption boundary: into an encrypted bucket,
    // out of one, between two under different keys, and between two that encrypt nothing.
    const auto first = key();
    const auto second = key();
    const auto plaintext = data(kChunk + 4321);

    const TempFile plain;
    const TempFile encrypted;
    const TempFile target;

    writeFile(plain.path, plaintext);
    writeFile(encrypted.path, ObjectCipher::Encrypt(first, plaintext));

    BOOST_TEST(ObjectCipher::Transcode({}, plain.path, second, target.path) == static_cast<std::int64_t>(plaintext.size()));
    BOOST_TEST(ObjectCipher::DecryptFile(second, target.path) == plaintext);

    BOOST_TEST(ObjectCipher::Transcode(first, encrypted.path, {}, target.path) == static_cast<std::int64_t>(plaintext.size()));
    BOOST_TEST(readFile(target.path) == plaintext);

    BOOST_TEST(ObjectCipher::Transcode(first, encrypted.path, second, target.path) == static_cast<std::int64_t>(plaintext.size()));
    BOOST_TEST(ObjectCipher::DecryptFile(second, target.path) == plaintext);
    BOOST_CHECK_THROW(auto opened = ObjectCipher::DecryptFile(first, target.path), std::exception);

    BOOST_TEST(ObjectCipher::Transcode({}, plain.path, {}, target.path) == static_cast<std::int64_t>(plaintext.size()));
    BOOST_TEST(readFile(target.path) == plaintext);
}

BOOST_AUTO_TEST_CASE(DigestsDataThatIsNeverAllInMemory) {

    // The checksum an encrypted multipart upload reports has to be the one the client would compute
    // over its local file, not over anything ESM writes to disk.
    const auto plaintext = data(200000);

    Euclid::Core::Md5Digest digest;
    for (std::size_t offset = 0; offset < plaintext.size(); offset += 4096) {
        digest.update(std::string_view(plaintext).substr(offset, std::min<std::size_t>(4096, plaintext.size() - offset)));
    }

    BOOST_TEST(digest.hex() == CryptoUtils::md5Sum(plaintext));
}
