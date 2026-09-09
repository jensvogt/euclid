// C++ includes
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

// zlib includes
#include <zlib.h>

// libxml2 includes
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/valid.h>

// xmlsec includes
#include <xmlsec/openssl/app.h>
#include <xmlsec/openssl/crypto.h>
#include <xmlsec/openssl/symbols.h>
#include <xmlsec/transforms.h>
#include <xmlsec/xmldsig.h>
#include <xmlsec/xmlsec.h>
#include <xmlsec/xmltree.h>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SamlProvider.h>
#include <euclid/core/UuidUtils.h>

namespace Euclid::Core {

    namespace {

        constexpr auto kProtocolNs = "urn:oasis:names:tc:SAML:2.0:protocol";
        constexpr auto kAssertionNs = "urn:oasis:names:tc:SAML:2.0:assertion";
        constexpr auto kMetadataNs = "urn:oasis:names:tc:SAML:2.0:metadata";
        constexpr auto kSignatureNs = "http://www.w3.org/2000/09/xmldsig#";
        constexpr auto kStatusSuccess = "urn:oasis:names:tc:SAML:2.0:status:Success";
        constexpr auto kBindingPost = "urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST";

        // ── libxml2 conveniences ─────────────────────────────────────────────

        std::string toString(const xmlChar *value) {
            return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char *>(value));
        }

        bool isElement(const xmlNodePtr node, const char *ns, const char *name) {
            return node != nullptr && node->type == XML_ELEMENT_NODE &&
                   xmlStrcmp(node->name, BAD_CAST name) == 0 &&
                   node->ns != nullptr && xmlStrcmp(node->ns->href, BAD_CAST ns) == 0;
        }

        xmlNodePtr firstChild(const xmlNodePtr parent, const char *ns, const char *name) {
            if (parent == nullptr) return nullptr;
            for (xmlNodePtr child = parent->children; child != nullptr; child = child->next) {
                if (isElement(child, ns, name)) return child;
            }
            return nullptr;
        }

        std::vector<xmlNodePtr> childrenNamed(const xmlNodePtr parent, const char *ns, const char *name) {
            std::vector<xmlNodePtr> found;
            if (parent == nullptr) return found;
            for (xmlNodePtr child = parent->children; child != nullptr; child = child->next) {
                if (isElement(child, ns, name)) found.push_back(child);
            }
            return found;
        }

        // Every element of this name anywhere in the tree. Used for the counting rules - "exactly
        // one Assertion", "no EncryptedAssertion" - which are about the whole document, not about
        // one level of it, because a wrapping attack puts its extra copy wherever it is not looked
        // for.
        void collect(const xmlNodePtr node, const char *ns, const char *name, std::vector<xmlNodePtr> &found) {
            for (xmlNodePtr child = node; child != nullptr; child = child->next) {
                if (isElement(child, ns, name)) found.push_back(child);
                if (child->children != nullptr) collect(child->children, ns, name, found);
            }
        }

        std::vector<xmlNodePtr> findAll(const xmlNodePtr root, const char *ns, const char *name) {
            std::vector<xmlNodePtr> found;
            collect(root, ns, name, found);
            return found;
        }

        std::string attribute(const xmlNodePtr node, const char *name) {
            if (node == nullptr) return {};
            xmlChar *value = xmlGetNoNsProp(node, BAD_CAST name);
            std::string result = toString(value);
            if (value != nullptr) xmlFree(value);
            return result;
        }

        std::string textOf(const xmlNodePtr node) {
            if (node == nullptr) return {};
            xmlChar *content = xmlNodeGetContent(node);
            std::string result = toString(content);
            if (content != nullptr) xmlFree(content);

            // Providers indent their XML, and an indented element's content arrives with the
            // indentation in it.
            const auto first = result.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            return result.substr(first, result.find_last_not_of(" \t\r\n") - first + 1);
        }

        // ── time ─────────────────────────────────────────────────────────────

        // xs:dateTime, always UTC in a SAML assertion. Parsed here rather than through
        // DateTimeUtils because a validity window is exactly the wrong place to go through a local
        // timezone and back.
        std::optional<std::chrono::system_clock::time_point> parseInstant(const std::string &value) {

            if (value.empty()) return std::nullopt;

            std::tm tm{};
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            if (std::sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
                return std::nullopt;
            }
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = minute;
            tm.tm_sec = second;

#ifdef _WIN32
            const auto seconds = _mkgmtime(&tm);
#else
            const auto seconds = timegm(&tm);
#endif
            if (seconds == -1) return std::nullopt;
            return std::chrono::system_clock::from_time_t(seconds);
        }

        std::string formatInstant(const std::chrono::system_clock::time_point &when) {
            const auto seconds = std::chrono::system_clock::to_time_t(when);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &seconds);
#else
            gmtime_r(&seconds, &tm);
#endif
            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buffer;
        }

        // ── xmlsec ───────────────────────────────────────────────────────────

        // Initialised once and never torn down: the library is process-wide state, and a module
        // that has verified one assertion will verify more.
        void initialiseXmlSec() {

            static std::once_flag once;
            static bool ready = false;

            std::call_once(once, [] {
                xmlInitParser();
                if (xmlSecInit() < 0) {
                    log_error << "xmlsec could not be initialised";
                    return;
                }
                if (xmlSecOpenSSLAppInit(nullptr) < 0) {
                    log_error << "The xmlsec OpenSSL backend could not be initialised";
                    return;
                }
                if (xmlSecOpenSSLInit() < 0) {
                    log_error << "The xmlsec OpenSSL crypto engine could not be initialised";
                    return;
                }
                ready = true;
            });

            if (!ready) throw SamlError("XML signature support is not available: xmlsec failed to initialise");
        }

        // Parses a SAML document defensively.
        //
        // No network, no DTD loading and no entity substitution: an assertion arrives from outside,
        // and a parser that resolves what a document tells it to resolve is how XML external entity
        // attacks work. Huge documents are refused for the same reason - nothing legitimate here is
        // large.
        xmlDocPtr parseDocument(const std::string &xml) {

            constexpr std::size_t kMaxDocumentSize = 1 << 20;
            if (xml.size() > kMaxDocumentSize) throw SamlError("SAML response is implausibly large");

            // XML_PARSE_NONET and nothing else. In particular not XML_PARSE_NOBLANKS: the
            // whitespace between a provider's elements is part of what its signature covers, and a
            // parser that tidies it away produces a document whose digest no longer matches. Nor
            // XML_PARSE_NOENT, which is what turns a document's own entity declarations into an
            // external entity attack.
            xmlDocPtr document = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "saml.xml", nullptr, XML_PARSE_NONET);
            if (document == nullptr) throw SamlError("SAML response is not well-formed XML");
            return document;
        }

        // Makes the ID attributes resolvable, so that a signature's "#..." reference finds its
        // element - and refuses the document if an ID is used twice, which is one of the shapes a
        // signature wrapping attack takes.
        void registerIds(const xmlDocPtr document, const xmlNodePtr node) {

            for (xmlNodePtr child = node; child != nullptr; child = child->next) {
                if (child->type == XML_ELEMENT_NODE) {
                    if (xmlAttrPtr id = xmlHasProp(child, BAD_CAST "ID"); id != nullptr) {
                        xmlChar *value = xmlNodeListGetString(document, id->children, 1);
                        if (value != nullptr) {
                            if (xmlGetID(document, value) != nullptr) {
                                xmlFree(value);
                                throw SamlError("SAML response uses the same ID twice");
                            }
                            xmlAddID(nullptr, document, value, id);
                            xmlFree(value);
                        }
                    }
                }
                if (child->children != nullptr) registerIds(document, child->children);
            }
        }

        // Verifies one signature against the configured certificate.
        //
        // The key is set explicitly rather than left to the document's KeyInfo: a signature that
        // verifies against a certificate the attacker attached verifies nothing at all.
        void verifySignature(const xmlNodePtr signature, const std::string &certificatePem) {

            initialiseXmlSec();

            xmlSecKeyPtr key = xmlSecOpenSSLAppKeyLoadMemory(reinterpret_cast<const xmlSecByte *>(certificatePem.data()),
                                                             certificatePem.size(), xmlSecKeyDataFormatCertPem,
                                                             nullptr, nullptr, nullptr);
            if (key == nullptr) throw SamlError("The configured identity provider certificate could not be read");

            xmlSecDSigCtxPtr context = xmlSecDSigCtxCreate(nullptr);
            if (context == nullptr) {
                xmlSecKeyDestroy(key);
                throw SamlError("XML signature context could not be created");
            }
            context->signKey = key;// adopted; destroyed with the context

            // Only what a SAML deployment written this decade uses. Enabling a list rather than
            // accepting the default set is what keeps SHA-1 - and the various canonicalisations
            // whose differences attacks are built out of - from being an option at all.
            const bool transformsEnabled =
                    xmlSecDSigCtxEnableReferenceTransform(context, xmlSecTransformExclC14NId) >= 0 &&
                    xmlSecDSigCtxEnableReferenceTransform(context, xmlSecTransformEnvelopedId) >= 0 &&
                    xmlSecDSigCtxEnableReferenceTransform(context, xmlSecOpenSSLTransformSha256Id) >= 0 &&
                    xmlSecDSigCtxEnableSignatureTransform(context, xmlSecTransformExclC14NId) >= 0 &&
                    xmlSecDSigCtxEnableSignatureTransform(context, xmlSecOpenSSLTransformRsaSha256Id) >= 0;
            if (!transformsEnabled) {
                xmlSecDSigCtxDestroy(context);
                throw SamlError("XML signature algorithms could not be restricted");
            }

            const int verified = xmlSecDSigCtxVerify(context, signature);
            const auto status = context->status;
            xmlSecDSigCtxDestroy(context);

            if (verified < 0) throw SamlError("The assertion's signature could not be checked");
            if (status != xmlSecDSigStatusSucceeded) throw SamlError("The assertion's signature does not match the configured identity provider certificate");
        }

        // ── deflate, for the redirect binding ────────────────────────────────

        // Raw DEFLATE, which is what the HTTP-Redirect binding means by "DEFLATE" - the zlib
        // wrapper a naive compress() would add makes the request unreadable to every provider.
        std::string deflateRaw(const std::string &data) {

            z_stream stream{};
            if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                throw SamlError("The authentication request could not be compressed");
            }

            std::string out(deflateBound(&stream, data.size()), '\0');
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
            stream.avail_in = static_cast<uInt>(data.size());
            stream.next_out = reinterpret_cast<Bytef *>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());

            const int result = deflate(&stream, Z_FINISH);
            const auto written = out.size() - stream.avail_out;
            deflateEnd(&stream);

            if (result != Z_STREAM_END) throw SamlError("The authentication request could not be compressed");
            out.resize(written);
            return out;
        }

        std::string percentEncode(const std::string_view value) {
            static constexpr std::string_view unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
            std::string encoded;
            encoded.reserve(value.size());
            for (const char c: value) {
                if (unreserved.find(c) != std::string_view::npos) {
                    encoded += c;
                } else {
                    static constexpr char hex[] = "0123456789ABCDEF";
                    encoded += '%';
                    encoded += hex[static_cast<unsigned char>(c) >> 4];
                    encoded += hex[static_cast<unsigned char>(c) & 0x0F];
                }
            }
            return encoded;
        }

        // An xsd:ID may not start with a digit, and a bare UUID sometimes does.
        std::string newIdentifier() {
            std::string id = "_" + UuidUtils::CreateRandomUuid();
            std::erase(id, '-');
            return id;
        }

        std::string xmlEscape(const std::string &value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char c: value) {
                switch (c) {
                    case '&': escaped += "&amp;"; break;
                    case '<': escaped += "&lt;"; break;
                    case '>': escaped += "&gt;"; break;
                    case '"': escaped += "&quot;"; break;
                    case '\'': escaped += "&apos;"; break;
                    default: escaped += c;
                }
            }
            return escaped;
        }

        // Holds a parsed document for as long as the verification needs it, whatever way that ends.
        struct DocumentGuard {
            xmlDocPtr document{nullptr};
            ~DocumentGuard() { if (document != nullptr) xmlFreeDoc(document); }
        };

    }// namespace

    // ── Configuration ────────────────────────────────────────────────────────

    SamlConfiguration SamlConfiguration::FromConfiguration(const std::string &module) {

        const auto &cfg = Configuration::instance();
        const std::string prefix = "euclid.modules." + module + ".saml.";

        SamlConfiguration config;
        config.enabled = cfg.getOr<bool>(prefix + "enabled", false);
        config.entityId = cfg.getOr<std::string>(prefix + "entity-id", "");
        config.acsUrl = cfg.getOr<std::string>(prefix + "acs-url", "");
        config.idpEntityId = cfg.getOr<std::string>(prefix + "idp-entity-id", "");
        config.idpSsoUrl = cfg.getOr<std::string>(prefix + "idp-sso-url", "");
        config.idpCertificate = cfg.getOr<std::string>(prefix + "idp-certificate", "");
        config.idpCertificateFile = cfg.getOr<std::string>(prefix + "idp-certificate-file", "");
        config.usernameAttribute = cfg.getOr<std::string>(prefix + "username-attribute", "");
        config.emailAttribute = cfg.getOr<std::string>(prefix + "email-attribute", "email");
        config.accountId = cfg.getOr<std::string>(prefix + "account-id", "");
        config.jitProvisioning = cfg.getOr<bool>(prefix + "jit-provisioning", true);
        config.linkExistingUsers = cfg.getOr<bool>(prefix + "link-existing-users", false);
        config.allowIdpInitiated = cfg.getOr<bool>(prefix + "allow-idp-initiated", false);
        config.clockSkew = std::chrono::seconds(cfg.getOr<long>(prefix + "clock-skew-seconds", 60));
        config.stateTtl = std::chrono::seconds(cfg.getOr<long>(prefix + "state-ttl-seconds", 600));

        if (cfg.has(prefix + "return-to-prefixes")) {
            config.returnToPrefixes = cfg.getArray<std::string>(prefix + "return-to-prefixes");
        }

        if (config.accountId.empty() && cfg.has("euclid.account-ids")) {
            if (const auto accountIds = cfg.getArray<std::string>("euclid.account-ids"); !accountIds.empty()) {
                config.accountId = accountIds.front();
            }
        }
        return config;
    }

    std::vector<std::string> SamlConfiguration::Validate() const {

        std::vector<std::string> problems;
        if (!enabled) return problems;

        if (entityId.empty()) problems.emplace_back("saml.entity-id is not set");
        if (acsUrl.empty()) problems.emplace_back("saml.acs-url is not set");
        if (idpEntityId.empty()) problems.emplace_back("saml.idp-entity-id is not set");

        if (idpCertificate.empty() && idpCertificateFile.empty()) {
            problems.emplace_back("neither saml.idp-certificate nor saml.idp-certificate-file is set; there is nothing to verify assertions against");
        }
        if (!idpCertificate.empty() && !idpCertificateFile.empty()) {
            problems.emplace_back("saml.idp-certificate and saml.idp-certificate-file are both set; only one of them can be the certificate");
        }
        if (!idpCertificateFile.empty() && !std::ifstream(idpCertificateFile).good()) {
            problems.emplace_back("saml.idp-certificate-file cannot be read: " + idpCertificateFile);
        }
        return problems;
    }

    std::vector<std::string> SamlConfiguration::ValidateForAuthentication() const {

        auto problems = Validate();
        if (!enabled) return problems;

        // Only starting a login needs this: it is where the browser is sent. Consuming an assertion
        // that arrived some other way does not.
        if (idpSsoUrl.empty()) problems.emplace_back("saml.idp-sso-url is not set");
        return problems;
    }

    std::optional<SamlDescription> SamlResponseVerifier::Describe(const std::string &responseXml) {

        try {
            DocumentGuard guard{parseDocument(responseXml)};
            const xmlNodePtr response = xmlDocGetRootElement(guard.document);
            if (!isElement(response, kProtocolNs, "Response")) return std::nullopt;

            const auto assertions = findAll(response, kAssertionNs, "Assertion");
            if (assertions.empty()) return std::nullopt;
            const xmlNodePtr assertion = assertions.front();

            SamlDescription description;
            description.destination = attribute(response, "Destination");
            description.issuer = textOf(firstChild(assertion, kAssertionNs, "Issuer"));
            if (description.issuer.empty()) description.issuer = textOf(firstChild(response, kAssertionNs, "Issuer"));

            const xmlNodePtr conditions = firstChild(assertion, kAssertionNs, "Conditions");
            description.notOnOrAfter = attribute(conditions, "NotOnOrAfter");
            for (const xmlNodePtr restriction: childrenNamed(conditions, kAssertionNs, "AudienceRestriction")) {
                if (const xmlNodePtr audience = firstChild(restriction, kAssertionNs, "Audience"); audience != nullptr) {
                    description.audience = textOf(audience);
                }
            }

            const xmlNodePtr subject = firstChild(assertion, kAssertionNs, "Subject");
            description.nameId = textOf(firstChild(subject, kAssertionNs, "NameID"));
            for (const xmlNodePtr confirmation: childrenNamed(subject, kAssertionNs, "SubjectConfirmation")) {
                if (const xmlNodePtr data = firstChild(confirmation, kAssertionNs, "SubjectConfirmationData"); data != nullptr) {
                    description.recipient = attribute(data, "Recipient");
                }
            }

            description.hasSignature = !findAll(response, kSignatureNs, "Signature").empty();

            for (const xmlNodePtr statement: childrenNamed(assertion, kAssertionNs, "AttributeStatement")) {
                for (const xmlNodePtr attributeNode: childrenNamed(statement, kAssertionNs, "Attribute")) {
                    const auto name = attribute(attributeNode, "Name");
                    const auto friendlyName = attribute(attributeNode, "FriendlyName");
                    const auto values = childrenNamed(attributeNode, kAssertionNs, "AttributeValue");
                    description.attributes.push_back((friendlyName.empty() ? name : friendlyName + " (" + name + ")") +
                                                     " = " + (values.empty() ? "" : textOf(values.front())));
                }
            }
            return description;

        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    std::string SamlConfiguration::IdpCertificatePem() const {

        if (!idpCertificate.empty()) return idpCertificate;

        std::ifstream file(idpCertificateFile, std::ios::binary);
        if (!file) throw SamlError("The identity provider certificate could not be read: " + idpCertificateFile);

        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    bool SamlConfiguration::IsReturnToAllowed(const std::string &returnTo) const {
        return Core::IsReturnToAllowed(returnTo, returnToPrefixes);
    }

    // ── Verification ─────────────────────────────────────────────────────────

    std::optional<FederatedIdentity> SamlResponseVerifier::Verify(const std::string &responseXml, const SamlConfiguration &config,
                                                                  const std::string &expectedInResponseTo, std::string &error) {

        try {
            DocumentGuard guard{parseDocument(responseXml)};
            const xmlNodePtr response = xmlDocGetRootElement(guard.document);

            if (!isElement(response, kProtocolNs, "Response")) {
                error = "The document is not a SAML Response";
                return std::nullopt;
            }
            registerIds(guard.document, response);

            // Encrypted assertions are a supported thing to configure a provider for, and not a
            // thing euclid can read. Said plainly rather than being reported as "no assertion",
            // which is what an operator would otherwise have to work out for themselves.
            if (!findAll(response, kAssertionNs, "EncryptedAssertion").empty()) {
                error = "The assertion is encrypted, which this installation cannot read - turn assertion encryption off for this application";
                return std::nullopt;
            }

            const auto assertions = findAll(response, kAssertionNs, "Assertion");
            if (assertions.size() != 1) {
                error = assertions.empty() ? "The response carries no assertion"
                                           : "The response carries more than one assertion";
                return std::nullopt;
            }
            const xmlNodePtr assertion = assertions.front();

            // Status first: a provider that refused says so here, and its reason is worth more to
            // whoever is reading the log than "no subject".
            if (const xmlNodePtr status = firstChild(response, kProtocolNs, "Status"); status != nullptr) {
                const xmlNodePtr code = firstChild(status, kProtocolNs, "StatusCode");
                if (const auto value = attribute(code, "Value"); value != kStatusSuccess) {
                    const auto message = textOf(firstChild(status, kProtocolNs, "StatusMessage"));
                    error = "The identity provider refused the login: " + (value.empty() ? "no status" : value) +
                            (message.empty() ? "" : " (" + message + ")");
                    return std::nullopt;
                }
            }

            // Which element is signed decides which element may be trusted. Both shapes are
            // legitimate - providers differ, and OneLogin signs the assertion - but each has to
            // carry its own signature as a direct child, and that signature has to name its parent
            // by ID. A signature found anywhere else is a signature over something else.
            xmlNodePtr signature = nullptr;
            xmlNodePtr signedElement = nullptr;
            if (const auto assertionSignatures = childrenNamed(assertion, kSignatureNs, "Signature"); !assertionSignatures.empty()) {
                if (assertionSignatures.size() > 1) {
                    error = "The assertion carries more than one signature";
                    return std::nullopt;
                }
                signature = assertionSignatures.front();
                signedElement = assertion;
            } else if (const auto responseSignatures = childrenNamed(response, kSignatureNs, "Signature"); !responseSignatures.empty()) {
                if (responseSignatures.size() > 1) {
                    error = "The response carries more than one signature";
                    return std::nullopt;
                }
                signature = responseSignatures.front();
                signedElement = response;

                // A signed Response only vouches for an assertion that is actually inside it.
                if (assertion->parent != response) {
                    error = "The response is signed but the assertion is not inside what was signed";
                    return std::nullopt;
                }
            } else {
                error = "Neither the response nor the assertion is signed";
                return std::nullopt;
            }

            // The reference has to name the signed element by its own ID. This is the check that
            // makes wrapping fail: a signature lifted from another document references that
            // document's element, not this one.
            const xmlNodePtr signedInfo = firstChild(signature, kSignatureNs, "SignedInfo");
            const auto references = childrenNamed(signedInfo, kSignatureNs, "Reference");
            if (references.size() != 1) {
                error = "The signature does not cover exactly one element";
                return std::nullopt;
            }
            const auto signedId = attribute(signedElement, "ID");
            if (signedId.empty() || attribute(references.front(), "URI") != "#" + signedId) {
                error = "The signature does not reference the element it is attached to";
                return std::nullopt;
            }

            verifySignature(signature, config.IdpCertificatePem());

            // Everything from here reads the element that was verified, or its children.
            const auto issuer = textOf(firstChild(signedElement, kAssertionNs, "Issuer"));
            const auto assertionIssuer = textOf(firstChild(assertion, kAssertionNs, "Issuer"));
            if (const auto stated = issuer.empty() ? assertionIssuer : issuer; stated != config.idpEntityId) {
                error = "The assertion was issued by '" + stated + "', not by the configured identity provider";
                return std::nullopt;
            }

            if (const auto destination = attribute(response, "Destination"); !destination.empty() && destination != config.acsUrl) {
                error = "The response was addressed to '" + destination + "', not to this installation's assertion consumer service";
                return std::nullopt;
            }

            const auto now = std::chrono::system_clock::now();
            const auto skew = config.clockSkew;

            const xmlNodePtr conditions = firstChild(assertion, kAssertionNs, "Conditions");
            if (const auto notBefore = parseInstant(attribute(conditions, "NotBefore")); notBefore.has_value() && now + skew < *notBefore) {
                error = "The assertion is not valid yet";
                return std::nullopt;
            }
            const auto notOnOrAfter = parseInstant(attribute(conditions, "NotOnOrAfter"));
            if (notOnOrAfter.has_value() && now - skew >= *notOnOrAfter) {
                error = "The assertion has expired";
                return std::nullopt;
            }

            // The audience is what stops an assertion minted for another service - by the same
            // provider, with the same signature - from being replayed here.
            bool audienceMatches = false;
            bool audienceStated = false;
            for (const xmlNodePtr restriction: childrenNamed(conditions, kAssertionNs, "AudienceRestriction")) {
                for (const xmlNodePtr audience: childrenNamed(restriction, kAssertionNs, "Audience")) {
                    audienceStated = true;
                    if (textOf(audience) == config.entityId) audienceMatches = true;
                }
            }
            if (!audienceStated || !audienceMatches) {
                error = audienceStated ? "The assertion is addressed to another service provider"
                                       : "The assertion names no audience";
                return std::nullopt;
            }

            const xmlNodePtr subject = firstChild(assertion, kAssertionNs, "Subject");
            const auto nameId = textOf(firstChild(subject, kAssertionNs, "NameID"));
            if (nameId.empty()) {
                error = "The assertion names no subject";
                return std::nullopt;
            }

            // Whether this answers a login euclid started. An unsolicited assertion is a valid
            // thing for a provider to send, and a thing this installation has to have opted into.
            bool inResponseToChecked = false;
            for (const xmlNodePtr confirmation: childrenNamed(subject, kAssertionNs, "SubjectConfirmation")) {
                const xmlNodePtr data = firstChild(confirmation, kAssertionNs, "SubjectConfirmationData");
                if (data == nullptr) continue;

                if (const auto recipient = attribute(data, "Recipient"); !recipient.empty() && recipient != config.acsUrl) {
                    error = "The assertion was meant for '" + recipient + "', not for this installation";
                    return std::nullopt;
                }
                if (const auto expires = parseInstant(attribute(data, "NotOnOrAfter")); expires.has_value() && now - skew >= *expires) {
                    error = "The assertion's subject confirmation has expired";
                    return std::nullopt;
                }

                const auto inResponseTo = attribute(data, "InResponseTo");
                if (expectedInResponseTo.empty()) {
                    if (!inResponseTo.empty()) {
                        error = "The assertion answers a login request this installation did not make";
                        return std::nullopt;
                    }
                    if (!config.allowIdpInitiated) {
                        error = "Unsolicited logins are not enabled (saml.allow-idp-initiated)";
                        return std::nullopt;
                    }
                } else if (inResponseTo != expectedInResponseTo) {
                    error = "The assertion answers a different login request";
                    return std::nullopt;
                }
                inResponseToChecked = true;
            }
            if (!inResponseToChecked) {
                error = "The assertion carries no subject confirmation";
                return std::nullopt;
            }

            FederatedIdentity identity;
            identity.provider = "saml";
            identity.subject = nameId;
            identity.issuer = config.idpEntityId;
            identity.assertionId = attribute(assertion, "ID");
            identity.assertionExpiresAt = notOnOrAfter.value_or(now + std::chrono::minutes(10));

            // Attributes, by Name or by FriendlyName - providers disagree about which they send,
            // and an operator configuring this should not have to know which kind theirs is.
            std::string username;
            for (const xmlNodePtr statement: childrenNamed(assertion, kAssertionNs, "AttributeStatement")) {
                for (const xmlNodePtr attributeNode: childrenNamed(statement, kAssertionNs, "Attribute")) {
                    const auto name = attribute(attributeNode, "Name");
                    const auto friendlyName = attribute(attributeNode, "FriendlyName");
                    const auto values = childrenNamed(attributeNode, kAssertionNs, "AttributeValue");
                    if (values.empty()) continue;

                    const auto value = textOf(values.front());
                    if (!config.usernameAttribute.empty() && (name == config.usernameAttribute || friendlyName == config.usernameAttribute)) {
                        username = value;
                    }
                    if (!config.emailAttribute.empty() && (name == config.emailAttribute || friendlyName == config.emailAttribute)) {
                        identity.email = value;
                    }
                }
            }

            if (username.empty()) username = nameId;
            if (identity.email.empty() && nameId.find('@') != std::string::npos) identity.email = nameId;

            // A NameID is often an email address, and an email address makes an unwieldy user ID;
            // its local part is what an operator would recognise, the same choice the OIDC side
            // makes.
            if (const auto at = username.find('@'); at != std::string::npos && username == identity.email) {
                username = username.substr(0, at);
            }
            identity.userId = SanitizeUserId(username);

            return identity;

        } catch (const SamlError &e) {
            error = e.what();
            return std::nullopt;
        } catch (const std::exception &e) {
            error = std::string("The assertion could not be read: ") + e.what();
            return std::nullopt;
        }
    }

    // ── Provider ─────────────────────────────────────────────────────────────

    SamlProvider::SamlProvider(SamlConfiguration config, std::string stateSecret)
        : _config(std::move(config)), _stateSecret(std::move(stateSecret)) {}

    SamlProvider::Authentication SamlProvider::BeginAuthentication(const std::string &returnTo) const {

        const auto requestId = newIdentifier();

        const std::string request =
                R"(<samlp:AuthnRequest xmlns:samlp="urn:oasis:names:tc:SAML:2.0:protocol" xmlns:saml="urn:oasis:names:tc:SAML:2.0:assertion")"
                " ID=\"" + requestId + "\" Version=\"2.0\""
                " IssueInstant=\"" + formatInstant(std::chrono::system_clock::now()) + "\""
                " Destination=\"" + xmlEscape(_config.idpSsoUrl) + "\""
                " ProtocolBinding=\"" + kBindingPost + "\""
                " AssertionConsumerServiceURL=\"" + xmlEscape(_config.acsUrl) + "\">"
                "<saml:Issuer>" + xmlEscape(_config.entityId) + "</saml:Issuer>"
                R"(<samlp:NameIDPolicy AllowCreate="true"/>)"
                "</samlp:AuthnRequest>";

        const auto relayState = SealFederationState({{"requestId", requestId}, {"returnTo", returnTo}}, _stateSecret, _config.stateTtl);

        // The redirect binding: deflate, base64, then percent-encode - in that order, and the
        // deflate is raw.
        std::string url = _config.idpSsoUrl;
        url += url.find('?') == std::string::npos ? "?" : "&";
        url += "SAMLRequest=" + percentEncode(CryptoUtils::Base64Encode(deflateRaw(request)));
        url += "&RelayState=" + percentEncode(relayState);

        return {.url = url, .relayState = relayState, .requestId = requestId};
    }

    FederatedIdentity SamlProvider::Consume(const std::string &samlResponse, const std::string &relayState, std::string &returnTo) const {

        if (samlResponse.empty()) throw SamlError("No SAMLResponse in the callback");

        // An unsolicited assertion has no RelayState of ours, and nothing to compare against.
        std::string expectedRequestId;
        if (!relayState.empty()) {
            const auto state = OpenFederationState(relayState, _stateSecret);
            if (!state.has_value()) {
                throw SamlError("The RelayState is not one this installation issued, or it has expired");
            }
            const boost::json::value asValue = *state;
            expectedRequestId = GetStringValue(asValue, "requestId");
            returnTo = GetStringValue(asValue, "returnTo");
        }

        std::string decoded;
        try {
            decoded = CryptoUtils::Base64Decode(samlResponse);
        } catch (const std::exception &) {
            throw SamlError("The SAMLResponse is not valid base64");
        }

        std::string error;
        auto identity = SamlResponseVerifier::Verify(decoded, _config, expectedRequestId, error);
        if (!identity.has_value()) throw SamlError(error);

        return *identity;
    }

    std::string SamlProvider::Metadata() const {

        return R"(<?xml version="1.0" encoding="UTF-8"?>)"
               R"(<md:EntityDescriptor xmlns:md=")" + std::string(kMetadataNs) + "\" entityID=\"" + xmlEscape(_config.entityId) + "\">"
               R"(<md:SPSSODescriptor AuthnRequestsSigned="false" WantAssertionsSigned="true" protocolSupportEnumeration="urn:oasis:names:tc:SAML:2.0:protocol">)"
               R"(<md:NameIDFormat>urn:oasis:names:tc:SAML:2.0:nameid-format:emailAddress</md:NameIDFormat>)"
               R"(<md:AssertionConsumerService Binding=")" + std::string(kBindingPost) + "\" Location=\"" + xmlEscape(_config.acsUrl) + "\" index=\"0\" isDefault=\"true\"/>"
               "</md:SPSSODescriptor></md:EntityDescriptor>";
    }

}// namespace Euclid::Core
