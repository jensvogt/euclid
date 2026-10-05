// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <filesystem>
#include <string>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::EAP::LocalNode {

    /**
     * @brief Provisions the worker that runs on the manager's own host - docs/worker-nodes.md §13.3.
     *
     * @par What it does
     * On every start of EAP, when `euclid.modules.eap.local-node.enabled` is set:
     *   - makes sure a principal for the node exists - `local-node`, in the installation's first
     *     account, with no login, one access key, and the built-in `node` role;
     *   - writes the credentials file the worker signs with, holding that key;
     *   - writes the worker's whole configuration, generated from the manager's: the gateway as
     *     reached from this host, the certificate it serves, the runtimes this host has, and a port
     *     range of its own.
     * Each is written only when it differs from what is there, so a restart changes nothing.
     *
     * @par Why the manager writes the worker's configuration
     * Because everything in it is the manager's to know - its port, whether it serves TLS and with
     * which certificate, where this host's JDKs are - and a second file somebody keeps in step by
     * hand is how the two come to disagree. Generated rather than copied, so nothing in euclid.json
     * that is not listed here can reach it: least of all euclid.modules.eam.jwt-secret, which a
     * worker refuses to start with.
     *
     * @par Who can read it
     * Both files are the worker's identity, so they are written owner- and group-readable only,
     * with the group set to `euclid.modules.eap.local-node.group` (`euclid-wrk`) - the worker's
     * account, which the package makes the manager's account a member of so it can hand them over.
     * Nothing else on the host reads them.
     */
    void Provision();

    /**
     * @brief The configuration a local worker is given, from the manager's own - see Provision().
     * Separated so it can be asserted without writing anything.
     *
     * @param credentialsPath where the worker's credentials file is.
     */
    [[nodiscard]] boost::json::object WorkerConfiguration(const std::filesystem::path &credentialsPath);

}// namespace Euclid::EAP::LocalNode
