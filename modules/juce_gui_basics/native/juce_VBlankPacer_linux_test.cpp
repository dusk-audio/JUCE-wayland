/*
 // Distributed under the GPLv3 license
 */

namespace juce
{

class LinuxVBlankPacerTest final : public UnitTest
{
public:
    LinuxVBlankPacerTest() : UnitTest ("LinuxVBlankPacer", UnitTestCategories::gui) {}

    void runTest() override
    {
        const ScopedJuceInitialiser_GUI juceInit;

        beginTest ("An idle peer runs no timer");
        {
            Peer peer;
            peer.pacer.setFrequencyHz (100);
            expect (! isTicking (peer.pacer));
            expect (! isDispatchPending (peer.pacer));
        }

        beginTest ("A repaint at rest is dispatched without the timer");
        {
            Peer peer;
            peer.pacer.repaintRequested();
            expect (isDispatchPending (peer.pacer));
            expect (! isTicking (peer.pacer));

            runPendingDispatch (peer.pacer);
            expectEquals (peer.dispatches, 1);
            expect (! isTicking (peer.pacer));
            expect (! isDispatchPending (peer.pacer));
        }

        beginTest ("A second repaint inside one frame interval waits for the timer");
        {
            Peer peer;
            peer.pacer.repaintRequested();
            runPendingDispatch (peer.pacer);

            peer.nowMs += 4;
            peer.pacer.repaintRequested();
            expect (! isDispatchPending (peer.pacer));
            expect (isTicking (peer.pacer));
            expectEquals (timerInterval (peer.pacer), 10);

            peer.nowMs += 10;
            tick (peer.pacer);
            expectEquals (peer.dispatches, 2);
            expect (! isTicking (peer.pacer));
        }

        beginTest ("A repaint a frame interval after the last dispatch goes straight out");
        {
            Peer peer;
            peer.pacer.repaintRequested();
            runPendingDispatch (peer.pacer);

            peer.nowMs += 10;
            peer.pacer.repaintRequested();
            expect (isDispatchPending (peer.pacer));
            expect (! isTicking (peer.pacer));
        }

        beginTest ("Regions left after a dispatch keep the timer running until a tick paints them");
        {
            Peer peer;
            peer.workLeft = true;
            peer.pacer.repaintRequested();
            runPendingDispatch (peer.pacer);
            expect (isTicking (peer.pacer));

            tick (peer.pacer);
            expect (isTicking (peer.pacer));

            peer.workLeft = false;
            tick (peer.pacer);
            expect (! isTicking (peer.pacer));
            expectEquals (peer.dispatches, 3);
        }

        beginTest ("A listener keeps the timer running until it is removed");
        {
            Peer peer;
            peer.workLeft = true;
            peer.pacer.listenerAdded();
            expect (isTicking (peer.pacer));
            expectEquals (peer.dispatches, 0);

            tick (peer.pacer);
            tick (peer.pacer);
            expect (isTicking (peer.pacer));

            peer.workLeft = false;
            tick (peer.pacer);
            expect (! isTicking (peer.pacer));
        }

        beginTest ("A tick drops a dispatch that is already queued");
        {
            Peer peer;
            peer.pacer.repaintRequested();
            peer.workLeft = true;
            peer.pacer.listenerAdded();
            tick (peer.pacer);
            expect (! isDispatchPending (peer.pacer));
            expectEquals (peer.dispatches, 1);
        }

        beginTest ("A compositor frame dispatches and rests when nothing is left");
        {
            Peer peer;
            peer.pacer.frameArrived();
            expectEquals (peer.dispatches, 1);
            expect (! isTicking (peer.pacer));

            peer.workLeft = true;
            peer.pacer.frameArrived();
            expectEquals (peer.dispatches, 2);
            expect (isTicking (peer.pacer));
        }

        beginTest ("A new display rate applies to a running timer and to the next start");
        {
            Peer peer;
            peer.pacer.setFrequencyHz (60);
            peer.pacer.listenerAdded();
            expectEquals (timerInterval (peer.pacer), 16);

            peer.pacer.setFrequencyHz (100);
            expectEquals (timerInterval (peer.pacer), 10);
        }
    }

private:
    struct Peer
    {
        uint32 nowMs = 100000;
        int dispatches = 0;
        bool workLeft = false;
        LinuxVBlankPacer pacer { [this] { ++dispatches; },
                                 [this] { return workLeft; },
                                 [this] { return nowMs; } };
    };

    static bool isTicking (const LinuxVBlankPacer& p)         { return p.timer.isTimerRunning(); }
    static int timerInterval (const LinuxVBlankPacer& p)      { return p.timer.getTimerInterval(); }
    static bool isDispatchPending (const LinuxVBlankPacer& p) { return p.isUpdatePending(); }
    static void runPendingDispatch (LinuxVBlankPacer& p)      { p.handleUpdateNowIfNeeded(); }
    static void tick (LinuxVBlankPacer& p)                    { p.tick(); }
};

static LinuxVBlankPacerTest linuxVBlankPacerTest;

} // namespace juce
