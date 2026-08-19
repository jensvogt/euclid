// Euclid includes
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>

// jwt-cpp includes
#include <jwt-cpp/traits/boost-json/traits.h>

namespace Euclid::Core {

    namespace {

        // Decodes and verifies token, reporting the specific failure reason via ec (cleared on
        // success). Malformed tokens that fail to even decode are reported as a generic
        // token_verification_error rather than propagating jwt-cpp's decode exception, so callers
        // only have to deal with std::error_code.
        std::optional<std::string> decodeAndVerify(const std::string &token, const std::string &secret, std::error_code &ec) {

            try {
                const auto decoded = jwt::decode<jwt::traits::boost_json>(token);

                const auto verifier = jwt::verify<jwt::traits::boost_json>()
                        .allow_algorithm(jwt::algorithm::hs256{secret});
                verifier.verify(decoded, ec);

                if (ec || !decoded.has_subject()) return std::nullopt;
                return decoded.get_subject();

            } catch (const std::exception &e) {
                // Decoding itself failed (malformed/garbled token) - not an expiry issue.
                log_debug << "JWT verification failed, error: " << e.what();
                return std::nullopt;
            }
        }

    }// namespace

    std::string JwtUtils::CreateToken(const std::string &subject, const std::string &secret, const std::chrono::seconds ttl) {

        const auto now = std::chrono::system_clock::now();
        return jwt::create<jwt::traits::boost_json>()
                .set_type("JWT")
                .set_issued_at(now)
                .set_expires_at(now + ttl)
                .set_subject(subject)
                .sign(jwt::algorithm::hs256{secret});
    }

    std::optional<std::string> JwtUtils::VerifyToken(const std::string &token, const std::string &secret) {
        std::error_code ec;
        return decodeAndVerify(token, secret, ec);
    }

    bool JwtUtils::IsTokenExpired(const std::string &token, const std::string &secret) {
        std::error_code ec;
        decodeAndVerify(token, secret, ec);
        return ec == jwt::error::token_verification_error::token_expired;
    }

}// namespace Euclid::Core