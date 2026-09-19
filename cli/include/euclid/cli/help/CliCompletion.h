// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <functional>
#include <string>
#include <vector>

namespace Euclid::CLI::Completion {

    /**
     * @brief The token that asks an action for its option names instead of its help.
     *
     * @par
     * Recognised by IsHelpRequest(), so an action short-circuits to PrintActionHelp() before it
     * parses anything, authenticates or calls a server - which is what makes completion free and
     * safe to run on a keystroke. PrintActionHelp() then fills the sink below rather than printing
     * a man page.
     *
     * @par
     * Deliberately ugly and deliberately not documented in help: it is the interface between this
     * binary and the shell, not something anybody types.
     */
    constexpr auto kOptionsToken = "--euclid-complete-options";

    /**
     * @brief Where PrintActionHelp() puts option names while completing, or null when it should
     * print help as usual.
     */
    std::vector<std::string> *OptionSink();

    /**
     * @brief Points PrintActionHelp() at a collector, or at nothing to restore normal help.
     */
    void SetOptionSink(std::vector<std::string> *sink);

    /**
     * @brief The line being completed, split the way the shell sees it.
     *
     * @param words the whole command line as tokens, without the program name. The word under the
     * cursor is the last of them, and is empty when the cursor sits after a space.
     */
    struct Request {
        std::vector<std::string> words;
    };

    /**
     * @brief Splits a command line at the cursor into the tokens before it and the word being
     * completed.
     *
     * @par
     * Quoting is handled only as far as it has to be - a quoted value is one word, and a value
     * still being typed inside an open quote is the word under the cursor. Nothing else about
     * shell syntax matters here: by the time a pipe or a redirect appears the shell has stopped
     * asking us.
     *
     * @param line the command line, as COMP_LINE gives it, including the program name
     * @param point how far along it the cursor is, as COMP_POINT gives it
     * @return the tokens after the program name, the last of which is the word under the cursor
     */
    [[nodiscard]]
    Request Split(const std::string &line, std::size_t point);

    /**
     * @brief What may follow what has been typed so far.
     *
     * @par
     * Pure, and takes its tables as arguments, so the rules can be tested without a binary that
     * knows thirteen modules - the same reason the autoscaler's arithmetic lives apart from the
     * manager.
     *
     * @par
     * The rules, in the order they apply:
     *   - nothing typed yet, or a partial first word: the modules
     *   - a module and nothing more: that module's actions, and "help"
     *   - a module, an action, and a word starting with "-": that action's options
     *   - a word starting with "-" before any module: the global options
     *   - anything else: nothing, rather than a guess
     *
     * Everything is filtered by the word under the cursor, because a completer invoked with
     * `complete -C` is what does the filtering - the shell prints whatever it is handed.
     *
     * @param request the split command line
     * @param modules every module name
     * @param actionsOf the actions of a module, empty for a name that is not one
     * @param optionsOf the option names of an action, empty when there are none
     * @param globalOptions the options that come before the module
     * @return the candidates, sorted, without duplicates
     */
    [[nodiscard]]
    std::vector<std::string> Candidates(const Request &request,
                                        const std::vector<std::string> &modules,
                                        const std::function<std::vector<std::string>(const std::string &)> &actionsOf,
                                        const std::function<std::vector<std::string>(const std::string &, const std::string &)> &optionsOf,
                                        const std::vector<std::string> &globalOptions);

}// namespace Euclid::CLI::Completion
