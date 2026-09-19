// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>

// Euclid includes
#include <euclid/cli/help/CliCompletion.h>

namespace Euclid::CLI::Completion {

    namespace {

        // One per process, and only ever set around a single synchronous call in main() - the
        // completer runs, collects, and exits. A thread-local would suggest concurrency that does
        // not exist here.
        std::vector<std::string> *g_optionSink = nullptr;

        bool StartsWith(const std::string &text, const std::string &prefix) {
            return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
        }

        std::vector<std::string> Matching(const std::vector<std::string> &candidates, const std::string &prefix) {
            std::vector<std::string> matched;
            for (const auto &candidate: candidates) {
                if (StartsWith(candidate, prefix)) matched.push_back(candidate);
            }
            std::ranges::sort(matched);
            matched.erase(std::ranges::unique(matched).begin(), matched.end());
            return matched;
        }

    }// namespace

    std::vector<std::string> *OptionSink() {
        return g_optionSink;
    }

    void SetOptionSink(std::vector<std::string> *sink) {
        g_optionSink = sink;
    }

    Request Split(const std::string &line, const std::size_t point) {

        const auto upToCursor = line.substr(0, std::min(point, line.size()));

        std::vector<std::string> words;
        std::string current;
        bool inWord = false;
        char quote = '\0';

        for (const char c: upToCursor) {
            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                } else {
                    current.push_back(c);
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                inWord = true;
                continue;
            }
            if (c == ' ' || c == '\t') {
                if (inWord) {
                    words.push_back(current);
                    current.clear();
                    inWord = false;
                }
                continue;
            }
            current.push_back(c);
            inWord = true;
        }

        // The word under the cursor is the last one, and it is empty when the cursor sits after a
        // space - which is the difference between "what completes eqs" and "what completes eqs ".
        words.push_back(inWord ? current : std::string{});

        // The program name is not one of the words anybody is completing.
        if (!words.empty()) words.erase(words.begin());

        return Request{.words = words};
    }

    std::vector<std::string> Candidates(const Request &request,
                                        const std::vector<std::string> &modules,
                                        const std::function<std::vector<std::string>(const std::string &)> &actionsOf,
                                        const std::function<std::vector<std::string>(const std::string &, const std::string &)> &optionsOf,
                                        const std::vector<std::string> &globalOptions) {

        if (request.words.empty()) return Matching(modules, "");

        const auto &current = request.words.back();
        const std::vector<std::string> before(request.words.begin(), request.words.end() - 1);

        // The module and the action are the first two words that are not options or option values.
        // Skipping a value this way is crude - it treats the word after any option as a value -
        // but the global options that take one all do, and the ones that do not are flags nobody
        // puts before the module.
        std::vector<std::string> positional;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (StartsWith(before[i], "-")) {
                if (before[i].find('=') == std::string::npos && i + 1 < before.size()) ++i;
                continue;
            }
            positional.push_back(before[i]);
        }

        if (positional.empty()) {
            // Before the module: either a global option or the module itself.
            if (StartsWith(current, "-")) return Matching(globalOptions, current);
            return Matching(modules, current);
        }

        if (positional.size() == 1) {
            auto actions = actionsOf(positional[0]);

            // A word that is not a module gets nothing, not even "help": there is no such thing as
            // "euclid-cli nonsense help", and offering it would say there is.
            if (actions.empty()) return {};

            // Every module answers "help", and it is in no module's table because it is the
            // framework's action rather than one of that module's own.
            actions.emplace_back("help");
            return Matching(actions, current);
        }

        // Past the action, the only thing that can be completed without asking a server is an
        // option name. A value is left alone rather than guessed at - the shell falls back to
        // filenames, which is right for the options that take one.
        if (StartsWith(current, "-")) return Matching(optionsOf(positional[0], positional[1]), current);

        return {};
    }

}// namespace Euclid::CLI::Completion
