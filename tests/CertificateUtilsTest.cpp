// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE CertificateUtilsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
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

    // ── EnsureServerCertificate ──────────────────────────────────────────────────────────────
    //
    // What the manager calls on every start, so the case that matters most is the one that does
    // nothing: a certificate an operator put in place must survive every restart untouched.

    namespace {

        // The certificate every release shipped until it stopped: CN=localhost and no subject
        // alternative names at all. Its key is not needed - only the certificate is read.
        const std::string kShippedCertificate =
                "-----BEGIN CERTIFICATE-----\n"
                "MIIDCTCCAfGgAwIBAgIUEY+As0yi9U1WBFReZsWoXbPxGu8wDQYJKoZIhvcNAQEL\n"
                "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgxODExMDEzN1oXDTI3MDgx\n"
                "ODExMDEzN1owFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
                "AAOCAQ8AMIIBCgKCAQEA2m7178f1bojG3y6b5P7YTsx0eWlB99WYuTFoWJYIVKA5\n"
                "qh8hrT+nrnBf5fALrp4lYWhR5UHfSizZ2DKDtIe2WG92hPelDKXRPH1bFQ24Zzl9\n"
                "kbNu+D7oahsFaUBOzP44XBQ9Gk5EFa3jTvBrLmVbbB3y3buQaQ6xK/DHXWo/RpuM\n"
                "4Uyz3/iQNc4xMrQmBDrWuWFyrN9cdohAQI88YpoNSA7Suftt+nh5WUbyl33N6NPP\n"
                "n2e+xb2aUegsDiljfQU6+eFRpgnyscj4nzi8kZ8MxUv0rA1zOfRuSnbiEHnJuHfg\n"
                "mKEGR1agbe85sgFUwVdYppyoWEC7IxQbh2hoXAyPnwIDAQABo1MwUTAdBgNVHQ4E\n"
                "FgQUHUux6EV/L0cZ5yg0RyHm9euCtnIwHwYDVR0jBBgwFoAUHUux6EV/L0cZ5yg0\n"
                "RyHm9euCtnIwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAhete\n"
                "oK6h0NWrwlZOfiM0qw0pyaxwNIeWdgOu7rsoDkiHbn8jci6WU1P5Ac35vM4EKz7Q\n"
                "TYvrr41xKO9rJT5ifsG1ulIqUZkXi4hF9//lsbESTjplW/I4hLgFgXR/i+ux5F0S\n"
                "Uy/7zXI0CuJ7ONqJLpgmkOY4zr7fxPkdP6NDYYFfqrNClDpChpwoYFLzamGyMOVz\n"
                "qJ8moSV+d8EiMhhbKAACPI9da4eRNECiBPEsN85K5yYrIzbrPe1RMbHlgKIIsyGj\n"
                "yp6zm+CK029DvOcV9DeXy/hUdQBW6bUJJTKL9nB5qea1JacKGPsCCMxXB2H33b1T\n"
                "7b1R31hGHfTBKxPs1Q==\n"
                "-----END CERTIFICATE-----\n";

        struct TempDir {
            std::filesystem::path path;
            TempDir() : path(std::filesystem::temp_directory_path() / ("euclid-cert-test-" + std::to_string(std::random_device{}()))) {
                std::filesystem::create_directories(path);
            }
            ~TempDir() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        };

        std::string slurp(const std::filesystem::path &path) {
            std::ifstream in(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        void spit(const std::filesystem::path &path, const std::string &content) {
            std::ofstream(path, std::ios::binary) << content;
        }

    }// namespace

    BOOST_AUTO_TEST_CASE(EnsureGeneratesWhenThereIsNone) {

        const TempDir dir;
        const auto certificate = (dir.path / "etc" / "gateway.crt").string();
        const auto key = (dir.path / "etc" / "gateway.key").string();

        const auto result = CertificateUtils::EnsureServerCertificate(certificate, key, "host.example", {"localhost", "192.168.1.10"});

        BOOST_CHECK(result.action == Euclid::Core::ServerCertificateResult::Action::Generated);
        BOOST_REQUIRE(result.info.has_value());
        BOOST_CHECK(std::ranges::find(result.info->subjectAltNames, "DNS:host.example") != result.info->subjectAltNames.end());
        BOOST_CHECK(std::ranges::find(result.info->subjectAltNames, "IP:192.168.1.10") != result.info->subjectAltNames.end());
        BOOST_CHECK(CertificateUtils::KeyMatches(slurp(certificate), slurp(key)));
    }

    BOOST_AUTO_TEST_CASE(EnsureLeavesAWorkingCertificateAlone) {

        const TempDir dir;
        const auto certificate = (dir.path / "gateway.crt").string();
        const auto key = (dir.path / "gateway.key").string();
        const auto existing = CertificateUtils::GenerateSelfSigned("operator.example");
        spit(certificate, existing.certificate);
        spit(key, existing.privateKey);

        const auto result = CertificateUtils::EnsureServerCertificate(certificate, key, "host.example", {"localhost"});

        BOOST_CHECK(result.action == Euclid::Core::ServerCertificateResult::Action::Kept);
        BOOST_CHECK_EQUAL(slurp(certificate), existing.certificate);
        BOOST_CHECK_EQUAL(slurp(key), existing.privateKey);
    }

    BOOST_AUTO_TEST_CASE(EnsureReplacesTheShippedCertificate) {

        const TempDir dir;
        const auto certificate = (dir.path / "gateway.crt").string();
        const auto key = (dir.path / "gateway.key").string();
        spit(certificate, kShippedCertificate);
        spit(key, "the shipped key");

        const auto result = CertificateUtils::EnsureServerCertificate(certificate, key, "host.example", {"localhost"});

        BOOST_CHECK(result.action == Euclid::Core::ServerCertificateResult::Action::Replaced);
        BOOST_CHECK_EQUAL(result.backupFile, certificate + ".no-san");
        BOOST_CHECK_EQUAL(slurp(result.backupFile), kShippedCertificate);
        BOOST_REQUIRE(result.info.has_value());
        BOOST_CHECK(!result.info->subjectAltNames.empty());
        BOOST_CHECK(CertificateUtils::KeyMatches(slurp(certificate), slurp(key)));
    }

    BOOST_AUTO_TEST_CASE(EnsureLeavesAnUnreadableCertificateForTheListenerToRefuse) {

        // Somebody's own file, however broken: replacing it would hide the mistake behind a
        // certificate nobody asked for.
        const TempDir dir;
        const auto certificate = (dir.path / "gateway.crt").string();
        const auto key = (dir.path / "gateway.key").string();
        spit(certificate, "not a certificate");
        spit(key, "not a key");

        const auto result = CertificateUtils::EnsureServerCertificate(certificate, key, "host.example", {});

        BOOST_CHECK(result.action == Euclid::Core::ServerCertificateResult::Action::Kept);
        BOOST_CHECK(!result.info.has_value());
        BOOST_CHECK_EQUAL(slurp(certificate), "not a certificate");
    }

BOOST_AUTO_TEST_SUITE_END()
