/*
 // Distributed under the GPLv3 license
 */

namespace juce
{

//==============================================================================
/*  Paces the vblank work of a Linux peer.

    The vblank timer runs only while the peer has something to do at the display rate:
    VBlankListeners to call, or repaint regions that the last dispatch could not paint yet.
    An idle window costs no wake-ups. A repaint that arrives while the timer rests is
    dispatched on the message thread straight away when the last dispatch is at least one
    frame interval old, and otherwise waits for the timer, so paints never come faster than
    the display rate.
*/
class LinuxVBlankPacer final : private AsyncUpdater
{
public:
    LinuxVBlankPacer (std::function<void()> dispatchIn,
                      std::function<bool()> needsTicksIn,
                      std::function<uint32()> clockIn = [] { return Time::getMillisecondCounter(); })
        : dispatch (std::move (dispatchIn)),
          needsTicks (std::move (needsTicksIn)),
          clock (std::move (clockIn))
    {
    }

    void setFrequencyHz (int frequencyHz)
    {
        const auto newIntervalMs = jmax (1, 1000 / jmax (1, frequencyHz));

        if (newIntervalMs == intervalMs)
            return;

        intervalMs = newIntervalMs;

        if (timer.isTimerRunning())
            timer.startTimer (intervalMs);
    }

    void repaintRequested()
    {
        if (timer.isTimerRunning() || isUpdatePending())
            return;

        if (clock() - lastDispatchMs >= (uint32) intervalMs)
            triggerAsyncUpdate();
        else
            timer.startTimer (intervalMs);
    }

    void listenerAdded()
    {
        if (! timer.isTimerRunning())
            timer.startTimer (intervalMs);
    }

    /*  The compositor is ready for a new frame: dispatch now, and keep any further ticks in
        phase with its frames.
    */
    void frameArrived()
    {
        tick();

        if (timer.isTimerRunning())
            timer.startTimer (intervalMs);
    }

private:
    void handleAsyncUpdate() override  { tick(); }

    void tick()
    {
        cancelPendingUpdate();
        lastDispatchMs = clock();
        dispatch();

        if (! needsTicks())
            timer.stopTimer();
        else if (! timer.isTimerRunning())
            timer.startTimer (intervalMs);
    }

    std::function<void()> dispatch;
    std::function<bool()> needsTicks;
    std::function<uint32()> clock;
    TimedCallback timer { [this] { tick(); } };
    uint32 lastDispatchMs = 0;
    int intervalMs = 10;

    friend class LinuxVBlankPacerTest;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinuxVBlankPacer)
};

} // namespace juce
