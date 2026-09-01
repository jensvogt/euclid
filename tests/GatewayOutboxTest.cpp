#define BOOST_TEST_MODULE GatewayOutboxTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/manager/Outbox.h>

using Euclid::main::Outbox;

// What a websocket connection does when its client stops reading. The queue has to be bounded -
// one stalled client must not grow the gateway until it dies - and the whole design is in which
// frames are given up when it fills: events, which a durable subscriber can still claim from the
// store, and never responses, which somebody is blocked waiting for.

namespace {

    std::string frame(const std::size_t bytes = 8) { return std::string(bytes, 'x'); }

}// namespace

BOOST_AUTO_TEST_CASE(BelowTheBoundNothingIsDropped) {
    Outbox outbox(4, 1024);

    BOOST_TEST((outbox.Push(frame(), true) == Outbox::Result::Idle));
    outbox.BeginWrite();
    BOOST_TEST((outbox.Push(frame(), true) == Outbox::Result::Writing));

    BOOST_TEST(outbox.Size() == 2U);
    BOOST_TEST(outbox.TakeDropped() == 0);
}

BOOST_AUTO_TEST_CASE(EventsAreDroppedOldestFirst) {
    Outbox outbox(3, 1024);

    for (int i = 0; i < 5; ++i) outbox.Push(frame(), true);

    // Three kept, two given up - and the ones kept are the newest, because an event a client has
    // not seen yet is worth more than one it has already fallen behind on.
    BOOST_TEST(outbox.Size() == 3U);
    BOOST_TEST(outbox.TakeDropped() == 2);
    // Taking the count resets it: it is reported to the client once, not on every drain.
    BOOST_TEST(outbox.TakeDropped() == 0);
}

BOOST_AUTO_TEST_CASE(ResponsesAreNeverDropped) {
    Outbox outbox(2, 1024);

    outbox.Push(frame(), false);
    outbox.Push(frame(), false);
    outbox.Push(frame(), true);

    // The event goes rather than either response: a dropped response would hang the call that is
    // waiting for it, while a dropped event costs a notification of something still in the store.
    BOOST_TEST(outbox.Size() == 2U);
    BOOST_TEST(outbox.TakeDropped() == 1);
}

BOOST_AUTO_TEST_CASE(AResponseBacklogWithNothingToDropOverflows) {
    Outbox outbox(2, 1024);

    outbox.Push(frame(), false);
    outbox.Push(frame(), false);

    // A client that is not reading its responses is not slow, it is gone - and holding frames for
    // it forever is what the bound exists to prevent, so the caller is told to close it.
    BOOST_TEST((outbox.Push(frame(), false) == Outbox::Result::Overflow));
}

BOOST_AUTO_TEST_CASE(TheFrameBeingWrittenIsNeverDropped) {
    Outbox outbox(2, 1024);

    outbox.Push(std::string("in-flight"), true);
    outbox.BeginWrite();
    for (int i = 0; i < 4; ++i) outbox.Push(frame(), true);

    // async_write holds a buffer into the front frame for the duration of the write, so dropping
    // it would hand the socket a dangling pointer. Everything behind it is fair game.
    BOOST_TEST(outbox.Front() == "in-flight");
    BOOST_TEST(outbox.Size() == 2U);
}

BOOST_AUTO_TEST_CASE(TheByteBoundAlsoDrops) {
    Outbox outbox(100, 32);

    for (int i = 0; i < 6; ++i) outbox.Push(frame(10), true);

    // Frame count is not the only way a slow client costs memory: a few large events do it too,
    // which is why the bound is on both.
    BOOST_TEST(outbox.QueuedBytes() <= 32U);
    BOOST_TEST(outbox.TakeDropped() > 0);
}

BOOST_AUTO_TEST_CASE(WritingDrainsTheQueue) {
    Outbox outbox(4, 1024);

    outbox.Push(std::string("first"), false);
    outbox.Push(std::string("second"), true);

    outbox.BeginWrite();
    BOOST_TEST(outbox.Front() == "first");
    outbox.CompleteWrite();

    BOOST_TEST(outbox.Front() == "second");
    outbox.BeginWrite();
    outbox.CompleteWrite();

    BOOST_TEST(outbox.Empty());
    BOOST_TEST(outbox.QueuedBytes() == 0U);
}
