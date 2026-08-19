
// Euclid includes
#include <../include/euclid/core/HttpUtils.h>

namespace Euclid::Core {

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
