//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <memory>

// Euclid includes
#include <BasicAuthenticator.h>
#include <Backends.h>
#include <ClientStream.h>
#include <RouteTable.h>

namespace Euclid::EAG {

    /**
     * @brief What a listener speaks to the callers that reach it.
     */
    enum class Protocol {

        /**
         * @brief Plain HTTP. What a gateway behind a load balancer that has already terminated
         * TLS wants, and what an installation reached only over a private network can live with.
         */
        HTTP,

        /**
         * @brief HTTPS, terminated by the gateway itself with a certificate from the key
         * management module.
         */
        HTTPS
    };

    /**
     * @brief Reads a protocol name as it is written in the configuration.
     *
     * @param protocol "http" or "https", in any case.
     * @return the protocol, or nothing if the name is neither - which is refused rather than
     * defaulted, since a typo silently becoming HTTP would publish in clear text a port somebody
     * believed was encrypted.
     */
    [[nodiscard]]
    std::optional<Protocol> ProtocolFromString(std::string protocol);

    /**
     * @brief The name of a protocol, for logs and messages.
     */
    [[nodiscard]]
    std::string ProtocolToString(Protocol protocol);

    /**
     * @brief The API gateway's own listener: the port callers actually talk to.
     *
     * @par
     * Separate from the Unix socket the manager supervises this process through, in the same way a
     * transfer server has both. The socket carries euclid's own traffic - the route table is
     * managed through it - and this carries everybody else's.
     *
     * @par
     * A request arriving here is matched against the route table by path, authenticated if its
     * route asks for it, and forwarded to one of the instances of the application that route
     * names. The caller never learns which instance answered, and the application never learns it
     * was proxied: the path it receives is the path that was asked for.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ProxyServer {

    public:

        /**
         * @brief Binds the listener and starts serving.
         *
         * @param port TCP port to listen on.
         * @param threads io_context worker threads.
         * @param refreshSeconds how often the route table and backend list are re-read.
         */
        /**
         * @brief One port the gateway answers on, and the namespace its routes belong to.
         *
         * @par
         * Several of them is how one installation serves more than one environment without the
         * environment appearing in anybody's URL: development on 8080 and integration on 8081 can
         * both publish "/api/produktmeldungen", because the port says which is meant. The
         * alternative - one port and the namespace in the path - puts euclid's own structure into
         * addresses that outlive it, and makes the application see a segment that is nothing to do
         * with it.
         */
        struct Listener {

            /**
             * @brief TCP port to answer on.
             */
            unsigned short port{};

            /**
             * @brief Namespace whose routes this port serves. Empty serves every route, which is
             * what a single-listener installation gets.
             */
            std::string nameSpace;

            /**
             * @brief Whether this port speaks HTTP or HTTPS.
             *
             * @par
             * Per listener rather than per installation, because the same gateway commonly needs
             * both: a public port that terminates TLS itself, and one reached only from inside a
             * network or from a load balancer that has already done so. Defaults to HTTP, which
             * is what every listener written before this attribute existed was.
             */
            Protocol protocol{Protocol::HTTP};

            /**
             * @brief Name of the EKM certificate this port serves, when it speaks HTTPS.
             *
             * @par
             * A name rather than a pair of file paths: the certificate is key material and lives
             * where euclid keeps key material, which is also what makes it listable, replaceable
             * and watchable for expiry through the CLI. Empty takes the conventional name for the
             * listener's namespace, and a certificate that does not exist yet is generated -
             * self-signed - so that an installation can serve HTTPS before anybody has bought it
             * a certificate.
             */
            std::string certificate;
        };

        ProxyServer(std::vector<Listener> listeners, int threads, long refreshSeconds, long basicAuthCacheSeconds,
                    unsigned short euclidGatewayPort, bool euclidGatewayTls, const std::string &euclidGatewayCert);

        ~ProxyServer();

        ProxyServer(const ProxyServer &) = delete;
        ProxyServer &operator=(const ProxyServer &) = delete;

        /**
         * @brief Starts accepting, and the refresh loop that keeps the routes current.
         */
        void start();

        /**
         * @brief Stops accepting and joins the workers.
         */
        void stop();

        /**
         * @brief Whether the ports are actually bound and answering.
         *
         * @par
         * Not the same question as "was a listener configured": binding fails when a port is
         * taken or a certificate cannot be loaded, and the module keeps running so that the route
         * table can still be managed. Something asking what the gateway serves needs to be told
         * the difference.
         */
        [[nodiscard]]
        bool serving() const { return _running.load(); }

    private:

        /**
         * @brief Accepts one connection and re-arms itself.
         */
        void accept(std::size_t index);

        /**
         * @brief Serves one connection: complete the handshake if there is one, read a request,
         * route it, answer it.
         */
        void serve(boost::asio::ip::tcp::socket socket, std::size_t index);

        /**
         * @brief Matches one request to a route, authenticates it if the route says so, and
         * forwards it to one of the application's instances.
         */
        void route(const std::string &nameSpace,
                   const std::shared_ptr<ClientStream> &stream,
                   const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request);

        /**
         * @brief Forwards one request to a backend port and returns whatever comes back.
         */
        void proxyTo(const std::shared_ptr<ClientStream> &stream,
                     const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request,
                     int port, const std::string &routeId,
                     const std::string &euclidTarget, const std::string &euclidAction);

        /**
         * @brief Proxies to euclid's own gateway over TLS.
         *
         * @par
         * Separate from proxyTo() rather than a flag on it, because the stream type differs all
         * the way down and there is a handshake in the middle. Used when
         * euclid.gateway.tls.enabled is set, which it is by default - a module route that spoke
         * plain HTTP to it would be answered with a dropped connection and nothing else.
         */
        void proxyToTls(const std::shared_ptr<ClientStream> &stream,
                        const std::shared_ptr<boost::beast::http::request<boost::beast::http::string_body> > &request,
                        int port, const std::string &routeId,
                        const std::string &euclidTarget, const std::string &euclidAction);

        /**
         * @brief Writes one response to the caller and closes the connection.
         */
        static void respond(const std::shared_ptr<ClientStream> &stream,
                            const std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body> > &response);

        /**
         * @brief Re-reads the route table, then the backends of whatever it now names.
         *
         * @par
         * In that order and in one place, so the two can never disagree about which applications
         * matter: a route added a moment ago has its instances looked up in the same pass.
         */
        void refresh();

        std::vector<Listener> _listeners;
        int _threads;
        std::chrono::seconds _refreshInterval;

        boost::asio::io_context _ioc;

        /**
         * @brief One acceptor per listener, in the same order, so an accepted connection knows
         * which namespace it arrived for.
         */
        std::vector<boost::asio::ip::tcp::acceptor> _acceptors;

        /**
         * @brief One server context per listener, in the same order again, holding the
         * certificate and key that listener terminates TLS with. Null for a listener that speaks
         * plain HTTP.
         *
         * @par
         * Built once when the gateway starts rather than per connection: a context re-read and
         * re-parsed for every handshake would put the cost of loading a certificate on the path
         * of every caller.
         */
        std::vector<std::shared_ptr<boost::asio::ssl::context> > _serverContexts;

        std::vector<std::thread> _workers;
        std::thread _refreshThread;
        std::atomic<bool> _running{false};

        RouteTable _routes;
        Backends _backends;

        /**
         * @brief Checks Basic credentials for the routes that ask for them.
         */
        BasicAuthenticator _basicAuth;

        /**
         * @brief Port euclid's own gateway listens on, where a route naming a module is sent.
         */
        unsigned short _euclidGatewayPort;

        /**
         * @brief Whether euclid's own gateway expects TLS. Mirrors euclid.gateway.tls.enabled.
         */
        bool _euclidGatewayTls;

        /**
         * @brief The installation's region, supplied for callers that do not name one.
         */
        std::string _region;

        /**
         * @brief Client context for talking to it, built once because building one per request
         * would re-read and re-parse the certificate every time.
         */
        boost::asio::ssl::context _euclidGatewayCtx;
    };

}// namespace Euclid::EAG
