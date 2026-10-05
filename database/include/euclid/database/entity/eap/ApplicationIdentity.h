// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <map>
#include <string>

// Euclid includes
#include <euclid/database/entity/eap/Application.h>

namespace Euclid::Database::Entity::EAP {

    /**
     * @brief The namespace an application's own requests run in.
     *
     * @par
     * Its own field when it has one. When it does not - every application deployed before
     * applications carried one, and anything created by a client that does not set it yet - the
     * application's EAM user is asked instead: a user granted exactly one namespace in this account
     * leaves no room for doubt about which was meant. Several, or none, and it stays empty rather
     * than guessing, which resolves names at the account root.
     *
     * @par
     * Here rather than in the manager because it decides what goes into an application's
     * credentials, and those are issued in two places: by the manager for what it runs itself, and
     * by EAP's issue-instance-credentials for what a worker runs. Asked in only one of them, the
     * same application resolved names differently depending on which host it landed on.
     */
    [[nodiscard]] std::string ApplicationNamespace(const Application &application);

    /**
     * @brief What an application's process is told about itself, from its definition: the
     * application's own environment, then the EUCLID_* variables that describe it.
     *
     * @par
     * The definition's half of the environment, the same on every host. The host's half - where
     * the gateway is from here, which certificate to trust, where the credentials file is - is
     * added by whoever starts the process, see Core::Launch::AddHostEnvironment(). The manager
     * calls both for what it runs; EAP sends this to a worker with each assignment and the worker
     * adds its own.
     *
     * @par
     * The application's own entries go on first, so it cannot shadow any of these.
     *
     * @param application the definition.
     * @param withAccessKey whether to pass on the long-lived access key of an operator-named,
     * login-enabled user. The manager and EAP's renewal both do, so an application has the same
     * environment on a node as on the manager; false is for a caller that must not hand the key on.
     */
    [[nodiscard]] std::map<std::string, std::string> ApplicationEnvironment(const Application &application, bool withAccessKey);

}// namespace Euclid::Database::Entity::EAP
