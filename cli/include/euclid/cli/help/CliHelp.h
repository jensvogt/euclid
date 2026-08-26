#pragma once

// C++ includes
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/program_options.hpp>

namespace Euclid::CLI {

    /**
     * @brief Checks whether the action arguments are asking for help rather than
     * providing real option values, e.g. "euclid-cli access login help".
     *
     * @param args action arguments
     * @return true if the first argument is "help", "--help" or "-h"
     */
    [[nodiscard]]
    inline bool IsHelpRequest(const std::vector<std::string> &args) {
        return !args.empty() && (args.front() == "help" || args.front() == "--help" || args.front() == "-h");
    }

    /**
     * @brief Word-wraps text to a maximum line width, indenting every line with the given prefix.
     *
     * @param text text to wrap
     * @param indent string prepended to every output line
     * @param maxWidth maximum total line width, including the indent
     * @return the wrapped, indented text
     */
    [[nodiscard]]
    inline std::string WrapText(const std::string &text, const std::string &indent, const std::size_t maxWidth = 160) {
        const std::size_t width = maxWidth > indent.size() ? maxWidth - indent.size() : maxWidth;
        std::istringstream words(text);
        std::string word, line, result;
        while (words >> word) {
            if (!line.empty() && line.size() + 1 + word.size() > width) {
                result += indent + line + "\n";
                line.clear();
            }
            if (!line.empty()) {
                line += ' ';
            }
            line += word;
        }
        if (!line.empty()) {
            result += indent + line + "\n";
        }
        return result;
    }

    /**
     * @brief Prints a man-page style description of a single action and its options.
     *
     * @param module module name, e.g. "access"
     * @param action action name, e.g. "login"
     * @param synopsis one-line summary of the action's options, e.g. "--user <user> --password <password>"
     * @param description longer prose description of what the action does
     * @param options the action's option descriptions
     * @return 0, so callers can `return PrintActionHelp(...);`
     */
    inline int PrintActionHelp(const std::string &module, const std::string &action, const std::string &synopsis, const std::string &description, const boost::program_options::options_description &options) {
        std::cout << "NAME\n"
                << "    " << module << " " << action << "\n\n"
                << "SYNOPSIS\n"
                << WrapText("euclid-cli " + module + " " + action + " " + synopsis, "    ") << "\n"
                << "DESCRIPTION\n"
                << WrapText(description, "    ") << "\n"
                << options << std::endl;
        return 0;
    }

    /**
     * @brief Prints the list of actions available for a module, e.g. "euclid-cli access help".
     *
     * @param module module name, e.g. "access"
     * @param actions list of (action name, one-line description) pairs
     * @return 0, so callers can `return PrintModuleHelp(...);`
     */
    inline int PrintModuleHelp(const std::string &module, const std::vector<std::pair<std::string, std::string> > &actions) {
        std::cout << "Usage: euclid-cli " << module << " <action> [options...]\n"
                << "       euclid-cli " << module << " <action> help   (show help for a single action)\n\n"
                << "Available " << module << " actions:\n";
        for (const auto &[name, summary]: actions) {
            std::cout << "    " << name << "\n        " << summary << "\n";
        }
        std::cout << std::endl;
        return 0;
    }

}// namespace Euclid::CLI