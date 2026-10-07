// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace Euclid::Core {

    using std::chrono::system_clock;

    /**
     * @brief An X.509 certificate and the private key that goes with it, both PEM-encoded.
     *
     * @par
     * The two are kept together because they are only ever useful together: a certificate without
     * its key cannot terminate a connection, and a key without its certificate cannot say who it
     * belongs to. Whatever holds one has to hold the other.
     */
    struct CertificatePair {

        /**
         * @brief PEM-encoded certificate. May hold a chain, leaf first, as a real certificate
         * authority hands one out.
         */
        std::string certificate;

        /**
         * @brief PEM-encoded private key, unencrypted PKCS#8.
         */
        std::string privateKey;
    };

    /**
     * @brief What a certificate says about itself, read out of the PEM rather than taken on trust
     * from whoever supplied it.
     *
     * @par
     * Kept alongside the certificate so that "which certificate is this port using, and when does
     * it expire" can be answered without parsing X.509 again - and so an expiry can be noticed
     * before a caller notices it for us.
     */
    struct CertificateInfo {

        /**
         * @brief Distinguished name of the subject, e.g. "/CN=gateway.example.com".
         */
        std::string subject;

        /**
         * @brief Distinguished name of the issuer. Equal to the subject for a self-signed
         * certificate.
         */
        std::string issuer;

        /**
         * @brief Serial number, hexadecimal.
         */
        std::string serialNumber;

        /**
         * @brief SHA-256 fingerprint, lower-case hexadecimal without separators. What somebody
         * compares against when asked to trust a self-signed certificate.
         */
        std::string fingerprint;

        /**
         * @brief The subject alternative names the certificate carries, in the form they appear
         * in it, e.g. "DNS:localhost" or "IP:127.0.0.1". Empty when it has none - which for a
         * modern client means it matches no host name at all.
         */
        std::vector<std::string> subjectAltNames;

        /**
         * @brief Start of the validity period.
         */
        system_clock::time_point notBefore{};

        /**
         * @brief End of the validity period.
         */
        system_clock::time_point notAfter{};

        /**
         * @brief Whether subject and issuer are the same, which is what makes a certificate its
         * own trust anchor.
         */
        bool selfSigned{false};
    };

    /**
     * @brief What CertificateUtils::EnsureServerCertificate() found, and what it did about it.
     */
    struct ServerCertificateResult {

        enum class Action {
            /** The files were there and usable, and were left alone. */
            Kept,
            /** There was no certificate, and one was generated. */
            Generated,
            /** There was a certificate naming no host at all; it was moved aside and replaced. */
            Replaced
        };

        Action action{Action::Kept};

        /**
         * @brief The certificate now in place, or nothing if what was kept does not parse - which
         * is left for the listener to fail on, since it is somebody's own file.
         */
        std::optional<CertificateInfo> info;

        /**
         * @brief Where the replaced certificate was moved to, for Action::Replaced.
         */
        std::string backupFile;
    };

    /**
     * @brief Generates, reads and checks the X.509 certificates euclid terminates TLS with.
     *
     * @par
     * A listener that speaks HTTPS needs a certificate before it can answer anything at all, and
     * an installation that has not been given one yet still has to come up. So both are provided
     * for here: a real certificate is imported, and one that was never supplied is generated -
     * self-signed, valid, and honest about the fact that nobody has vouched for it.
     *
     * @author jensvogt47\@gmail.com
     */
    class CertificateUtils {

    public:

        /**
         * @brief Generates a self-signed certificate and its private key.
         *
         * @par
         * Marked as its own certificate authority, because a self-signed certificate is only
         * usable if it can also be the trust anchor a client verifies against - which is exactly
         * how euclid's own modules already treat the gateway's certificate.
         *
         * @param commonName subject common name, e.g. a host name. Also added as a subject
         * alternative name, since a client that checks host names ignores the common name.
         * @param subjectAltNames further names the certificate should be valid for. An entry that
         * parses as an IP address is added as one, anything else as a DNS name.
         * @param validDays how long the certificate is valid for, from now.
         * @param keyBits RSA key length.
         * @return the certificate and its key, PEM-encoded.
         * @throws std::runtime_error if key generation or signing fails.
         */
        [[nodiscard]]
        static CertificatePair GenerateSelfSigned(const std::string &commonName,
                                                  const std::vector<std::string> &subjectAltNames = {},
                                                  long validDays = 825, int keyBits = 2048);

        /**
         * @brief Reads what a PEM-encoded certificate says about itself.
         *
         * @param certificatePem PEM-encoded certificate; a chain is read as its first (leaf)
         * certificate, which is the one the connection is made with.
         * @return the certificate's properties, or nothing if the PEM does not parse - which is
         * how a caller tells a certificate from whatever else was pasted into the field.
         */
        [[nodiscard]]
        static std::optional<CertificateInfo> Inspect(const std::string &certificatePem);

        /**
         * @brief Whether a private key belongs to a certificate.
         *
         * @par
         * Checked when a certificate is imported rather than when a listener first tries to use
         * it: a mismatched pair is stored happily, and only shows up as a handshake that fails
         * for every caller at the next restart.
         *
         * @param certificatePem PEM-encoded certificate.
         * @param privateKeyPem PEM-encoded, unencrypted private key.
         * @return true if the key is the certificate's, false if it is not, or if either does not
         * parse.
         */
        [[nodiscard]]
        static bool KeyMatches(const std::string &certificatePem, const std::string &privateKeyPem);

        /**
         * @brief Makes sure a server certificate and its key exist, generating them if not.
         *
         * @par
         * What lets a package ship no certificate. Every installation used to carry the same one,
         * private key included - so anyone holding a copy of a release could stand in for any
         * gateway still using it - and it named no host, which every client that checks host
         * names refuses. This gives each host its own on first start instead.
         *
         * @par
         * A certificate is generated when the certificate or the key file is missing. One that
         * exists but carries no subject alternative names at all is replaced too, after being
         * moved aside to "<file>.no-san": no current client accepts it for any host, so it cannot
         * be what anybody relies on - and it is what an installation upgraded from a release
         * that shipped one still has. Anything else is left alone: a certificate an operator put
         * there is theirs, whatever it says.
         *
         * @param certificateFile where the PEM certificate is, or is to be written.
         * @param keyFile where the PEM private key is, or is to be written; owner-readable only.
         * @param commonName subject common name for a generated certificate.
         * @param subjectAltNames the names a generated certificate is valid for.
         * @return what was found and done.
         * @throws std::runtime_error if a certificate is needed and cannot be generated or written.
         */
        static ServerCertificateResult EnsureServerCertificate(const std::string &certificateFile, const std::string &keyFile,
                                                               const std::string &commonName,
                                                               const std::vector<std::string> &subjectAltNames);
    };

}// namespace Euclid::Core
