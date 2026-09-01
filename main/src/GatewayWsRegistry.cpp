// C++ includes
#include <iterator>

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

    long GatewayWsRegistry::DeliverToSubscriber(const std::string &subscriber, const std::string &topic, const std::string &accountId,
                                                const std::string &region, const boost::json::object &body) {

        if (subscriber.empty()) return 0;

        // Every scope is walked rather than one bucket looked up, because a name-addressed event
        // does not always know the region it should be delivered in - an event's payload names
        // the account it belongs to far more reliably than the region - and a subscriber name is
        // unique across the installation anyway. Region is used only when the event states one.
        std::vector<std::shared_ptr<IWsSession> > live;
        {
            std::lock_guard lock(_mutex);
            for (auto it = _sessionsByScope.begin(); it != _sessionsByScope.end();) {
                std::erase_if(it->second, [&live](const std::weak_ptr<IWsSession> &weak) {
                    if (auto session = weak.lock()) {
                        live.push_back(std::move(session));
                        return false;
                    }
                    return true;
                });
                it = it->second.empty() ? _sessionsByScope.erase(it) : std::next(it);
            }
        }

        long delivered = 0;
        std::string frame;
        for (const auto &session: live) {
            if (session->subscriberName() != subscriber) continue;
            if (!accountId.empty() && session->accountId() != accountId) continue;
            if (!region.empty() && session->region() != region) continue;
            if (frame.empty()) frame = Core::WsFrame::BuildEventFrame(topic, accountId, region, body);
            session->PostEvent(frame);
            ++delivered;
        }
        return delivered;
    }

}// namespace Euclid::main
