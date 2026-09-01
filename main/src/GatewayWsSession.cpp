// C++ includes
#include <algorithm>

// Boost includes
#include <boost/asio/strand.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/WsFrame.h>
#include <euclid/manager/GatewayServer.h>
#include <euclid/manager/GatewayWsRegistry.h>
#include <euclid/manager/GatewayWsSession.h>

namespace Euclid::main {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    namespace asio = boost::asio;

    namespace {

        std::size_t MaxOutboxFrames() {
            constexpr long kDefault = 256;
            return static_cast<std::size_t>(Core::Configuration::instance().getOr<long>("euclid.gateway.websocket.max-outbox-frames", kDefault));
        }

        std::size_t MaxOutboxBytes() {
            constexpr long kDefault = 8L * 1024 * 1024;
            return static_cast<std::size_t>(Core::Configuration::instance().getOr<long>("euclid.gateway.websocket.max-outbox-bytes", kDefault));
        }

        // Queues one frame and either starts the write loop or closes the connection - see Outbox
        // for which frames may be dropped on the way and why. Runs on the websocket's executor.
        // Takes the two things it has to be able to do rather than the session itself, so the
        // plaintext and TLS sessions - which share no base class, only a shape - can both use it.
        template<class StartWrite, class Close>
        void Enqueue(Outbox &outbox, std::string frame, const bool isEvent, StartWrite startWrite, Close close) {
            switch (outbox.Push(std::move(frame), isEvent)) {
                case Outbox::Result::Idle:
                    startWrite();
                    return;
                case Outbox::Result::Writing:
                    return;
                case Outbox::Result::Overflow:
                    // Nothing droppable left: the backlog is all frames somebody is waiting for,
                    // so it is the connection that is the problem, not the pace.
                    log_warning << "GatewayWsSession: outbox overflow with no droppable frames, closing connection";
                    close();
            }
        }

        // Called when the queue has drained: tells a client that fell behind how much it missed,
        // which for a durable subscription is a precise instruction - the events are still in the
        // store, so claim them.
        template<class StartWrite, class Close>
        void ReportDroppedEvents(Outbox &outbox, StartWrite startWrite, Close close) {
            const auto dropped = outbox.TakeDropped();
            if (dropped == 0) return;
            log_warning << "GatewayWsSession: dropped events for a client that fell behind, count: " << dropped;
            Enqueue(outbox, boost::json::serialize(boost::json::object{{"type", "lag"}, {"dropped", dropped}}), false,
                    std::move(startWrite), std::move(close));
        }

        std::size_t MaxMessageSize() {
            constexpr long kDefault = 1L * 1024 * 1024;
            return static_cast<std::size_t>(Core::Configuration::instance().getOr<long>("euclid.gateway.websocket.max-message-size", kDefault));
        }

        std::chrono::seconds IdleTimeout() {
            constexpr long kDefault = 300;
            return std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.gateway.websocket.idle-timeout-seconds", kDefault));
        }

        template<class Stream>
        void ConfigureWs(websocket::stream<Stream> &ws) {
            auto opt = websocket::stream_base::timeout::suggested(beast::role_type::server);
            opt.idle_timeout = IdleTimeout();
            ws.set_option(opt);
            ws.read_message_max(MaxMessageSize());
        }

        // Shared by GatewayWsSession::handleRequestFrame() and GatewayWsTlsSession's - parses one
        // inbound frame and, if it's a well-formed request, dispatches the backend UDS round-trip
        // onto ioc so it runs independently of the read loop and of any other in-flight request
        // on the same connection (see GatewayWsSession.h's class doc comment). self is kept alive
        // for the duration via the captured shared_ptr.
        template<class SessionPtr>
        void DispatchFrame(SessionPtr self, boost::asio::io_context &ioc, ServiceController &ctrl, const std::string &authorization,
                           const std::string &accountId, const std::string &region, const std::string &ns, const std::string &text) {

            std::string error;
            auto parsed = Core::WsFrame::ParseRequest(text, error);
            if (!parsed.has_value()) {
                self->PostFrame(Core::WsFrame::BuildErrorFrame("", 400, error));
                return;
            }

            asio::post(ioc, [self, &ctrl, authorization, accountId, region, ns, request = std::move(*parsed)]() mutable {
                const auto id = request.id;
                const auto target = request.target;
                const auto httpReq = Core::WsFrame::ToHttpRequest(request, authorization, accountId, region, ns);

                const auto handle = ctrl.acquireInstance(target);
                if (!handle) {
                    self->PostFrame(Core::WsFrame::BuildErrorFrame(id, 503, "service '" + target + "' not registered or not running"));
                    return;
                }

                // Mirrors GatewayServer.cpp route()'s identical guard - guarantees the
                // autoscaler's active-request count is released on every exit path below.
                struct ReleaseGuard {
                    ServiceController &ctrl;
                    const std::string &name;
                    pid_t pid;
                    ~ReleaseGuard() { ctrl.releaseInstance(name, pid); }
                } guard{.ctrl = ctrl, .name = target, .pid = handle->pid};

                const auto res = forwardToService(httpReq, handle->socketPath);
                self->PostFrame(Core::WsFrame::BuildResponseFrame(id, res));
            });
        }

        // The body of the ees call a subscribe frame turns into. A frame that named no
        // subscriber gets one of its own: it cannot outlive the connection that sent it, so the
        // subscription is live (nothing is stored for it) and ephemeral (removed when the
        // connection ends). A frame that named one is describing a subscription meant to outlast
        // the socket, so it defaults to durable.
        boost::json::object BuildEesSubscribeBody(const Core::WsFrame::ParsedSubscription &parsed, const std::string &name, const bool anonymous) {
            boost::json::array eventTypes;
            eventTypes.push_back(boost::json::string(parsed.topic));

            auto mode = parsed.mode;
            if (mode.empty()) mode = anonymous ? "live" : "durable";

            return boost::json::object{
                    {"name", name},
                    {"eventTypes", std::move(eventTypes)},
                    {"filter", parsed.filter},
                    {"mode", mode},
                    {"ephemeral", anonymous},
            };
        }

        // Calls the ees module with one already-built action body, on the io_context so the read
        // loop is never held up by the round trip, and answers the client with the ack or with
        // why it failed.
        template<class SessionPtr>
        void CallEes(SessionPtr self, asio::io_context &ioc, ServiceController &ctrl, const std::string &authorization,
                     const std::string &accountId, const std::string &region, const std::string &ns,
                     const std::string &action, boost::json::object body, const std::string &id,
                     const std::string &ackType, const std::string &name) {

            asio::post(ioc, [self, &ctrl, authorization, accountId, region, ns, action, body = std::move(body), id, ackType, name]() mutable {
                const Core::WsFrame::Request request{.id = id, .target = "ees", .action = action, .body = std::move(body)};
                const auto httpReq = Core::WsFrame::ToHttpRequest(request, authorization, accountId, region, ns);

                const auto handle = ctrl.acquireInstance("ees");
                if (!handle) {
                    // Deliberately still an ack: an installation running without the ees module
                    // keeps the per-connection delivery it always had, and a client that would
                    // otherwise see its subscribe frame fail has no way to ask for that instead.
                    log_warning << "GatewayWsSession: ees not available, subscription is connection-local, name: " << name;
                    self->PostFrame(Core::WsFrame::BuildSubscriptionAckFrame(id, ackType, name));
                    return;
                }

                struct ReleaseGuard {
                    ServiceController &ctrl;
                    pid_t pid;
                    ~ReleaseGuard() { ctrl.releaseInstance("ees", pid); }
                } guard{.ctrl = ctrl, .pid = handle->pid};

                const auto res = forwardToService(httpReq, handle->socketPath);
                if (res.result_int() >= 300) {
                    self->PostFrame(Core::WsFrame::BuildErrorFrame(id, res.result_int(), res.body()));
                    return;
                }
                self->PostFrame(Core::WsFrame::BuildSubscriptionAckFrame(id, ackType, name));
            });
        }

        // Shared by GatewayWsSession::handleSubscriptionFrame() and GatewayWsTlsSession's - one
        // inbound subscribe/unsubscribe frame.
        //
        // A subscription frame is an EES subscription: the same names, filters and modes a
        // client would use over HTTP, so a stream is described once and can be attached to from
        // anywhere, and what a connection receives was decided when the event was published.
        //
        // Naming nothing still works and still means "just this connection": the gateway names
        // the subscription for it and marks it ephemeral.
        template<class SessionPtr>
        void HandleSubscriptionFrame(SessionPtr self, asio::io_context &ioc, ServiceController &ctrl, const std::string &authorization,
                                     const std::string &accountId, const std::string &region, const std::string &ns,
                                     std::mutex &mutex, std::set<std::string> &subscriberNames, std::string &ephemeralName,
                                     const std::string &text) {

            std::string error;
            const auto parsed = Core::WsFrame::ParseSubscription(text, error);
            if (!parsed.has_value()) {
                self->PostFrame(Core::WsFrame::BuildErrorFrame("", 400, error));
                return;
            }

            const bool subscribing = parsed->type == "subscribe";
            const bool anonymous = parsed->name.empty();

            std::string name;
            {
                std::lock_guard lock(mutex);
                if (anonymous) {
                    // One per connection, created the first time it is needed and reused after,
                    // so a client that subscribes to three topics ends up with one subscription
                    // carrying three event types rather than three subscriptions.
                    if (ephemeralName.empty()) ephemeralName = "ws-" + Core::UuidUtils::CreateRandomUuid();
                    name = ephemeralName;
                } else {
                    name = parsed->name;
                }

                if (subscribing) {
                    // Added rather than replacing what was there: one connection commonly carries
                    // several listeners, and attaching to a second subscription must not silently
                    // stop the first from being delivered.
                    subscriberNames.insert(name);
                } else if (parsed->topic.empty()) {
                    // A name and no topic: stop receiving, leave the subscription alone. That is
                    // what detaching means, and it is the only way to stop listening without also
                    // throwing away what a durable subscription has collected.
                    subscriberNames.erase(name);
                }
            }

            // Nothing to register or remove: the frame named a subscription and said no more, so
            // it was an attach or a detach.
            if (parsed->topic.empty()) {
                self->PostFrame(Core::WsFrame::BuildSubscriptionAckFrame(parsed->id, subscribing ? "subscribed" : "unsubscribed", name));
                return;
            }

            const auto body = subscribing
                                  ? BuildEesSubscribeBody(*parsed, name, anonymous)
                                  : boost::json::object{{"name", name}, {"eventType", parsed->topic}};

            CallEes(self, ioc, ctrl, authorization, accountId, region, ns,
                    subscribing ? "subscribe-events" : "unsubscribe-events", body, parsed->id,
                    subscribing ? "subscribed" : "unsubscribed", name);
        }

        // Removes the subscription a connection created for itself, once that connection is gone.
        // Only ever the ephemeral one: a named subscription belongs to the client, not to the
        // socket, and outliving the connection is the point of it.
        void RemoveEphemeralSubscription(asio::io_context &ioc, ServiceController &ctrl, const std::string &authorization,
                                         const std::string &accountId, const std::string &region, const std::string &ns,
                                         const std::string &name) {

            if (name.empty()) return;

            asio::post(ioc, [&ctrl, authorization, accountId, region, ns, name] {
                const Core::WsFrame::Request request{
                        .id = "", .target = "ees", .action = "unsubscribe-events",
                        .body = boost::json::object{{"name", name}}};

                const auto handle = ctrl.acquireInstance("ees");
                if (!handle) return;

                struct ReleaseGuard {
                    ServiceController &ctrl;
                    pid_t pid;
                    ~ReleaseGuard() { ctrl.releaseInstance("ees", pid); }
                } guard{.ctrl = ctrl, .pid = handle->pid};

                std::ignore = forwardToService(Core::WsFrame::ToHttpRequest(request, authorization, accountId, region, ns), handle->socketPath);
            });
        }

    }// namespace

    // ── GatewayWsSession ─────────────────────────────────────────────────────

    GatewayWsSession::GatewayWsSession(beast::tcp_stream stream, asio::io_context &ioc, ServiceController &ctrl,
                                       std::string authorization, std::string accountId, std::string region, std::string ns)
        : _ws(std::move(stream)), _ioc(ioc), _ctrl(ctrl), _authorization(std::move(authorization)), _accountId(std::move(accountId)),
          _region(std::move(region)), _namespace(std::move(ns)), _strand(asio::make_strand(ioc)),
          _outbox(MaxOutboxFrames(), MaxOutboxBytes()) {}

    void GatewayWsSession::run(http::request<http::string_body> req) {
        ConfigureWs(_ws);
        _ws.async_accept(req, [self = shared_from_this()](const beast::error_code &ec) { self->onAccept(ec); });
    }

    void GatewayWsSession::onAccept(const beast::error_code &ec) {
        if (ec) {
            log_warning << "GatewayWsSession: handshake failed, error: " << ec.message();
            return;
        }
        GatewayWsRegistry::instance().Register(_accountId, _region, weak_from_this());
        doRead();
    }

    void GatewayWsSession::doRead() {
        _ws.async_read(_buffer, [self = shared_from_this()](const beast::error_code &ec, const std::size_t bytes) { self->onRead(ec, bytes); });
    }

    void GatewayWsSession::onRead(const beast::error_code &ec, std::size_t) {
        if (ec) return;// EOF, timeout, etc. - session destructs once every outstanding handler drops its shared_ptr

        if (_ws.got_text()) handleFrame(beast::buffers_to_string(_buffer.data()));
        _buffer.consume(_buffer.size());
        doRead();
    }

    void GatewayWsSession::handleFrame(const std::string &text) {
        const auto type = Core::WsFrame::FrameType(text);
        if (type == "subscribe" || type == "unsubscribe") {
            handleSubscriptionFrame(text);
            return;
        }
        handleRequestFrame(text);
    }

    void GatewayWsSession::handleRequestFrame(const std::string &text) {
        DispatchFrame(shared_from_this(), _ioc, _ctrl, _authorization, _accountId, _region, _namespace, text);
    }

    void GatewayWsSession::handleSubscriptionFrame(const std::string &text) {
        HandleSubscriptionFrame(shared_from_this(), _ioc, _ctrl, _authorization, _accountId, _region, _namespace,
                                _subscriptionsMutex, _subscriberNames, _ephemeralName, text);
    }

    GatewayWsSession::~GatewayWsSession() {
        RemoveEphemeralSubscription(_ioc, _ctrl, _authorization, _accountId, _region, _namespace, _ephemeralName);
    }

    bool GatewayWsSession::IsAttachedTo(const std::string &name) const {
        std::lock_guard lock(_subscriptionsMutex);
        return _subscriberNames.contains(name);
    }


    void GatewayWsSession::PostFrame(std::string frame) {
        asio::post(_strand, [self = shared_from_this(), frame = std::move(frame)]() mutable {
            Enqueue(self->_outbox, std::move(frame), false, [self] { self->doWrite(); }, [self] { self->closeSocket(); });
        });
    }

    void GatewayWsSession::PostEvent(std::string frame) {
        asio::post(_strand, [self = shared_from_this(), frame = std::move(frame)]() mutable {
            Enqueue(self->_outbox, std::move(frame), true, [self] { self->doWrite(); }, [self] { self->closeSocket(); });
        });
    }

    void GatewayWsSession::closeSocket() {
        beast::error_code ignored;
        beast::get_lowest_layer(_ws).socket().close(ignored);
    }

    void GatewayWsSession::doWrite() {
        _outbox.BeginWrite();
        _ws.text(true);
        _ws.async_write(asio::buffer(_outbox.Front()),
                        asio::bind_executor(_strand, [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                            if (ec) {
                                // Closed explicitly rather than left alone: without this the queue keeps its
                                // frames and no further write is ever started, so the session sits there holding
                                // them until the read side happens to fail too.
                                self->_outbox.FailWrite();
                                log_debug << "GatewayWsSession: write failed, closing connection, error: " << ec.message();
                                self->closeSocket();
                                return;
                            }
                            self->_outbox.CompleteWrite();
                            if (!self->_outbox.Empty()) self->doWrite();
                            else ReportDroppedEvents(self->_outbox, [self] { self->doWrite(); }, [self] { self->closeSocket(); });
                        }));
    }

    // ── GatewayWsTlsSession ──────────────────────────────────────────────────

    GatewayWsTlsSession::GatewayWsTlsSession(beast::ssl_stream<beast::tcp_stream> stream, asio::io_context &ioc, ServiceController &ctrl,
                                             std::string authorization, std::string accountId, std::string region, std::string ns)
        : _ws(std::move(stream)), _ioc(ioc), _ctrl(ctrl), _authorization(std::move(authorization)), _accountId(std::move(accountId)),
          _region(std::move(region)), _namespace(std::move(ns)), _strand(asio::make_strand(ioc)),
          _outbox(MaxOutboxFrames(), MaxOutboxBytes()) {}

    void GatewayWsTlsSession::run(http::request<http::string_body> req) {
        ConfigureWs(_ws);
        _ws.async_accept(req, [self = shared_from_this()](const beast::error_code &ec) { self->onAccept(ec); });
    }

    void GatewayWsTlsSession::onAccept(const beast::error_code &ec) {
        if (ec) {
            log_warning << "GatewayWsTlsSession: handshake failed, error: " << ec.message();
            return;
        }
        GatewayWsRegistry::instance().Register(_accountId, _region, weak_from_this());
        doRead();
    }

    void GatewayWsTlsSession::doRead() {
        _ws.async_read(_buffer, [self = shared_from_this()](const beast::error_code &ec, const std::size_t bytes) { self->onRead(ec, bytes); });
    }

    void GatewayWsTlsSession::onRead(const beast::error_code &ec, std::size_t) {
        if (ec) return;

        if (_ws.got_text()) handleFrame(beast::buffers_to_string(_buffer.data()));
        _buffer.consume(_buffer.size());
        doRead();
    }

    void GatewayWsTlsSession::handleFrame(const std::string &text) {
        const auto type = Core::WsFrame::FrameType(text);
        if (type == "subscribe" || type == "unsubscribe") {
            handleSubscriptionFrame(text);
            return;
        }
        handleRequestFrame(text);
    }

    void GatewayWsTlsSession::handleRequestFrame(const std::string &text) {
        DispatchFrame(shared_from_this(), _ioc, _ctrl, _authorization, _accountId, _region, _namespace, text);
    }

    void GatewayWsTlsSession::handleSubscriptionFrame(const std::string &text) {
        HandleSubscriptionFrame(shared_from_this(), _ioc, _ctrl, _authorization, _accountId, _region, _namespace,
                                _subscriptionsMutex, _subscriberNames, _ephemeralName, text);
    }

    GatewayWsTlsSession::~GatewayWsTlsSession() {
        RemoveEphemeralSubscription(_ioc, _ctrl, _authorization, _accountId, _region, _namespace, _ephemeralName);
    }

    bool GatewayWsTlsSession::IsAttachedTo(const std::string &name) const {
        std::lock_guard lock(_subscriptionsMutex);
        return _subscriberNames.contains(name);
    }


    void GatewayWsTlsSession::PostFrame(std::string frame) {
        asio::post(_strand, [self = shared_from_this(), frame = std::move(frame)]() mutable {
            Enqueue(self->_outbox, std::move(frame), false, [self] { self->doWrite(); }, [self] { self->closeSocket(); });
        });
    }

    void GatewayWsTlsSession::PostEvent(std::string frame) {
        asio::post(_strand, [self = shared_from_this(), frame = std::move(frame)]() mutable {
            Enqueue(self->_outbox, std::move(frame), true, [self] { self->doWrite(); }, [self] { self->closeSocket(); });
        });
    }

    void GatewayWsTlsSession::closeSocket() {
        beast::error_code ignored;
        beast::get_lowest_layer(_ws).socket().close(ignored);
    }

    void GatewayWsTlsSession::doWrite() {
        _outbox.BeginWrite();
        _ws.text(true);
        _ws.async_write(asio::buffer(_outbox.Front()),
                        asio::bind_executor(_strand, [self = shared_from_this()](const beast::error_code &ec, std::size_t) {
                            if (ec) {
                                // Closed explicitly rather than left alone: without this the queue keeps its
                                // frames and no further write is ever started, so the session sits there holding
                                // them until the read side happens to fail too.
                                self->_outbox.FailWrite();
                                log_debug << "GatewayWsSession: write failed, closing connection, error: " << ec.message();
                                self->closeSocket();
                                return;
                            }
                            self->_outbox.CompleteWrite();
                            if (!self->_outbox.Empty()) self->doWrite();
                            else ReportDroppedEvents(self->_outbox, [self] { self->doWrite(); }, [self] { self->closeSocket(); });
                        }));
    }

}// namespace Euclid::main