// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>

// Boost includes
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

// Euclid includes
#include <DnsListener.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/monitoring/MetricEventBus.h>

namespace Euclid::EDS {

    namespace asio = boost::asio;
    using asio::ip::tcp;
    using asio::ip::udp;

    namespace {

        // Zone transfers are not offered: the zones are in the database, and a secondary that
        // wants them can read them through the API with a credential, which AXFR has no notion of.
        constexpr std::uint16_t kIxfr = 251;
        constexpr std::uint16_t kAxfr = 252;

        constexpr std::size_t kHeaderSize = 12;

        std::string rcodeName(const std::uint8_t rcode) {
            switch (rcode) {
                case Dns::Rcode::NoError: return "NOERROR";
                case Dns::Rcode::FormErr: return "FORMERR";
                case Dns::Rcode::ServFail: return "SERVFAIL";
                case Dns::Rcode::NxDomain: return "NXDOMAIN";
                case Dns::Rcode::NotImp: return "NOTIMP";
                case Dns::Rcode::Refused: return "REFUSED";
                default: return std::to_string(rcode);
            }
        }

        void count(const std::string &outcome) {
            Core::Monitoring::MetricEventBus::instance().sigMetricRate("eds-query-count", "outcome", outcome);
        }

        // "192.0.2.53", "192.0.2.53:5300", "2001:db8::53" or "[2001:db8::53]:5300".
        std::optional<udp::endpoint> parseEndpoint(const std::string &text) {

            std::string host = text;
            unsigned short port = 53;

            if (text.starts_with('[')) {
                const auto close = text.find(']');
                if (close == std::string::npos) return std::nullopt;
                host = text.substr(1, close - 1);
                if (close + 1 < text.size()) {
                    if (text[close + 1] != ':') return std::nullopt;
                    try {
                        port = static_cast<unsigned short>(std::stoul(text.substr(close + 2)));
                    } catch (...) {
                        return std::nullopt;
                    }
                }
            } else if (std::ranges::count(text, ':') == 1) {
                const auto colon = text.find(':');
                host = text.substr(0, colon);
                try {
                    port = static_cast<unsigned short>(std::stoul(text.substr(colon + 1)));
                } catch (...) {
                    return std::nullopt;
                }
            }

            boost::system::error_code ec;
            const auto address = asio::ip::make_address(host, ec);
            if (ec) return std::nullopt;
            return udp::endpoint(address, port);
        }

    }// namespace

    bool InNetworks(const asio::ip::address &address, const std::vector<std::string> &networks) {

        auto client = address;
        if (client.is_v6() && client.to_v6().is_v4_mapped()) {
            client = asio::ip::make_address_v4(asio::ip::v4_mapped, client.to_v6());
        }

        for (const auto &cidr: networks) {
            boost::system::error_code ec;
            if (client.is_v4()) {
                const auto network = asio::ip::make_network_v4(cidr, ec);
                if (ec) continue;
                if (asio::ip::network_v4(client.to_v4(), network.prefix_length()).canonical() == network.canonical()) return true;
            } else {
                const auto network = asio::ip::make_network_v6(cidr, ec);
                if (ec) continue;
                if (asio::ip::network_v6(client.to_v6(), network.prefix_length()).canonical() == network.canonical()) return true;
            }
        }
        return false;
    }

    // ── TCP ──────────────────────────────────────────────────────────────────

    /**
     * @brief One TCP connection: length-prefixed queries (RFC 1035 §4.2.2), answered in order,
     * until the client closes or goes quiet for tcpIdleTimeout.
     */
    class DnsListener::TcpSession : public std::enable_shared_from_this<TcpSession> {

    public:
        TcpSession(const DnsListener &listener, tcp::socket socket)
            : _listener(listener), _socket(std::move(socket)), _timer(_socket.get_executor()) {
            boost::system::error_code ec;
            const auto remote = _socket.remote_endpoint(ec);
            if (!ec) _client = remote.address();
        }

        void Start() { readLength(); }

    private:
        void arm() {
            _timer.expires_after(_listener._options.tcpIdleTimeout);
            _timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) {
                if (ec) return;
                boost::system::error_code ignored;
                self->_socket.close(ignored);
            });
        }

        void readLength() {
            arm();
            asio::async_read(_socket, asio::buffer(_length), [self = shared_from_this()](const boost::system::error_code &ec, std::size_t) {
                if (ec) return;
                const std::size_t size = (static_cast<std::size_t>(self->_length[0]) << 8) | self->_length[1];
                if (size < kHeaderSize) {
                    boost::system::error_code ignored;
                    self->_socket.close(ignored);
                    return;
                }
                self->_body.resize(size);
                asio::async_read(self->_socket, asio::buffer(self->_body), [self](const boost::system::error_code &readError, std::size_t) {
                    if (readError) return;
                    self->_timer.cancel();
                    self->answer();
                });
            });
        }

        void answer() {
            // Off the socket's strand: a forwarded query blocks for as long as the upstream takes.
            asio::post(*_listener._workers, [self = shared_from_this()] {
                auto reply = self->_listener.Answer(self->_body, self->_client, true);
                asio::post(self->_socket.get_executor(), [self, reply = std::move(reply)]() mutable {
                    if (reply.empty()) {
                        boost::system::error_code ignored;
                        self->_socket.close(ignored);
                        return;
                    }
                    self->_out.clear();
                    self->_out.push_back(static_cast<std::uint8_t>(reply.size() >> 8));
                    self->_out.push_back(static_cast<std::uint8_t>(reply.size() & 0xFF));
                    self->_out.insert(self->_out.end(), reply.begin(), reply.end());
                    asio::async_write(self->_socket, asio::buffer(self->_out), [self](const boost::system::error_code &ec, std::size_t) {
                        if (!ec) self->readLength();
                    });
                });
            });
        }

        const DnsListener &_listener;
        tcp::socket _socket;
        asio::steady_timer _timer;
        asio::ip::address _client;
        std::array<std::uint8_t, 2> _length{};
        std::vector<std::uint8_t> _body;
        std::vector<std::uint8_t> _out;
    };

    // ── Listener ─────────────────────────────────────────────────────────────

    DnsListener::DnsListener(const Catalog &catalog, ListenerOptions options)
        : _catalog(catalog), _options(std::move(options)), _udpStrand(asio::make_strand(_io)), _udp(_io), _acceptor(_io) {}

    DnsListener::~DnsListener() {
        Stop();
    }

    void DnsListener::Start() {

        const auto address = asio::ip::make_address(_options.address);

        // No SO_REUSEADDR on the UDP socket: for UDP it lets a second process bind the same port
        // and share its datagrams, which would turn a port conflict into half the queries going
        // to somebody else instead of a bind error that says so.
        _udp.open(address.is_v4() ? udp::v4() : udp::v6());
        _udp.bind(udp::endpoint(address, _options.port));

        _acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6());
        _acceptor.set_option(tcp::acceptor::reuse_address(true));
        _acceptor.bind(tcp::endpoint(address, _options.port));
        _acceptor.listen();

        _workers = std::make_unique<asio::thread_pool>(static_cast<std::size_t>(std::max(1, _options.workerThreads)));

        receiveUdp();
        acceptTcp();

        for (int i = 0; i < std::max(1, _options.ioThreads); ++i) {
            _ioThreads.emplace_back([this] {
                // Kept running with nothing pending, so a moment with no outstanding operation -
                // between a datagram and re-arming the receive - does not end the thread.
                auto guard = asio::make_work_guard(_io);
                _io.run();
            });
        }
        _serving = true;
        log_info << "EDS listening, address: " << _options.address << ", port: " << _options.port << " (udp, tcp)"
                 << ", forwarders: " << _options.forwarders.size();
    }

    void DnsListener::Stop() {
        if (!_serving && _ioThreads.empty()) return;
        _serving = false;

        // Workers first, so none of them posts a reply into a context that has gone; then the
        // sockets' threads, and only then the sockets, which nothing is touching any more.
        if (_workers) {
            _workers->stop();
            _workers->join();
        }
        _io.stop();
        for (auto &thread: _ioThreads) {
            if (thread.joinable()) thread.join();
        }
        _ioThreads.clear();

        boost::system::error_code ignored;
        _udp.close(ignored);
        _acceptor.close(ignored);
    }

    void DnsListener::receiveUdp() {

        _udp.async_receive_from(asio::buffer(_udpBuffer), _udpSender, asio::bind_executor(_udpStrand, [this](const boost::system::error_code &ec, const std::size_t size) {
            if (ec == asio::error::operation_aborted || !_udp.is_open()) return;

            // Anything else - an ICMP port-unreachable surfacing as connection_refused, which a
            // UDP socket on Windows reports for a reply nobody took - is about one datagram, and
            // the socket carries on.
            if (!ec && size > 0) {
                auto request = std::make_shared<std::vector<std::uint8_t> >(_udpBuffer.begin(), _udpBuffer.begin() + static_cast<std::ptrdiff_t>(size));
                const auto sender = _udpSender;

                asio::post(*_workers, [this, request, sender] {
                    auto reply = std::make_shared<std::vector<std::uint8_t> >(Answer(*request, sender.address(), false));
                    if (reply->empty()) return;
                    asio::post(_udpStrand, [this, reply, sender] {
                        _udp.async_send_to(asio::buffer(*reply), sender, asio::bind_executor(_udpStrand, [reply](const boost::system::error_code &, std::size_t) {}));
                    });
                });
            }
            receiveUdp();
        }));
    }

    void DnsListener::acceptTcp() {

        _acceptor.async_accept(asio::make_strand(_io), [this](const boost::system::error_code &ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted || !_acceptor.is_open()) return;
            if (!ec) std::make_shared<TcpSession>(*this, std::move(socket))->Start();
            acceptTcp();
        });
    }

    std::vector<std::uint8_t> DnsListener::Answer(const std::span<const std::uint8_t> request, const asio::ip::address &client, const bool tcp) const {

        if (request.size() < kHeaderSize) return {};

        const auto parsed = Dns::Parse(request);
        if (!parsed.has_value()) {
            // Never answer a response, even a malformed one - that is how two servers end up
            // bouncing a packet between them for ever.
            if ((request[2] & 0x80) != 0) return {};
            Dns::Message formErr;
            formErr.id = static_cast<std::uint16_t>((request[0] << 8) | request[1]);
            formErr.response = true;
            formErr.rcode = Dns::Rcode::FormErr;
            count("FORMERR");
            return Dns::Serialize(formErr, Dns::ClassicUdpLimit);
        }
        if (parsed->response) return {};

        const auto limit = tcp ? std::size_t{65535}
                               : parsed->ednsUdpSize.has_value() ? std::min<std::size_t>(*parsed->ednsUdpSize, Dns::EdnsUdpLimit)
                                                                 : Dns::ClassicUdpLimit;

        const bool recursion = !_options.forwarders.empty() && InNetworks(client, _options.allowRecursion);

        const auto refuse = [&](const std::uint8_t rcode) {
            auto response = Dns::ErrorResponse(*parsed, rcode);
            response.recursionAvailable = recursion;
            count(rcodeName(rcode));
            return Dns::Serialize(response, limit);
        };

        if (parsed->opcode != 0) return refuse(Dns::Rcode::NotImp);
        if (parsed->questions.size() != 1) return refuse(Dns::Rcode::FormErr);

        const auto &question = parsed->questions.front();
        if (question.klass != Dns::ClassIn && question.klass != Dns::Type::ANY) return refuse(Dns::Rcode::Refused);
        if (question.type == kAxfr || question.type == kIxfr) return refuse(Dns::Rcode::Refused);

        const auto zones = _catalog.Current();
        const auto answer = Resolve(*zones, question.name, question.type);

        if (!answer.has_value()) {
            if (!parsed->recursionDesired || !recursion) return refuse(Dns::Rcode::Refused);

            auto forwarded = forward(request, parsed->id);
            if (!forwarded.has_value()) return refuse(Dns::Rcode::ServFail);
            if (forwarded->size() > limit) {
                // Larger than this client may be sent over UDP: tell it to ask again over TCP.
                auto truncated = Dns::ErrorResponse(*parsed, Dns::Rcode::NoError);
                truncated.recursionAvailable = true;
                truncated.truncated = true;
                count("TRUNCATED");
                return Dns::Serialize(truncated, limit);
            }
            count("FORWARDED");
            return std::move(*forwarded);
        }

        Dns::Message response;
        response.id = parsed->id;
        response.response = true;
        response.opcode = parsed->opcode;
        response.authoritative = true;
        response.recursionDesired = parsed->recursionDesired;
        response.recursionAvailable = recursion;
        response.rcode = answer->rcode;
        response.questions = parsed->questions;
        response.answers = answer->answers;
        response.authority = answer->authority;
        if (parsed->ednsUdpSize.has_value()) response.ednsUdpSize = Dns::EdnsUdpLimit;

        count(rcodeName(answer->rcode));
        return Dns::Serialize(response, limit);
    }

    std::optional<std::vector<std::uint8_t> > DnsListener::forward(const std::span<const std::uint8_t> request, const std::uint16_t id) const {

        for (const auto &forwarder: _options.forwarders) {

            const auto upstream = parseEndpoint(forwarder);
            if (!upstream.has_value()) {
                log_warning << "EDS forwarder is not an address, skipped, forwarder: " << forwarder;
                continue;
            }

            // A socket of its own per query: a fresh ephemeral port each time is half of what makes
            // a forged answer hard to land (RFC 5452), the ID being the other half.
            asio::io_context context;
            udp::socket socket(context);
            boost::system::error_code ec;
            socket.open(upstream->protocol(), ec);
            if (ec) continue;
            socket.send_to(asio::buffer(request.data(), request.size()), *upstream, 0, ec);
            if (ec) continue;

            const auto deadline = std::chrono::steady_clock::now() + _options.forwardTimeout;
            std::vector<std::uint8_t> reply(65535);

            while (true) {
                bool done = false;
                std::size_t received = 0;
                boost::system::error_code receiveError;
                udp::endpoint from;

                socket.async_receive_from(asio::buffer(reply), from, [&](const boost::system::error_code &error, const std::size_t size) {
                    receiveError = error;
                    received = size;
                    done = true;
                });
                context.restart();
                context.run_until(deadline);

                if (!done) {
                    socket.cancel(ec);
                    context.restart();
                    context.run();
                    log_debug << "EDS forwarder did not answer in time, forwarder: " << forwarder;
                    break;
                }
                if (receiveError) break;

                // Only the upstream's answer to this question: anything else arriving on the port
                // is ignored and the wait goes on, up to the same deadline.
                if (from != *upstream || received < kHeaderSize) continue;
                if (static_cast<std::uint16_t>((reply[0] << 8) | reply[1]) != id) continue;

                reply.resize(received);
                return reply;
            }
        }
        return std::nullopt;
    }

}// namespace Euclid::EDS
