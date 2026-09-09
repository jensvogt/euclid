#define BOOST_TEST_MODULE ContentTypeTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/ContentTypeUtils.h>

using Euclid::Core::Configuration;
using Euclid::Core::ContentTypeUtils;

// ESM sniffs an object's content type from its first bytes rather than from the name it was stored
// under, which is right for every format libmagic recognizes from the magic bytes at its start and
// wrong for the ones it recognizes by parsing the whole document. JSON is the second kind: given
// the first 2000 bytes of a 6 kB document libmagic sees text, so every JSON object an application
// wrote through upload-file was stored as text/plain. What has to hold now is that the content
// still wins wherever it knows something, and the key is asked only where it does not.

namespace {

    // libmagic is given no database of its own here the way a running module is
    // (euclid.magic-file), and the one compiled into the library points wherever it was built, so
    // it has to be told. Without it every sniff comes back application/octet-stream and the cases
    // below would be testing nothing - hence magicLoaded(), which says so rather than passing.
    struct MagicFixture {

        MagicFixture() {
            for (const auto *candidate: {"/usr/local/euclid/etc/magic.mgc", "/usr/share/misc/magic.mgc", "/usr/share/file/magic.mgc"}) {
                if (std::filesystem::exists(candidate)) {
                    Configuration::instance().set<std::string>("euclid.magic-file", candidate);
                    break;
                }
            }
        }
    };

    // Whether this machine has a magic database that recognizes the formats these cases turn on.
    // JSON is the one that matters and it arrived in file 5.35, so an installation older than that
    // cannot detect a JSON object however much of it is sniffed - which is the other half of why
    // ESM has to be able to fall back to the key.
    bool magicLoaded() {
        return ContentTypeUtils::fromContent(R"({"probe":1})") == "application/json";
    }

    // A JSON document longer than the prefix ESM sniffs, so that what libmagic is given is not
    // parseable JSON - the case that produced text/plain.
    std::string largeJson() {
        std::string json = R"({"items":[)";
        for (int i = 0; i < 200; ++i) {
            if (i > 0) json += ",";
            json += R"({"id":)" + std::to_string(i) + R"(,"name":"object-)" + std::to_string(i) + R"("})";
        }
        return json + "]}";
    }

    // A 1x1 PNG's header: the signature and the IHDR chunk that follows it, which is as much as
    // libmagic needs (and as much as it gets, since ESM sniffs a prefix). NUL bytes throughout, so
    // it is built with an explicit length rather than read up to a terminator.
    std::string pngHeader() {
        constexpr unsigned char bytes[] = {
                0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
                0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                0x08, 0x06, 0x00, 0x00, 0x00,
                0x1f, 0x15, 0xc4, 0x89};
        return {reinterpret_cast<const char *>(bytes), sizeof(bytes)};
    }

}// namespace

BOOST_FIXTURE_TEST_SUITE(ContentTypeTest, MagicFixture)

    BOOST_AUTO_TEST_CASE(FromKeyMapsKnownExtensions) {
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("results.json"), "application/json");
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("2026/q3/report.CSV"), "text/csv");
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("a.b.c/index.html"), "text/html");
    }

    BOOST_AUTO_TEST_CASE(FromKeyDeclinesWhatItCannotName) {
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("results"), "");
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("archive.tar.unknown"), "");
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("results."), "");
        // A dot in a directory rather than in the name is not the object's extension.
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey("v1.2/results"), "");
        // A hidden file is a name that starts with a dot, not an extension without a name.
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromKey(".json"), "");
    }

    // The bug: a JSON document too large to be recognized from its prefix, stored under a key that
    // says what it is.
    BOOST_AUTO_TEST_CASE(DetectFallsBackToTheKeyWhenTheContentSaysNothing) {
        const auto json = largeJson();

        // The key is what has to carry this whichever way the sniff failed - as text/plain where
        // there is a magic database, as application/octet-stream where there is not.
        BOOST_CHECK_EQUAL(ContentTypeUtils::detect(json.substr(0, 2000), "jvo_0000091471eb45b798.json"), "application/json");

        if (!magicLoaded()) return;
        BOOST_CHECK_EQUAL(ContentTypeUtils::fromContent(json.substr(0, 2000)), "text/plain");
    }

    // ... and the same document, short enough for libmagic to parse: the content already knew, and
    // the answer does not depend on which of the two got there first.
    BOOST_AUTO_TEST_CASE(DetectKeepsWhatTheContentRecognizes) {
        if (!magicLoaded()) return;

        constexpr auto json = R"({"order":17})";
        BOOST_CHECK_EQUAL(ContentTypeUtils::detect(json, "order.json"), "application/json");
        BOOST_CHECK_EQUAL(ContentTypeUtils::detect(json, "order"), "application/json");
    }

    // A key can be named anything, so it must not be able to relabel content that identified
    // itself - an object whose bytes are a PNG stays a PNG.
    BOOST_AUTO_TEST_CASE(DetectDoesNotLetTheKeyOverrideRecognizedContent) {
        if (!magicLoaded()) return;

        BOOST_CHECK_EQUAL(ContentTypeUtils::detect(pngHeader(), "not-really.json"), "image/png");
    }

    // Neither knew: text is still more than nothing.
    BOOST_AUTO_TEST_CASE(DetectKeepsPlainTextWhenTheKeyAddsNothing) {
        if (!magicLoaded()) return;

        BOOST_CHECK_EQUAL(ContentTypeUtils::detect("just some prose, and nothing more", "notes"), "text/plain");
    }

BOOST_AUTO_TEST_SUITE_END()
