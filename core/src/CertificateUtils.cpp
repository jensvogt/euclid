//
// Created by vogje01 on 9/7/26.
//

// C++ includes
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>

// Platform includes, for the address parsing the subject alternative names need
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

// OpenSSL includes
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

// Euclid includes
#include <euclid/core/CertificateUtils.h>

namespace Euclid::Core {

    namespace {

        // OpenSSL hands out raw pointers with their own free function each. Wrapping them here
        // means the error paths below can simply return or throw, rather than each having to
        // remember which of the handles opened so far are already live.
        template<typename T, void (*Free)(T *)>
        using Handle = std::unique_ptr<T, decltype([](T *p) { if (p) Free(p); })>;

        using BioPtr = Handle<BIO, BIO_free_all>;
        using X509Ptr = Handle<X509, X509_free>;
        using EvpPkeyPtr = Handle<EVP_PKEY, EVP_PKEY_free>;
        using BignumPtr = Handle<BIGNUM, BN_free>;

        // The last thing OpenSSL complained about, so a thrown error says what actually went wrong
        // rather than only which of our steps was running.
        std::string opensslError() {
            const unsigned long code = ERR_get_error();
            if (code == 0) return "no OpenSSL error recorded";
            char buffer[256];
            ERR_error_string_n(code, buffer, sizeof(buffer));
            return buffer;
        }

        std::string bioToString(const BIO *bio) {
            char *data = nullptr;
            const auto length = BIO_get_mem_data(const_cast<BIO *>(bio), &data);
            return length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string{};
        }

        // A name is added to the certificate as an IP address if it is one, and as a DNS name
        // otherwise. Getting this wrong is not cosmetic: a client verifying "127.0.0.1" against a
        // certificate that lists it as a DNS name rejects the connection.
        std::string subjectAltNameEntry(const std::string &name) {
            unsigned char address[sizeof(struct in6_addr)];
            if (inet_pton(AF_INET, name.c_str(), address) == 1 || inet_pton(AF_INET6, name.c_str(), address) == 1) {
                return "IP:" + name;
            }
            return "DNS:" + name;
        }

        void addExtension(X509 *certificate, const int nid, const std::string &value) {
            X509V3_CTX context;
            X509V3_set_ctx_nodb(&context);
            X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);

            X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
            if (!extension) throw std::runtime_error("Could not build certificate extension: " + value + ", error: " + opensslError());

            X509_add_ext(certificate, extension, -1);
            X509_EXTENSION_free(extension);
        }

        std::string toHex(const unsigned char *data, const unsigned int length) {
            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (unsigned int i = 0; i < length; ++i) oss << std::setw(2) << static_cast<int>(data[i]);
            return oss.str();
        }

        system_clock::time_point fromAsn1Time(const ASN1_TIME *time) {
            if (!time) return {};

            std::tm tm{};
            if (ASN1_TIME_to_tm(time, &tm) != 1) return {};

#ifdef _WIN32
            const std::time_t seconds = _mkgmtime(&tm);
#else
            const std::time_t seconds = timegm(&tm);
#endif
            return seconds == static_cast<std::time_t>(-1) ? system_clock::time_point{} : system_clock::from_time_t(seconds);
        }

        std::string nameToString(X509_NAME *name) {
            if (!name) return {};
            const BioPtr bio(BIO_new(BIO_s_mem()));
            if (!bio) return {};

            // RFC 2253 rather than X509_NAME_oneline(): one line either way, but this is the form
            // every other tool prints a distinguished name in, so what euclid reports and what
            // "openssl x509 -subject" reports can be compared without translating one to the other.
            X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253);
            return bioToString(bio.get());
        }

        std::vector<std::string> subjectAltNamesOf(X509 *certificate) {

            std::vector<std::string> names;
            auto *alternatives = static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
            if (!alternatives) return names;

            for (int i = 0; i < sk_GENERAL_NAME_num(alternatives); ++i) {
                const GENERAL_NAME *entry = sk_GENERAL_NAME_value(alternatives, i);
                if (entry->type == GEN_DNS) {
                    const auto *value = ASN1_STRING_get0_data(entry->d.dNSName);
                    names.emplace_back("DNS:" + std::string(reinterpret_cast<const char *>(value)));
                } else if (entry->type == GEN_IPADD) {
                    const auto *octets = ASN1_STRING_get0_data(entry->d.iPAddress);
                    const auto length = ASN1_STRING_length(entry->d.iPAddress);
                    char text[INET6_ADDRSTRLEN]{};
                    if (length == 4 && inet_ntop(AF_INET, octets, text, sizeof(text))) {
                        names.emplace_back("IP:" + std::string(text));
                    } else if (length == 16 && inet_ntop(AF_INET6, octets, text, sizeof(text))) {
                        names.emplace_back("IP:" + std::string(text));
                    }
                }
            }
            GENERAL_NAMES_free(alternatives);
            return names;
        }

        X509Ptr readCertificate(const std::string &certificatePem) {
            if (certificatePem.empty()) return nullptr;

            const BioPtr bio(BIO_new_mem_buf(certificatePem.data(), static_cast<int>(certificatePem.size())));
            if (!bio) return nullptr;

            // The first certificate in the PEM, which for a chain is the leaf - the one the
            // connection is actually made with, and the one whose names and dates matter.
            return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        }

    }// namespace

    CertificatePair CertificateUtils::GenerateSelfSigned(const std::string &commonName,
                                                         const std::vector<std::string> &subjectAltNames,
                                                         const long validDays, const int keyBits) {

        const EvpPkeyPtr key(EVP_RSA_gen(static_cast<unsigned int>(keyBits)));
        if (!key) throw std::runtime_error("Could not generate a private key, error: " + opensslError());

        const X509Ptr certificate(X509_new());
        if (!certificate) throw std::runtime_error("Could not create a certificate, error: " + opensslError());

        // Version 3, given as 2 - X.509 numbers its versions from one and encodes them from zero.
        // Anything less has no extensions, and a certificate with no subject alternative names is
        // one no current client will accept.
        X509_set_version(certificate.get(), 2);

        // Random rather than sequential, so two installations that generate a certificate for the
        // same name do not also agree on its serial number.
        const BignumPtr serial(BN_new());
        if (!serial || BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1
            || !BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get()))) {
            throw std::runtime_error("Could not set the certificate serial number, error: " + opensslError());
        }

        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), static_cast<long>(validDays) * 24 * 60 * 60);

        X509_NAME *subject = X509_get_subject_name(certificate.get());
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0);

        // Its own issuer. That is what "self-signed" means, and what lets the certificate be
        // handed to a client as the thing to trust.
        X509_set_issuer_name(certificate.get(), subject);
        X509_set_pubkey(certificate.get(), key.get());

        // The common name is included as an alternative name too: a client that checks host names
        // has looked only at these since long before this was written, and ignores the CN.
        std::string alternatives = subjectAltNameEntry(commonName);
        for (const auto &name: subjectAltNames) {
            if (name.empty() || name == commonName) continue;
            alternatives += "," + subjectAltNameEntry(name);
        }

        addExtension(certificate.get(), NID_subject_alt_name, alternatives);
        addExtension(certificate.get(), NID_basic_constraints, "critical,CA:TRUE");
        addExtension(certificate.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
        addExtension(certificate.get(), NID_ext_key_usage, "serverAuth");

        if (X509_sign(certificate.get(), key.get(), EVP_sha256()) == 0) {
            throw std::runtime_error("Could not sign the certificate, error: " + opensslError());
        }

        const BioPtr certificateBio(BIO_new(BIO_s_mem()));
        const BioPtr keyBio(BIO_new(BIO_s_mem()));
        if (!certificateBio || !keyBio) throw std::runtime_error("Could not allocate a PEM buffer");

        if (PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1) {
            throw std::runtime_error("Could not write the certificate, error: " + opensslError());
        }

        // Unencrypted PKCS#8: what asio hands to OpenSSL when a listener starts, and what every
        // other tool reads. A passphrase would have to be stored next to it to be usable, which
        // protects nothing and breaks everything that reads the key.
        if (PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            throw std::runtime_error("Could not write the private key, error: " + opensslError());
        }

        return {.certificate = bioToString(certificateBio.get()), .privateKey = bioToString(keyBio.get())};
    }

    std::optional<CertificateInfo> CertificateUtils::Inspect(const std::string &certificatePem) {

        const auto certificate = readCertificate(certificatePem);
        if (!certificate) return std::nullopt;

        CertificateInfo info;
        info.subject = nameToString(X509_get_subject_name(certificate.get()));
        info.issuer = nameToString(X509_get_issuer_name(certificate.get()));
        info.selfSigned = !info.subject.empty() && info.subject == info.issuer;
        info.notBefore = fromAsn1Time(X509_get0_notBefore(certificate.get()));
        info.notAfter = fromAsn1Time(X509_get0_notAfter(certificate.get()));
        info.subjectAltNames = subjectAltNamesOf(certificate.get());

        if (const BignumPtr serial(ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate.get()), nullptr)); serial) {
            if (char *hex = BN_bn2hex(serial.get())) {
                info.serialNumber = hex;
                OPENSSL_free(hex);
            }
        }

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;
        if (X509_digest(certificate.get(), EVP_sha256(), digest, &digestLength) == 1) {
            info.fingerprint = toHex(digest, digestLength);
        }
        return info;
    }

    bool CertificateUtils::KeyMatches(const std::string &certificatePem, const std::string &privateKeyPem) {

        const auto certificate = readCertificate(certificatePem);
        if (!certificate || privateKeyPem.empty()) return false;

        const BioPtr keyBio(BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size())));
        if (!keyBio) return false;

        const EvpPkeyPtr key(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
        if (!key) return false;

        return X509_check_private_key(certificate.get(), key.get()) == 1;
    }

}// namespace Euclid::Core
