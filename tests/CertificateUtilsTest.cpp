#define BOOST_TEST_MODULE CertificateUtilsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/core/CertificateUtils.h>

using Euclid::Core::CertificateUtils;

// A gateway listener that speaks HTTPS is only as good as the certificate it was given, and an
// installation that was never given one generates its own. What is worth testing is that the
// generated certificate is a real certificate - it parses, it is valid now and not in 1970, it
// carries the names a client will check it against - and that an imported pair is refused when the
// key is not the certificate's, since that mismatch is otherwise invisible until every caller's
// handshake fails.

BOOST_AUTO_TEST_SUITE(CertificateUtilsTest)

    BOOST_AUTO_TEST_CASE(GeneratedCertificateIsReadable) {

        const auto pair = CertificateUtils::GenerateSelfSigned("gateway.example.com", {"localhost", "127.0.0.1"}, 30);

        BOOST_CHECK(pair.certificate.starts_with("-----BEGIN CERTIFICATE-----"));
        BOOST_CHECK(pair.privateKey.contains("PRIVATE KEY"));

        const auto info = CertificateUtils::Inspect(pair.certificate);
        BOOST_REQUIRE(info.has_value());
        BOOST_CHECK(info->subject.contains("gateway.example.com"));
        BOOST_CHECK_EQUAL(info->subject, info->issuer);
        BOOST_CHECK(info->selfSigned);
        BOOST_CHECK_EQUAL(info->fingerprint.size(), 64);
        BOOST_CHECK(!info->serialNumber.empty());
    }

    BOOST_AUTO_TEST_CASE(GeneratedCertificateCarriesEveryNameItWasGiven) {

        const auto pair = CertificateUtils::GenerateSelfSigned("gateway.example.com", {"localhost", "127.0.0.1"});
        const auto info = CertificateUtils::Inspect(pair.certificate);
        BOOST_REQUIRE(info.has_value());

        // The common name among them too: a client that checks host names looks only at the
        // alternative names, so a certificate that listed it only as the subject would match
        // nothing at all.
        BOOST_CHECK(std::ranges::find(info->subjectAltNames, "DNS:gateway.example.com") != info->subjectAltNames.end());
        BOOST_CHECK(std::ranges::find(info->subjectAltNames, "DNS:localhost") != info->subjectAltNames.end());

        // An address has to be stored as one. Listed as a DNS name it would be rejected by a
        // client dialling 127.0.0.1, which is exactly how a development installation is reached.
        BOOST_CHECK(std::ranges::find(info->subjectAltNames, "IP:127.0.0.1") != info->subjectAltNames.end());
    }

    BOOST_AUTO_TEST_CASE(GeneratedCertificateIsValidForTheDaysAskedFor) {

        const auto now = std::chrono::system_clock::now();
        const auto pair = CertificateUtils::GenerateSelfSigned("localhost", {}, 30);
        const auto info = CertificateUtils::Inspect(pair.certificate);
        BOOST_REQUIRE(info.has_value());

        BOOST_CHECK(info->notBefore <= now + std::chrono::minutes(1));
        BOOST_CHECK(info->notAfter > now + std::chrono::hours(24 * 29));
        BOOST_CHECK(info->notAfter < now + std::chrono::hours(24 * 31));
    }

    BOOST_AUTO_TEST_CASE(KeyMatchesOnlyItsOwnCertificate) {

        const auto first = CertificateUtils::GenerateSelfSigned("first.example.com");
        const auto second = CertificateUtils::GenerateSelfSigned("second.example.com");

        BOOST_CHECK(CertificateUtils::KeyMatches(first.certificate, first.privateKey));
        BOOST_CHECK(!CertificateUtils::KeyMatches(first.certificate, second.privateKey));
    }

    BOOST_AUTO_TEST_CASE(NonsenseIsNotACertificate) {

        BOOST_CHECK(!CertificateUtils::Inspect("").has_value());
        BOOST_CHECK(!CertificateUtils::Inspect("-----BEGIN CERTIFICATE-----\nnot base64 at all\n-----END CERTIFICATE-----\n").has_value());

        // The private key half pasted into the certificate field: a plausible mistake, and one
        // that has to be caught where the import happens rather than at the next start-up.
        const auto pair = CertificateUtils::GenerateSelfSigned("localhost");
        BOOST_CHECK(!CertificateUtils::Inspect(pair.privateKey).has_value());
        BOOST_CHECK(!CertificateUtils::KeyMatches(pair.certificate, pair.certificate));
    }

BOOST_AUTO_TEST_SUITE_END()
