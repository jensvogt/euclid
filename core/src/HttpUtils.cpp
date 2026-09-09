
// Euclid includes
#include <../include/euclid/core/HttpUtils.h>

namespace Euclid::Core {

    namespace {

        std::string percentDecode(const std::string &value) {

            std::string decoded;
            decoded.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '+') {
                    decoded += ' ';
                } else if (value[i] == '%' && i + 2 < value.size()) {
                    try {
                        decoded += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
                        i += 2;
                    } catch (const std::exception &) {
                        decoded += value[i];
                    }
                } else {
                    decoded += value[i];
                }
            }
            return decoded;
        }

    }// namespace

    std::map<std::string, std::string> ParseQueryParameters(const std::string_view target) {

        std::map<std::string, std::string> parameters;

        const auto start = target.find('?');
        if (start == std::string_view::npos) return parameters;

        std::string_view query = target.substr(start + 1);
        while (!query.empty()) {
            const auto separator = query.find('&');
            if (const auto pair = query.substr(0, separator); !pair.empty()) {
                if (const auto equals = pair.find('='); equals != std::string_view::npos) {
                    parameters[percentDecode(std::string(pair.substr(0, equals)))] = percentDecode(std::string(pair.substr(equals + 1)));
                } else {
                    // A flag-shaped parameter ("?debug") is still a parameter that was there, and
                    // a caller asking whether it was present should not have to care that it
                    // carried no value.
                    parameters[percentDecode(std::string(pair))] = {};
                }
            }
            if (separator == std::string_view::npos) break;
            query.remove_prefix(separator + 1);
        }
        return parameters;
    }

    void dumpRequest(const http::request<http::string_body> &req) {
        std::cout << "=== HTTP Request ===\n" << " HTTP/" << (req.version() / 10) << '.' << (req.version() % 10) << '\n';
        for (const auto &field: req) std::cout << field.name_string() << ": " << field.value() << '\n';
        std::cout << '\n' << req.body() << '\n';
    }

    void dumpResponse(const http::response<http::string_body> &res) {
        std::cout << "=== HTTP Response ===\n" << "HTTP/" << (res.version() / 10) << '.' << (res.version() % 10) << ' ' << res.result_int() << ' ' << res.reason() << '\n';
        for (const auto &field: res) std::cout << field.name_string() << ": " << field.value() << '\n';
        std::cout << '\n' << res.body() << '\n';
    }

} // namespace Euclid::main
