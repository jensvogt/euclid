// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE CommandLineQuotingTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/SystemUtils.h>

using Euclid::Core::SystemUtils;

// CreateProcess takes a flat command-line string where execvp() takes an array, so an argument
// containing a space or a quote has to be escaped such that the child's own argv parser recovers
// exactly the original bytes.
//
// Getting it wrong does not fail. It silently splits one argument into two, or swallows a quote, and
// the child receives something almost right - a jar path that stops at the first space, an argument
// whose trailing backslash ate the quote after it. The rule is CommandLineToArgvW's and the part
// nobody remembers is that backslashes immediately before a quote, or before the closing quote, are
// doubled.
//
// Two things spawn processes on Windows now - euclid-mgr through Manager::Platform, and
// euclid-wrk for the applications placed on its host - so this is one implementation with two
// callers rather than two implementations.
//
// The tests run everywhere, including the POSIX machines where nothing calls it: the rule is pure
// string manipulation, and a function that could only be exercised inside an #ifdef would be
// untested on the platform most of this is written on.

BOOST_AUTO_TEST_CASE(SomethingPlainIsLeftAlone) {

    // Quoting what needs no quoting is not wrong, but it is noise in every log line and every
    // process listing that shows a command.
    BOOST_TEST(SystemUtils::QuoteCommandLineArg("java") == "java");
    BOOST_TEST(SystemUtils::QuoteCommandLineArg("-jar") == "-jar");
    BOOST_TEST(SystemUtils::QuoteCommandLineArg("/usr/local/euclid/bin/app") == "/usr/local/euclid/bin/app");
}

BOOST_AUTO_TEST_CASE(ASpaceIsQuoted) {

    // The common case on Windows, and the one that silently becomes two arguments: every default
    // install path has a space in it.
    BOOST_TEST(SystemUtils::QuoteCommandLineArg(R"(C:\Program Files\euclid\bin\app.jar)")
               == R"("C:\Program Files\euclid\bin\app.jar")");
}

BOOST_AUTO_TEST_CASE(ATabOrNewlineIsQuotedToo) {

    BOOST_TEST(SystemUtils::QuoteCommandLineArg("a\tb") == "\"a\tb\"");
    BOOST_TEST(SystemUtils::QuoteCommandLineArg("a\nb") == "\"a\nb\"");
}

BOOST_AUTO_TEST_CASE(AQuoteIsEscaped) {

    // Through locals rather than inline: BOOST_TEST stringizes its argument, and MSVC's
    // preprocessor mis-handles a raw string containing a backslash-quote when it does - the
    // expression here is fine, the diagnostic comes from stringizing it.
    const std::string quoted = SystemUtils::QuoteCommandLineArg(R"(say "hi")");
    const std::string expected = R"("say \"hi\"")";
    BOOST_TEST(quoted == expected);
}

BOOST_AUTO_TEST_CASE(AnEmptyArgumentBecomesAnEmptyPairOfQuotes) {

    // Not nothing: an empty argument has to survive as an argument, or every one after it shifts
    // down a position and the child reads them all off by one.
    BOOST_TEST(SystemUtils::QuoteCommandLineArg("") == "\"\"");
}

// ── The part nobody remembers ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ABackslashNotTouchingAQuoteIsLeftAsIs) {

    // A Windows path with no space needs no quoting at all, and its backslashes are not doubled -
    // doubling them unconditionally is the other way to get this wrong.
    BOOST_TEST(SystemUtils::QuoteCommandLineArg(R"(C:\euclid\bin)") == R"(C:\euclid\bin)");
}

BOOST_AUTO_TEST_CASE(BackslashesBeforeAQuoteAreDoubled) {

    // Two backslashes then a quote: the backslashes become four, so the child reads two
    // backslashes and a literal quote rather than one backslash and an escaped quote.
    const std::string quoted = SystemUtils::QuoteCommandLineArg(R"(a\\"b)");
    const std::string expected = R"("a\\\\\"b")";
    BOOST_TEST(quoted == expected);
}

BOOST_AUTO_TEST_CASE(BackslashesBeforeTheClosingQuoteAreDoubled) {

    // The case that bites on Windows paths: an argument ending in a separator, inside quotes. The
    // trailing backslash would otherwise escape the closing quote and swallow it, running this
    // argument into the next one.
    const std::string quoted = SystemUtils::QuoteCommandLineArg(R"(C:\Program Files\euclid\)");
    const std::string expected = R"("C:\Program Files\euclid\\")";
    BOOST_TEST(quoted == expected);
}

BOOST_AUTO_TEST_CASE(ATrailingBackslashInAnUnquotedArgumentIsNotDoubled) {

    // No space, so no quotes, so no closing quote for the backslash to escape - and doubling it
    // here would change the path.
    BOOST_TEST(SystemUtils::QuoteCommandLineArg(R"(C:\euclid\)") == R"(C:\euclid\)");
}

// ── The property all of the above are examples of ───────────────────────────

namespace {

    // CommandLineToArgvW's rule, as the reverse direction - enough of it to parse one quoted
    // element back out. Written here rather than called, because the Windows function is not
    // available on the machines these tests mostly run on, and what is worth checking is that the
    // two rules are inverses.
    std::string unquote(const std::string &quoted) {

        std::string out;
        bool inQuotes = false;

        for (std::size_t i = 0; i < quoted.size();) {

            if (quoted[i] == '\\') {
                std::size_t backslashes = 0;
                while (i < quoted.size() && quoted[i] == '\\') {
                    ++backslashes;
                    ++i;
                }
                if (i < quoted.size() && quoted[i] == '"') {
                    // 2n backslashes then a quote: n backslashes, and the quote toggles. 2n+1
                    // backslashes then a quote: n backslashes and a literal quote.
                    out.append(backslashes / 2, '\\');
                    if (backslashes % 2 == 0) {
                        inQuotes = !inQuotes;
                    } else {
                        out.push_back('"');
                    }
                    ++i;
                } else {
                    out.append(backslashes, '\\');
                }
                continue;
            }

            if (quoted[i] == '"') {
                inQuotes = !inQuotes;
                ++i;
                continue;
            }
            out.push_back(quoted[i]);
            ++i;
        }
        return out;
    }

}// namespace

BOOST_AUTO_TEST_CASE(QuotingRoundTripsForEveryShapeThatMatters) {

    // The property, over the arguments a worker actually passes: an artifact path, a JVM flag, a
    // path with a space, one ending in a separator, one with a quote in it. If quoting and parsing
    // are not inverses then an application is started with arguments nobody wrote.
    const std::string cases[] = {
            "java",
            "-jar",
            R"(C:\euclid\bin\app.jar)",
            R"(C:\Program Files\euclid\bin\app.jar)",
            R"(C:\Program Files\euclid\)",
            R"(--name=a b c)",
            R"(say "hi")",
            R"(trailing\\)",
            R"(odd\"quote)",
            "",
            "-Dspring.profiles.active=production",
    };

    for (const auto &original: cases) {
        const auto quoted = SystemUtils::QuoteCommandLineArg(original);
        BOOST_TEST(unquote(quoted) == original,
                   "round trip failed for [" + original + "] quoted as [" + quoted + "]");
    }
}
