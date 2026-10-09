// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE CronReleaseTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <stdexcept>
#include <string>

// Euclid includes
#include <euclid/core/CronExpression.h>

using Euclid::Core::CronExpression;
using Euclid::Core::CronRelease;
using Euclid::Core::CronReleaseFor;
using std::chrono::system_clock;

// A scheduled job is released by the manager comparing one stored moment with the clock, and
// everything about that comparison that is not cron arithmetic is CronReleaseFor(). The cases below
// are the whole of it, because each one is a way the scheduler has to get it wrong: a job that
// never runs, a job that runs when the manager starts, or a job that runs itself twice over.

BOOST_AUTO_TEST_CASE(AMomentInTheFutureIsNotDue) {

    const auto now = system_clock::now();
    BOOST_TEST((CronReleaseFor(now + std::chrono::minutes(1), now, false) == CronRelease::NotDue));

    // And a job already running is still not due, which is the ordinary state of a long run: the
    // skip is only reached once the moment has actually passed.
    BOOST_TEST((CronReleaseFor(now + std::chrono::hours(9), now, true) == CronRelease::NotDue));
}

BOOST_AUTO_TEST_CASE(AMomentThatHasPassedFires) {

    const auto now = system_clock::now();
    BOOST_TEST((CronReleaseFor(now - std::chrono::seconds(1), now, false) == CronRelease::Fire));

    // Exactly now counts as passed. The occurrence is truncated to whole minutes and the manager
    // reconciles every few seconds, so the boundary is reachable - and treating it as not-yet would
    // only move the firing to the next tick, whereas treating it as passed twice would be a double
    // run. It cannot be: firing advances the moment.
    BOOST_TEST((CronReleaseFor(now, now, false) == CronRelease::Fire));
}

BOOST_AUTO_TEST_CASE(AMomentThatHasPassedWhileTheLastRunIsStillGoingIsSkipped) {

    const auto now = system_clock::now();
    BOOST_TEST((CronReleaseFor(now - std::chrono::minutes(30), now, true) == CronRelease::Skip));
}

BOOST_AUTO_TEST_CASE(NoStoredMomentIsUndatedRatherThanOverdue) {

    // The epoch is how "no moment" is stored, and it is fifty-six years in the past. Read as an
    // occurrence it would make every scheduled job in the installation overdue, and a manager
    // coming up at breakfast would start the nightly ones - every time it came up. This is the
    // single most expensive thing this function exists to prevent.
    const auto now = system_clock::now();
    BOOST_TEST((CronReleaseFor(system_clock::time_point{}, now, false) == CronRelease::Undated));

    // Including when something of it is already running, because there is still no moment to judge
    // and dating it is what has to happen first either way.
    BOOST_TEST((CronReleaseFor(system_clock::time_point{}, now, true) == CronRelease::Undated));
}

BOOST_AUTO_TEST_CASE(TheNextOccurrenceIsAlwaysInTheFuture) {

    // What makes storing an occurrence at the moment a schedule is set safe: it is never the
    // instant it was computed from, so a job scheduled at noon is next due tonight and not at once.
    const CronExpression nightly("0 2 * * *");
    const auto now = system_clock::now();

    BOOST_TEST((nightly.Next(now) > now));
    BOOST_TEST((CronReleaseFor(nightly.Next(now), now, false) == CronRelease::NotDue));

    // And advancing from an occurrence that has just been consumed moves past it rather than
    // returning it again, which is what stops a release from repeating on the next tick.
    const auto consumed = nightly.Next(now);
    BOOST_TEST((nightly.Next(consumed) > consumed));
}

BOOST_AUTO_TEST_CASE(AnExpressionThatCannotBeParsedThrows) {

    // Relied on by EAP, which refuses a schedule at the moment somebody types it rather than
    // letting the manager find out hours later that the job it was holding will never fire.
    BOOST_CHECK_THROW(CronExpression("not a schedule"), std::invalid_argument);
    BOOST_CHECK_THROW(CronExpression("0 2 * *"), std::invalid_argument);
    BOOST_CHECK_THROW(CronExpression("61 2 * * *"), std::invalid_argument);
    BOOST_CHECK_THROW(CronExpression("@fortnightly"), std::invalid_argument);

    // The forms the documentation promises, and the shorthand alongside the five fields.
    BOOST_CHECK_NO_THROW(CronExpression("0 2 * * *"));
    BOOST_CHECK_NO_THROW(CronExpression("@daily"));
    BOOST_CHECK_NO_THROW(CronExpression("*/15 * * * MON-FRI"));
}

BOOST_AUTO_TEST_CASE(TheMonthAndDayNamesMeanWhatTheySay) {

    // Documented by the header and reachable from the CLI now that applications carry a schedule,
    // so "MON-FRI" has to be the weekdays and not a parse error. Checked by equivalence rather than
    // by occurrence, because the numeric form is already covered above.
    const auto now = system_clock::now();

    BOOST_TEST((CronExpression("0 6 * * MON-FRI").Next(now) == CronExpression("0 6 * * 1-5").Next(now)));
    BOOST_TEST((CronExpression("0 6 * * sat,sun").Next(now) == CronExpression("0 6 * * 6,0").Next(now)));
    BOOST_TEST((CronExpression("0 0 1 JAN,JUL *").Next(now) == CronExpression("0 0 1 1,7 *").Next(now)));

    // A word that is not a name is refused rather than being read as some number.
    BOOST_CHECK_THROW(CronExpression("0 6 * * MONDAY"), std::invalid_argument);
    BOOST_CHECK_THROW(CronExpression("0 6 * SPRING *"), std::invalid_argument);
}
