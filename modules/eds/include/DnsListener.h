// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

// Euclid includes
#include <Catalog.h>

namespace Euclid::EDS {

    /**
     * @brief Where and how the name server listens.
     */
    struct ListenerOptions {

        /**
         * @brief Address to bind; "0.0.0.0" for every IPv4 interface, "::" for every interface.
         */
        std::string address{"0.0.0.0"};

        /**
         * @brief UDP and TCP port.
         *
         * @par
         * Not 53, which needs CAP_NET_BIND_SERVICE and euclid runs unprivileged - see docs/eds.md
         * for putting it there. And not 5353 either, though that is the usual "unprivileged DNS"
         * port: it is multicast DNS, which Avahi and Bonjour already hold on most machines, and a
         * socket bound beside them would be handed every mDNS packet on the network and answer it.
         */
        unsigned short port{1053};

        /**
         * @brief Threads for the sockets themselves.
         */
        int ioThreads{2};

        /**
         * @brief Threads that answer queries, forwarded ones included - a forwarded query holds
         * one for up to forwardTimeout per upstream.
         */
        int workerThreads{8};

        /**
         * @brief Upstream resolvers for names in no zone here: "192.0.2.53", "192.0.2.53:5300",
         * "[2001:db8::53]:53". Empty means such names are refused.
         */
        std::vector<std::string> forwarders;

        /**
         * @brief Networks whose clients may have names forwarded, as CIDRs.
         *
         * @par
         * Loopback and the private ranges by default. A forwarder that answers anybody is an open
         * resolver, which the internet uses as a traffic amplifier against third parties - a small
         * spoofed query buys a large answer sent to the victim. Authoritative answers are given to
         * every client whatever this says; they are small and only about names stored here.
         */
        std::vector<std::string> allowRecursion{"127.0.0.0/8", "::1/128", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "fc00::/7"};

        /**
         * @brief How long one upstream is waited for before the next is tried.
         */
        std::chrono::milliseconds forwardTimeout{2000};

        /**
         * @brief How long a TCP connection may sit idle between queries (RFC 7766 §6.2.3).
         */
        std::chrono::seconds tcpIdleTimeout{10};
    };

    /**
     * @brief Whether an address is in one of the networks, given as CIDRs. An IPv4-mapped IPv6
     * address is matched as the IPv4 address it carries. A CIDR that does not parse matches
     * nothing.
     */
    bool InNetworks(const boost::asio::ip::address &address, const std::vector<std::string> &networks);

    /**
     * @brief The DNS server: UDP and TCP on one port, answering from the Catalog.
     */
    class DnsListener {

    public:
        DnsListener(const Catalog &catalog, ListenerOptions options);
        ~DnsListener();

        DnsListener(const DnsListener &) = delete;
        DnsListener &operator=(const DnsListener &) = delete;

        /**
         * @brief Binds both sockets and starts serving.
         *
         * @throws boost::system::system_error when a socket cannot be bound - the port is taken,
         * or it is below 1024 and the process may not bind it.
         */
        void Start();

        /**
         * @brief Stops serving and joins every thread.
         */
        void Stop();

        /**
         * @brief Whether both sockets are bound.
         */
        [[nodiscard]] bool Serving() const { return _serving; }

        [[nodiscard]] const ListenerOptions &Options() const { return _options; }

        /**
         * @brief Answers one request: parses it, resolves it from the zones or forwards it, and
         * writes the response.
         *
         * @param request the request, as received.
         * @param client who sent it, which decides whether it may be forwarded.
         * @param tcp whether it came over TCP, which decides how large the response may be.
         * @return the response, or empty when nothing should be sent at all - a message too short
         * to have a header, or one that is itself a response.
         */
        [[nodiscard]] std::vector<std::uint8_t> Answer(std::span<const std::uint8_t> request,
                                                       const boost::asio::ip::address &client, bool tcp) const;

    private:
        class TcpSession;

        void receiveUdp();
        void acceptTcp();

        [[nodiscard]] std::optional<std::vector<std::uint8_t> > forward(std::span<const std::uint8_t> request, std::uint16_t id) const;

        const Catalog &_catalog;
        ListenerOptions _options;

        boost::asio::io_context _io;
        boost::asio::strand<boost::asio::io_context::executor_type> _udpStrand;
        boost::asio::ip::udp::socket _udp;
        boost::asio::ip::tcp::acceptor _acceptor;
        boost::asio::ip::udp::endpoint _udpSender;
        std::array<std::uint8_t, 65535> _udpBuffer{};

        std::unique_ptr<boost::asio::thread_pool> _workers;
        std::vector<std::thread> _ioThreads;
        bool _serving{false};
    };

}// namespace Euclid::EDS
