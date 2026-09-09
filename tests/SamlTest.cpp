#define BOOST_TEST_MODULE SamlTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <ctime>
#include <string>

// libxml2 includes
#include <libxml/parser.h>
#include <libxml/tree.h>

// xmlsec includes
#include <xmlsec/openssl/app.h>
#include <xmlsec/openssl/crypto.h>
#include <xmlsec/openssl/symbols.h>
#include <xmlsec/templates.h>
#include <xmlsec/transforms.h>
#include <xmlsec/xmldsig.h>
#include <xmlsec/xmlsec.h>

// Euclid includes
#include "FederationTestKeys.h"
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/SamlProvider.h>

using Euclid::Core::CryptoUtils;
using Euclid::Core::SamlConfiguration;
using Euclid::Core::SamlResponseVerifier;

// What a service provider has to refuse, and why. Every case here is a real assertion, signed with
// a real key at run time - a fixture with a baked-in signature would expire, and one with a faked
// signature would not exercise the thing being tested.
//
// The negative cases are the point. A SAML implementation that accepts good assertions is easy;
// the ones that have been broken over the years were broken by what they also accepted.

namespace {

    using Euclid::Test::kProviderCertificate;
    using Euclid::Test::kProviderKey;
    using Euclid::Test::kStrangerKey;

    constexpr auto kIdpEntityId = "https://example.onelogin.com/saml/metadata/12345";
    constexpr auto kSpEntityId = "https://euclid.example.com:5566/eam/saml/metadata";
    constexpr auto kAcsUrl = "https://euclid.example.com:5566/eam/saml/acs";
    constexpr auto kRequestId = "_a1b2c3d4e5f6";

    std::string instant(const std::chrono::seconds offset) {

        const auto when = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + offset);

        // gmtime_r is POSIX and gmtime_s is what Windows has, with its arguments the other way
        // round. Neither is going anywhere, so the two lines are simply written out.
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &when);
#else
        gmtime_r(&when, &tm);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buffer;
    }

    struct AssertionOptions {
        std::string issuer{kIdpEntityId};
        std::string audience{kSpEntityId};
        std::string recipient{kAcsUrl};
        std::string destination{kAcsUrl};
        std::string inResponseTo{kRequestId};
        std::string nameId{"jane.federated@example.com"};
        std::string assertionId{"_assertion-0001"};
        std::string statusCode{"urn:oasis:names:tc:SAML:2.0:status:Success"};
        std::chrono::seconds notBefore{std::chrono::seconds(-60)};
        std::chrono::seconds notOnOrAfter{std::chrono::minutes(5)};
        std::string signingKey{kProviderKey};
        bool signAssertion{true};
        bool signResponse{false};
        bool withSubjectConfirmation{true};
        std::string extraAttributes{R"(<saml:AttributeStatement><saml:Attribute Name="email"><saml:AttributeValue>jane.federated@example.com</saml:AttributeValue></saml:Attribute></saml:AttributeStatement>)"};
    };

    std::string responseTemplate(const AssertionOptions &options) {

        const std::string subjectConfirmation = options.withSubjectConfirmation
                ? R"(<saml:SubjectConfirmation Method="urn:oasis:names:tc:SAML:2.0:cm:bearer"><saml:SubjectConfirmationData )" +
                          (options.recipient.empty() ? "" : "Recipient=\"" + options.recipient + "\" ") +
                          (options.inResponseTo.empty() ? "" : "InResponseTo=\"" + options.inResponseTo + "\" ") +
                          "NotOnOrAfter=\"" + instant(options.notOnOrAfter) + "\"/></saml:SubjectConfirmation>"
                : "";

        return R"(<?xml version="1.0" encoding="UTF-8"?>)"
               R"(<samlp:Response xmlns:samlp="urn:oasis:names:tc:SAML:2.0:protocol" xmlns:saml="urn:oasis:names:tc:SAML:2.0:assertion" )"
               "ID=\"_response-0001\" Version=\"2.0\" IssueInstant=\"" + instant(std::chrono::seconds(0)) + "\" " +
               (options.destination.empty() ? "" : "Destination=\"" + options.destination + "\" ") +
               (options.inResponseTo.empty() ? "" : "InResponseTo=\"" + options.inResponseTo + "\"") + ">"
               "<saml:Issuer>" + options.issuer + "</saml:Issuer>"
               "<samlp:Status><samlp:StatusCode Value=\"" + options.statusCode + "\"/></samlp:Status>"
               "<saml:Assertion ID=\"" + options.assertionId + "\" Version=\"2.0\" IssueInstant=\"" + instant(std::chrono::seconds(0)) + "\">"
               "<saml:Issuer>" + options.issuer + "</saml:Issuer>"
               "<saml:Subject><saml:NameID Format=\"urn:oasis:names:tc:SAML:2.0:nameid-format:emailAddress\">" + options.nameId + "</saml:NameID>" +
               subjectConfirmation + "</saml:Subject>"
               "<saml:Conditions NotBefore=\"" + instant(options.notBefore) + "\" NotOnOrAfter=\"" + instant(options.notOnOrAfter) + "\">"
               "<saml:AudienceRestriction><saml:Audience>" + options.audience + "</saml:Audience></saml:AudienceRestriction>"
               "</saml:Conditions>" + options.extraAttributes +
               "</saml:Assertion></samlp:Response>";
    }

    // Signs one element of a document, the way an identity provider does: an enveloped signature
    // over the element, referenced by its own ID.
    void signElement(const xmlDocPtr document, const xmlNodePtr element, const std::string &id, const std::string &keyPem) {

        // The ID has to be resolvable before signing, or the reference has nothing to point at.
        if (xmlAttrPtr idAttribute = xmlHasProp(element, BAD_CAST "ID"); idAttribute != nullptr) {
            xmlAddID(nullptr, document, BAD_CAST id.c_str(), idAttribute);
        }

        const xmlNodePtr signature = xmlSecTmplSignatureCreate(document, xmlSecTransformExclC14NId,
                                                               xmlSecOpenSSLTransformRsaSha256Id, nullptr);
        BOOST_REQUIRE(signature != nullptr);
        xmlAddChild(element, signature);

        const xmlNodePtr reference = xmlSecTmplSignatureAddReference(signature, xmlSecOpenSSLTransformSha256Id, nullptr,
                                                                     BAD_CAST("#" + id).c_str(), nullptr);
        BOOST_REQUIRE(reference != nullptr);
        BOOST_REQUIRE(xmlSecTmplReferenceAddTransform(reference, xmlSecTransformEnvelopedId) != nullptr);
        BOOST_REQUIRE(xmlSecTmplReferenceAddTransform(reference, xmlSecTransformExclC14NId) != nullptr);

        const xmlSecDSigCtxPtr context = xmlSecDSigCtxCreate(nullptr);
        BOOST_REQUIRE(context != nullptr);
        context->signKey = xmlSecOpenSSLAppKeyLoadMemory(reinterpret_cast<const xmlSecByte *>(keyPem.data()), keyPem.size(),
                                                         xmlSecKeyDataFormatPem, nullptr, nullptr, nullptr);
        BOOST_REQUIRE(context->signKey != nullptr);
        BOOST_REQUIRE(xmlSecDSigCtxSign(context, signature) >= 0);
        xmlSecDSigCtxDestroy(context);
    }

    std::string buildResponse(const AssertionOptions &options = {}) {

        static const bool initialised = [] {
            xmlInitParser();
            BOOST_REQUIRE(xmlSecInit() >= 0);
            BOOST_REQUIRE(xmlSecOpenSSLAppInit(nullptr) >= 0);
            BOOST_REQUIRE(xmlSecOpenSSLInit() >= 0);
            return true;
        }();
        std::ignore = initialised;

        const auto xml = responseTemplate(options);
        const xmlDocPtr document = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "response.xml", nullptr, XML_PARSE_NONET);
        BOOST_REQUIRE(document != nullptr);

        const xmlNodePtr response = xmlDocGetRootElement(document);
        xmlNodePtr assertion = nullptr;
        for (xmlNodePtr child = response->children; child != nullptr; child = child->next) {
            if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, BAD_CAST "Assertion") == 0) assertion = child;
        }
        BOOST_REQUIRE(assertion != nullptr);

        if (options.signAssertion) signElement(document, assertion, options.assertionId, options.signingKey);
        if (options.signResponse) signElement(document, response, "_response-0001", options.signingKey);

        xmlChar *dumped = nullptr;
        int size = 0;
        xmlDocDumpMemory(document, &dumped, &size);
        std::string result(reinterpret_cast<const char *>(dumped), size);
        xmlFree(dumped);
        xmlFreeDoc(document);
        return result;
    }

    SamlConfiguration configuration() {
        SamlConfiguration config;
        config.enabled = true;
        config.entityId = kSpEntityId;
        config.acsUrl = kAcsUrl;
        config.idpEntityId = kIdpEntityId;
        config.idpSsoUrl = "https://example.onelogin.com/trust/saml2/http-redirect/sso/12345";
        config.idpCertificate = kProviderCertificate;
        config.emailAttribute = "email";
        return config;
    }

    std::optional<Euclid::Core::FederatedIdentity> verify(const std::string &xml, std::string &error,
                                                          const SamlConfiguration &config = configuration(),
                                                          const std::string &inResponseTo = kRequestId) {
        return SamlResponseVerifier::Verify(xml, config, inResponseTo, error);
    }

}// namespace

// ── the good case ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_signed_assertion_yields_the_identity) {

    std::string error;
    const auto identity = verify(buildResponse(), error);

    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->provider == "saml");
    BOOST_TEST(identity->subject == "jane.federated@example.com");
    BOOST_TEST(identity->userId == "jane.federated");
    BOOST_TEST(identity->email == "jane.federated@example.com");
    BOOST_TEST(identity->assertionId == "_assertion-0001");
}

BOOST_AUTO_TEST_CASE(a_signed_response_around_the_assertion_is_accepted_too) {

    // Providers differ about which element they sign, and both are legitimate.
    AssertionOptions options;
    options.signAssertion = false;
    options.signResponse = true;

    std::string error;
    const auto identity = verify(buildResponse(options), error);
    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->subject == "jane.federated@example.com");
}

// ── the signature ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_unsigned_assertion_is_refused) {

    AssertionOptions options;
    options.signAssertion = false;

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error == "Neither the response nor the assertion is signed");
}

BOOST_AUTO_TEST_CASE(an_assertion_signed_by_a_stranger_is_refused) {

    AssertionOptions options;
    options.signingKey = kStrangerKey;

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(!error.empty());
}

BOOST_AUTO_TEST_CASE(an_assertion_altered_after_signing_is_refused) {

    // The subject is rewritten in the signed document. The signature still verifies as a
    // signature; what fails is the digest over what it covers.
    auto xml = buildResponse();
    const auto position = xml.find("jane.federated@example.com");
    BOOST_REQUIRE(position != std::string::npos);
    xml.replace(position, std::string("jane.federated@example.com").size(), "admin.federated@example.com");

    std::string error;
    BOOST_TEST(!verify(xml, error).has_value());
    BOOST_TEST(!error.empty());
}

BOOST_AUTO_TEST_CASE(a_wrapped_assertion_is_refused) {

    // Signature wrapping: the real, signed assertion is kept - so the signature verifies - and a
    // forged one is added beside it in the hope that the reader takes the wrong one. The rule that
    // stops it is that there has to be exactly one assertion in the document.
    const auto signedXml = buildResponse();

    const auto assertionStart = signedXml.find("<saml:Assertion");
    const auto assertionEnd = signedXml.find("</saml:Assertion>") + std::string("</saml:Assertion>").size();
    BOOST_REQUIRE(assertionStart != std::string::npos);

    auto forged = signedXml.substr(assertionStart, assertionEnd - assertionStart);
    forged = forged.replace(forged.find("jane.federated@example.com"), std::string("jane.federated@example.com").size(), "admin@example.com");
    forged = forged.replace(forged.find("_assertion-0001"), std::string("_assertion-0001").size(), "_assertion-0002");

    const auto wrapped = signedXml.substr(0, assertionStart) + forged + signedXml.substr(assertionStart);

    std::string error;
    BOOST_TEST(!verify(wrapped, error).has_value());
    BOOST_TEST(error == "The response carries more than one assertion");
}

BOOST_AUTO_TEST_CASE(a_duplicated_id_is_refused) {

    // The other half of the same family: two elements claiming the same ID, so that a reference
    // can be made to resolve to whichever one the reader happens to reach first.
    auto xml = buildResponse();
    const auto position = xml.find("ID=\"_response-0001\"");
    BOOST_REQUIRE(position != std::string::npos);
    xml.replace(position, std::string("ID=\"_response-0001\"").size(), "ID=\"_assertion-0001\"");

    std::string error;
    BOOST_TEST(!verify(xml, error).has_value());
    BOOST_TEST(error == "SAML response uses the same ID twice");
}

BOOST_AUTO_TEST_CASE(an_encrypted_assertion_is_refused_with_a_useful_message) {

    const auto xml = std::string(R"(<?xml version="1.0"?><samlp:Response xmlns:samlp="urn:oasis:names:tc:SAML:2.0:protocol" )")
                     + R"(xmlns:saml="urn:oasis:names:tc:SAML:2.0:assertion" ID="_r1" Version="2.0"><saml:EncryptedAssertion/></samlp:Response>)";

    std::string error;
    BOOST_TEST(!verify(xml, error).has_value());
    BOOST_TEST(error.find("encrypted") != std::string::npos);
}

// ── the contents ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_assertion_from_another_issuer_is_refused) {

    AssertionOptions options;
    options.issuer = "https://not-your.onelogin.com/saml/metadata/999";

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error.find("not by the configured identity provider") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_assertion_for_another_service_provider_is_refused) {

    // One provider serves many services with one key. Without the audience check, an assertion
    // minted for any of the others would be accepted here.
    AssertionOptions options;
    options.audience = "https://someone-elses-service.example.com/saml";

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error.find("another service provider") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_assertion_naming_no_audience_is_refused) {

    AssertionOptions options;
    options.audience = "";

    std::string error;
    const auto xml = buildResponse(options);
    BOOST_TEST(!verify(xml, error).has_value());
}

BOOST_AUTO_TEST_CASE(an_expired_assertion_is_refused) {

    AssertionOptions options;
    options.notBefore = std::chrono::hours(-2);
    options.notOnOrAfter = std::chrono::hours(-1);

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error == "The assertion has expired");
}

BOOST_AUTO_TEST_CASE(an_assertion_from_the_future_is_refused) {

    AssertionOptions options;
    options.notBefore = std::chrono::hours(1);
    options.notOnOrAfter = std::chrono::hours(2);

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error == "The assertion is not valid yet");
}

BOOST_AUTO_TEST_CASE(a_refusal_from_the_provider_is_reported_as_one) {

    AssertionOptions options;
    options.statusCode = "urn:oasis:names:tc:SAML:2.0:status:AuthnFailed";

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error.find("refused the login") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_assertion_meant_for_another_endpoint_is_refused) {

    AssertionOptions options;
    options.recipient = "https://another-euclid.example.com/eam/saml/acs";

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error.find("was meant for") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_assertion_answering_a_different_request_is_refused) {

    AssertionOptions options;
    options.inResponseTo = "_some-other-login-attempt";

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error == "The assertion answers a different login request");
}

BOOST_AUTO_TEST_CASE(an_assertion_with_no_subject_confirmation_is_refused) {

    AssertionOptions options;
    options.withSubjectConfirmation = false;

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
    BOOST_TEST(error == "The assertion carries no subject confirmation");
}

// ── unsolicited assertions ───────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_unsolicited_assertion_is_refused_unless_it_is_enabled) {

    AssertionOptions options;
    options.inResponseTo = "";

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error, configuration(), "").has_value());
    BOOST_TEST(error.find("Unsolicited logins are not enabled") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_unsolicited_assertion_is_accepted_when_it_is_enabled) {

    AssertionOptions options;
    options.inResponseTo = "";

    auto config = configuration();
    config.allowIdpInitiated = true;

    std::string error;
    const auto identity = verify(buildResponse(options), error, config, "");
    BOOST_REQUIRE_MESSAGE(identity.has_value(), error);
    BOOST_TEST(identity->userId == "jane.federated");
}

BOOST_AUTO_TEST_CASE(an_assertion_answering_a_request_nobody_made_is_refused) {

    // The mirror image: this installation started no login, but the assertion claims to answer
    // one. Something is being replayed.
    auto config = configuration();
    config.allowIdpInitiated = true;

    std::string error;
    BOOST_TEST(!verify(buildResponse(), error, config, "").has_value());
    BOOST_TEST(error.find("did not make") != std::string::npos);
}

// ── configuration ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_disabled_configuration_has_nothing_to_complain_about) {

    const SamlConfiguration config;
    BOOST_TEST(config.Validate().empty());
}

BOOST_AUTO_TEST_CASE(an_enabled_configuration_names_what_is_missing) {

    SamlConfiguration config;
    config.enabled = true;

    // entity-id, acs-url, idp-entity-id and a certificate; the SSO URL is only needed to start a
    // login, and is reported by ValidateForAuthentication().
    BOOST_TEST(config.Validate().size() == 4);
    BOOST_TEST(config.ValidateForAuthentication().size() == 5);
}

BOOST_AUTO_TEST_CASE(an_sso_url_is_needed_only_to_start_a_login) {

    // An installation whose people fetch assertions from their provider's API never sends anybody
    // anywhere, so it has no use for an SSO URL - and must not be refused for the want of one.
    auto config = configuration();
    config.idpSsoUrl.clear();

    BOOST_TEST(config.Validate().empty());
    BOOST_TEST(config.ValidateForAuthentication().size() == 1);
    BOOST_TEST(config.ValidateForAuthentication().front().find("idp-sso-url") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_assertion_says_what_to_configure) {

    // The way out of the chicken and egg problem: what euclid has to be told about a provider is
    // written in any assertion that provider sends.
    const auto description = Euclid::Core::SamlResponseVerifier::Describe(buildResponse());

    BOOST_REQUIRE(description.has_value());
    BOOST_TEST(description->issuer == kIdpEntityId);
    BOOST_TEST(description->audience == kSpEntityId);
    BOOST_TEST(description->recipient == kAcsUrl);
    BOOST_TEST(description->nameId == "jane.federated@example.com");
    BOOST_TEST(description->hasSignature);
    BOOST_REQUIRE(description->attributes.size() == 1);
    BOOST_TEST(description->attributes.front() == "email = jane.federated@example.com");
}

BOOST_AUTO_TEST_CASE(describing_checks_nothing) {

    // Worth stating in a test, because it would be a bad thing to forget: this reads what an
    // unauthenticated document claims. An assertion signed by a stranger describes itself just as
    // readily, and Verify() is what decides whether any of it is true.
    AssertionOptions options;
    options.signingKey = kStrangerKey;

    const auto description = Euclid::Core::SamlResponseVerifier::Describe(buildResponse(options));
    BOOST_REQUIRE(description.has_value());
    BOOST_TEST(description->issuer == kIdpEntityId);

    std::string error;
    BOOST_TEST(!verify(buildResponse(options), error).has_value());
}

BOOST_AUTO_TEST_CASE(something_that_is_not_a_response_describes_as_nothing) {

    BOOST_TEST(!Euclid::Core::SamlResponseVerifier::Describe("<html>not saml</html>").has_value());
    BOOST_TEST(!Euclid::Core::SamlResponseVerifier::Describe("").has_value());
}

BOOST_AUTO_TEST_CASE(two_certificates_are_one_too_many) {

    auto config = configuration();
    config.idpCertificateFile = "/etc/somewhere/else.crt";

    BOOST_TEST(!config.Validate().empty());
}

BOOST_AUTO_TEST_CASE(the_metadata_states_what_the_provider_has_to_know) {

    const Euclid::Core::SamlProvider provider(configuration(), "an-installation-jwt-secret-that-is-long-enough");
    const auto metadata = provider.Metadata();

    BOOST_TEST(metadata.find(kSpEntityId) != std::string::npos);
    BOOST_TEST(metadata.find(kAcsUrl) != std::string::npos);
    BOOST_TEST(metadata.find("urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_authentication_request_is_deflated_onto_the_sso_url) {

    const Euclid::Core::SamlProvider provider(configuration(), "an-installation-jwt-secret-that-is-long-enough");
    const auto authentication = provider.BeginAuthentication("/console");

    BOOST_TEST(authentication.url.starts_with("https://example.onelogin.com/trust/saml2/http-redirect/sso/12345?SAMLRequest="));
    BOOST_TEST(authentication.url.find("RelayState=") != std::string::npos);
    BOOST_TEST(!authentication.requestId.empty());

    // The RelayState carries where to go afterwards, and does not show it to the browser.
    BOOST_TEST(authentication.relayState.find("console") == std::string::npos);
}
