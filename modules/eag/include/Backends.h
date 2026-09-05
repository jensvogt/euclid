//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Euclid::EAG {

    /**
     * @brief Where an application's instances can be reached, and whose turn it is.
     *
     * @par
     * An application's instances are started and stopped by the manager as its pool grows and
     * shrinks, and each is given a port of its own when it starts. Nothing here decides any of
     * that - it reads what the manager recorded, which is the only place the two facts that matter
     * are kept together: which instances are running, and which port each of them holds.
     *
     * @par
     * Refreshed on a timer rather than per request, for the same reason as the route table. Two or
     * three seconds of staleness costs an unlucky request one retry; a database round trip in
     * front of every proxied call costs all of them.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Backends {

    public:

        /**
         * @brief Re-reads the running instances of every application a route points at.
         *
         * @par
         * A failed read keeps what was there. An application whose instances have genuinely gone
         * away stops answering either way, and the alternative is dropping every backend because
         * the database was briefly unreachable.
         *
         * @param applicationIds applications the route table currently names.
         */
        void refresh(const std::vector<std::string> &applicationIds);

        /**
         * @brief The next instance to send a request to, or nothing if the application has none.
         *
         * @par
         * Round robin, per application: each call advances that application's own cursor, so two
         * routes pointing at one application share the rotation rather than each hammering the
         * instance the other just used.
         *
         * @param applicationId application to reach.
         * @return the port of the instance whose turn it is.
         */
        [[nodiscard]]
        std::optional<int> next(const std::string &applicationId);

        /**
         * @brief How many instances an application currently has, for reporting.
         */
        [[nodiscard]]
        std::size_t count(const std::string &applicationId) const;

    private:

        mutable std::mutex _mutex;

        /**
         * @brief Ports of the running instances of each application.
         */
        std::map<std::string, std::vector<int> > _ports;

        /**
         * @brief Whose turn it is, per application. Never reset by a refresh: an application whose
         * pool changed size should carry on from where the rotation had reached rather than
         * starting again at the first instance and favouring it.
         */
        std::map<std::string, std::size_t> _cursors;
    };

}// namespace Euclid::EAG
