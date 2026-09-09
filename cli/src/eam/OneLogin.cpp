// C++ includes
#include <cstdlib>
#include <vector>

// Euclid includes
#include <euclid/cli/eam/OneLogin.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/core/Totp.h>
#include <euclid/core/WebClient.h>

namespace Euclid::CLI {

    namespace {

        // Reads a setting the three ways it can be given, in the order that lets the more specific
        // one win: what the environment says overrides what the file says.
        std::string setting(const std::string &configKey, const char *environmentVariable) {

            if (environmentVariable != nullptr) {
                if (const char *value = std::getenv(environmentVariable); value != nullptr && *value != '\0') {
                    return value;
                }
            }
            return Core::Configuration::instance().getOr<std::string>("euclid.cli.onelogin." + configKey, "");
        }

        std::string apiBase(const std::string &subDomain) {
            return "https://" + subDomain + ".onelogin.com";
        }

        boost::json::value parseJson(const std::string &body, const std::string &what) {
            boost::system::error_code ec;
            auto parsed = boost::json::parse(body, ec);
            if (ec) throw OneLoginError("OneLogin's answer to " + what + " was not JSON");
            return parsed;
        }

        // OneLogin reports failures in more than one shape, and the useful part is in a different
        // place in each. All of them are worth passing on verbatim: "Authentication Failed" and
        // "MFA token is invalid" are different problems for whoever is reading them.
        std::string errorMessage(const boost::json::value &answer) {

            if (!answer.is_object()) return {};
            const auto &object = answer.as_object();

            if (const auto *status = object.if_contains("status"); status != nullptr && status->is_object()) {
                const auto message = Core::GetStringValue(*status, "message");
                const auto type = Core::GetStringValue(*status, "type");
                if (!message.empty()) return type.empty() ? message : type + ": " + message;
            }
            if (const auto message = Core::GetStringValue(answer, "message"); !message.empty()) return message;
            if (const auto name = Core::GetStringValue(answer, "name"); !name.empty()) return name;
            return {};
        }

    }// namespace

    // ── Configuration ────────────────────────────────────────────────────────

    OneLoginConfiguration OneLoginConfiguration::Read() {

        OneLoginConfiguration config;
        config.subDomain = setting("sub-domain", "EUCLID_ONELOGIN_SUBDOMAIN");
        config.baseUrl = setting("base-url", "EUCLID_ONELOGIN_BASE_URL");
        config.clientId = setting("client-id", "EUCLID_ONELOGIN_CLIENT_ID");
        config.clientSecret = setting("client-secret", "EUCLID_ONELOGIN_CLIENT_SECRET");
        config.user = setting("user", "EUCLID_ONELOGIN_USER");
        config.password = setting("password", "EUCLID_ONELOGIN_PASSWORD");
        config.otpKey = setting("otp-key", "EUCLID_ONELOGIN_OTP_KEY");
        config.application = setting("application", "EUCLID_ONELOGIN_APPLICATION");

        // One application, or several by name - "int" and "prod", typically. Both spellings are
        // accepted because an installation with one environment should not have to invent a name
        // for it.
        if (const auto single = setting("app-id", "EUCLID_ONELOGIN_APP_ID"); !single.empty()) {
            config.applications["default"] = single;
        }
        if (Core::Configuration::instance().has("euclid.cli.onelogin.app-ids")) {
            for (const auto &[name, value]: Core::Configuration::instance().getObject("euclid.cli.onelogin.app-ids")) {
                if (std::holds_alternative<std::string>(value)) config.applications[name] = std::get<std::string>(value);
                else if (std::holds_alternative<long>(value)) config.applications[name] = std::to_string(std::get<long>(value));
            }
        }
        return config;
    }

    std::string OneLoginConfiguration::ApplicationId() const {

        if (applications.empty()) return {};

        if (!application.empty()) {
            const auto named = applications.find(application);
            return named == applications.end() ? std::string{} : named->second;
        }
        if (applications.size() == 1) return applications.begin()->second;

        const auto fallback = applications.find("default");
        return fallback == applications.end() ? std::string{} : fallback->second;
    }

    std::vector<std::string> OneLoginConfiguration::Validate() const {

        std::vector<std::string> problems;
        if (subDomain.empty() && baseUrl.empty()) problems.emplace_back("onelogin.sub-domain is not set");
        if (clientId.empty()) problems.emplace_back("onelogin.client-id is not set");
        if (clientSecret.empty()) problems.emplace_back("onelogin.client-secret is not set");
        if (user.empty()) problems.emplace_back("onelogin.user is not set");

        if (applications.empty()) {
            problems.emplace_back("onelogin.app-id (or onelogin.app-ids) is not set");
        } else if (ApplicationId().empty()) {
            problems.emplace_back(application.empty()
                                          ? "onelogin.app-ids names several applications; say which one with --application"
                                          : "onelogin.app-ids does not name an application called '" + application + "'");
        }
        return problems;
    }

    // ── Client ───────────────────────────────────────────────────────────────

    OneLoginClient::OneLoginClient(OneLoginConfiguration config, std::string caCertPath)
        : _config(std::move(config)), _caCertPath(std::move(caCertPath)) {}

    OneLoginClient::AssertionAnswer OneLoginClient::ParseAssertionAnswer(const boost::json::value &answer) {

        // Two API versions, two envelopes. Version 1 wraps everything in {"status":..,"data":[..]}
        // and version 2 answers more directly; both are met in the wild, because which one a
        // tenant's endpoint speaks is not something a client gets to choose.
        const boost::json::value *payload = &answer;
        if (answer.is_object()) {
            if (const auto *data = answer.as_object().if_contains("data"); data != nullptr) {
                if (data->is_string()) {
                    // The assertion itself, which is what an account without a second factor gets.
                    return {.assertion = std::string(data->as_string())};
                }
                if (data->is_array() && !data->as_array().empty()) payload = &data->as_array().front();
                else if (data->is_object()) payload = data;
            }
        }

        if (payload->is_string()) return {.assertion = std::string(payload->as_string())};
        if (!payload->is_object()) throw OneLoginError("OneLogin answered in a shape this client does not understand");

        AssertionAnswer result;
        result.message = errorMessage(answer);
        result.stateToken = Core::GetStringValue(*payload, "state_token");

        // The device to satisfy. More than one may be offered; the first is what a person with one
        // authenticator has, and choosing between them is not something an unattended login can do.
        if (const auto *devices = payload->as_object().if_contains("devices");
            devices != nullptr && devices->is_array() && !devices->as_array().empty()) {

            const auto &device = devices->as_array().front();
            if (device.is_object()) {
                if (const auto *id = device.as_object().if_contains("device_id"); id != nullptr) {
                    result.deviceId = id->is_string() ? std::string(id->as_string())
                                                      : std::to_string(Core::GetLongValue(device, "device_id"));
                }
                result.deviceType = Core::GetStringValue(device, "device_type");
            }
        }

        if (result.assertion.empty() && result.stateToken.empty()) {
            throw OneLoginError(result.message.empty() ? "OneLogin returned neither an assertion nor a second-factor challenge"
                                                       : result.message);
        }
        return result;
    }

    std::string OneLoginClient::SamlAssertion(const std::string &password, const std::string &oneTimeCode,
                                              const OneTimeCodeProvider &askForCode) const {

        const auto base = _config.baseUrl.empty() ? apiBase(_config.subDomain) : _config.baseUrl;
        const auto appId = _config.ApplicationId();

        // 1. The program identifies itself. These credentials say which installation is asking,
        // not who is signing in.
        const auto tokenAnswer = Core::WebFetch(base + "/auth/oauth2/v2/token",
                                                {.method = "POST",
                                                 .body = R"({"grant_type":"client_credentials"})",
                                                 .authorization = "Basic " + Core::CryptoUtils::Base64Encode(_config.clientId + ":" + _config.clientSecret),
                                                 .caCertFile = _caCertPath});
        const auto tokenBody = parseJson(tokenAnswer.body, "the token request");
        if (!tokenAnswer.IsSuccess()) {
            throw OneLoginError("OneLogin refused the API credentials: " +
                                (errorMessage(tokenBody).empty() ? "HTTP " + std::to_string(tokenAnswer.status) : errorMessage(tokenBody)));
        }
        const auto accessToken = Core::GetStringValue(tokenBody, "access_token");
        if (accessToken.empty()) throw OneLoginError("OneLogin returned no API access token");

        // 2. The person signs in. This is where the password goes, and nowhere else.
        const boost::json::object request{
                {"username_or_email", _config.user},
                {"password", password},
                {"app_id", appId},
                {"subdomain", _config.subDomain},
        };
        const auto assertionAnswer = Core::WebFetch(base + "/api/2/saml_assertion",
                                                    {.method = "POST",
                                                     .body = boost::json::serialize(request),
                                                     .authorization = "Bearer " + accessToken,
                                                     .caCertFile = _caCertPath});
        const auto assertionBody = parseJson(assertionAnswer.body, "the assertion request");
        if (!assertionAnswer.IsSuccess()) {
            const auto message = errorMessage(assertionBody);
            throw OneLoginError("OneLogin refused the login: " +
                                (message.empty() ? "HTTP " + std::to_string(assertionAnswer.status) : message));
        }

        const auto parsed = ParseAssertionAnswer(assertionBody);
        if (!parsed.assertion.empty()) return parsed.assertion;

        // 3. The second factor, which most accounts will have.
        // In the order that costs the person the least: what they already gave, then what can be
        // computed for them, and only then a question.
        std::string code = oneTimeCode;
        if (code.empty() && !_config.otpKey.empty()) code = Core::Totp::Code(_config.otpKey);
        if (code.empty() && askForCode) code = askForCode(parsed.deviceType);
        if (code.empty()) {
            throw OneLoginError("OneLogin wants a one-time code, and there was no way to get one: pass --otp, set"
                                " EUCLID_ONELOGIN_OTP_KEY or onelogin.otp-key, or run this where a terminal can ask for it");
        }

        const boost::json::object verification{
                {"app_id", appId},
                {"device_id", parsed.deviceId},
                {"state_token", parsed.stateToken},
                {"otp_token", code},
                {"do_not_notify", true},
        };
        const auto verifyAnswer = Core::WebFetch(base + "/api/2/saml_assertion/verify_factor",
                                                 {.method = "POST",
                                                  .body = boost::json::serialize(verification),
                                                  .authorization = "Bearer " + accessToken,
                                                  .caCertFile = _caCertPath});
        const auto verifyBody = parseJson(verifyAnswer.body, "the second-factor request");
        if (!verifyAnswer.IsSuccess()) {
            const auto message = errorMessage(verifyBody);
            throw OneLoginError("OneLogin refused the one-time code: " +
                                (message.empty() ? "HTTP " + std::to_string(verifyAnswer.status) : message));
        }

        const auto verified = ParseAssertionAnswer(verifyBody);
        if (verified.assertion.empty()) {
            throw OneLoginError(verified.message.empty() ? "OneLogin accepted the one-time code but returned no assertion"
                                                         : verified.message);
        }
        return verified.assertion;
    }

}// namespace Euclid::CLI
