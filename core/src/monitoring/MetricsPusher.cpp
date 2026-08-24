// C++ includes
#include <chrono>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/monitoring/MetricsPusher.h>
#include <euclid/core/monitoring/MonitoringCollector.h>

namespace Euclid::Core::Monitoring {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;
    using namespace boost::beast::http;

    namespace {
        MetricsPusher::ModuleSocketLookup &moduleSocketLookup() {
            static MetricsPusher::ModuleSocketLookup lookup;
            return lookup;
        }

        // Connects to a module instance's own Unix socket and issues a "push-metrics" action -
        // mirrors MonitoringServer's (now-removed) pollMetrics client, just as a push instead of
        // a poll. Kept as its own small client rather than shared with any server-side code,
        // consistent with how this codebase already duplicates raw UDS connect/write/read
        // clients per call site (see GatewayServer::forwardToService, Controller.cpp's
        // waitForSocket).
        bool pushMetrics(const std::string &socketPath, const std::string &body) {
            try {
                namespace local = asio::local;
                asio::io_context ioc;
                local::stream_protocol::socket sock(ioc);
                sock.connect(local::stream_protocol::endpoint(socketPath));

                request<string_body> req{verb::post, "/", 11};
                req.set("x-euclid-action", "push-metrics");
                req.body() = body;
                req.prepare_payload();
                write(sock, req);

                beast::flat_buffer buf;
                response<string_body> res;
                read(sock, buf, res);

                return res.result() == status::ok;

            } catch (const std::exception &e) {
                log_warning << "MetricsPusher: failed to push to " << socketPath << ", error: " << e.what();
                return false;
            }
        }
    }// namespace

    void MetricsPusher::SetModuleSocketLookup(ModuleSocketLookup lookup) {
        moduleSocketLookup() = std::move(lookup);
    }

    MetricsPusher::MetricsPusher(std::string moduleName) : _moduleName(std::move(moduleName)) {

        auto &scheduler = Scheduler::instance();
        scheduler.Start();

        const auto period = std::chrono::seconds(Configuration::instance().getOr<long>("euclid.monitoring.push-period", 10));
        _taskId = scheduler.SchedulePeriodic("metrics-push-" + _moduleName, [this] { push(); },
                                              std::chrono::duration_cast<std::chrono::milliseconds>(period));
    }

    MetricsPusher::~MetricsPusher() {
        Scheduler::instance().Cancel(_taskId);
    }

    void MetricsPusher::push() const {

        const auto samples = MonitoringCollector::instance().Collect();
        if (samples.empty()) return;

        const auto &lookup = moduleSocketLookup();
        if (!lookup) return;

        const auto sockets = lookup("emo");
        if (sockets.empty()) return;

        boost::json::array items;
        for (const auto &sample: samples) {
            items.push_back(boost::json::object{
                    {"name", sample.name},
                    {"labelName", sample.labelName},
                    {"labelValue", sample.labelValue},
                    {"value", sample.value},
                    {"type", sample.isRate ? "rate" : "gauge"}
            });
        }
        const auto body = boost::json::serialize(boost::json::object{{"module", _moduleName}, {"items", items}});

        for (const auto &socketPath: sockets) {
            pushMetrics(socketPath, body);
        }
    }

}// namespace Euclid::Core::Monitoring
