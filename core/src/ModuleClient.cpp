// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Boost includes
#include <boost/asio/local/stream_protocol.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/ModuleClient.h>

namespace Euclid::Core::ModuleClient {

    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace local = boost::asio::local;

    ModuleResponse CallAt(const std::string &socketPath, const std::string &moduleName, const std::string &action,
                          const std::string &token, const std::vector<std::pair<std::string, std::string> > &headers,
                          const std::string &body) {

        try {
            boost::asio::io_context ioc;
            local::stream_protocol::socket sock(ioc);
            sock.connect(local::stream_protocol::endpoint(socketPath));

            auto req = BuildRequest(moduleName, action, token, headers, body);
            write(sock, req);

            // Beast caps a response body at 1MB unless told otherwise, which an object download
            // passes as soon as the file is bigger than a text file - the read then fails and the
            // call looks like an unreachable module. The peer is a local module answering over a
            // Unix socket with a size it has already bounded itself, so there is nothing left for
            // a limit here to protect against.
            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(boost::none);
            read(sock, buffer, parser);

            return {.status = static_cast<int>(parser.get().result_int()), .body = std::move(parser.get().body())};

        } catch (const std::exception &e) {
            log_warning << "Call to action '" << action << "' at " << socketPath << " failed, error: " << e.what();
            return {};
        }
    }

}// namespace Euclid::Core::ModuleClient
