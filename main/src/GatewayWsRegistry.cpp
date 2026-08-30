// Euclid includes
#include <euclid/core/WsFrame.h>
#include <euclid/manager/GatewayWsRegistry.h>

namespace Euclid::main {

    GatewayWsRegistry &GatewayWsRegistry::instance() {
        static GatewayWsRegistry registry;
        return registry;
    }

    std::string GatewayWsRegistry::key(const std::string &accountId, const std::string &region) {
        return accountId + "|" + region;
    }

    void GatewayWsRegistry::Register(const std::string &accountId, const std::string &region, const std::weak_ptr<IWsSession> &session) {
        std::lock_guard lock(_mutex);
        _sessionsByScope[key(accountId, region)].push_back(session);
    }

    void GatewayWsRegistry::Broadcast(const std::string &topic, const std::string &accountId, const std::string &region, const boost::json::object &body) {

        std::vector<std::shared_ptr<IWsSession> > live;
        {
            std::lock_guard lock(_mutex);
            auto it = _sessionsByScope.find(key(accountId, region));
            if (it == _sessionsByScope.end()) return;

            auto &sessions = it->second;
            // Prune dead entries (connection already closed) while collecting live ones under
            // the same lock, so Broadcast() self-cleans without needing an explicit
            // deregistration hook anywhere in a session's teardown path.
            std::erase_if(sessions, [&live](const std::weak_ptr<IWsSession> &weak) {
                if (auto session = weak.lock()) {
                    live.push_back(std::move(session));
                    return false;
                }
                return true;
            });
            if (sessions.empty()) _sessionsByScope.erase(it);
        }

        if (live.empty()) return;

        const auto frame = Core::WsFrame::BuildEventFrame(topic, accountId, region, body);
        for (const auto &session: live) {
            // Delivery is opt-in: a session with no matching "subscribe" frame on record gets
            // nothing, even though it's in scope for accountId/region - see WsFrame's class doc
            // comment for why (every session would otherwise see every queue's/key's/etc. events).
            if (session->WantsEvent(topic, body)) session->PostFrame(frame);
        }
    }

}// namespace Euclid::main
