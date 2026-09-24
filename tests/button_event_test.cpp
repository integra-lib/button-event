#include <gtest/gtest.h>

#include <cstdint>
#include <hwlib/events/button_event.hpp>
#include <string>
#include <vector>

namespace
{

using hwlib::events::ButtonAction;
using hwlib::events::ButtonEventConfig;
using hwlib::events::ButtonEventCore;

constexpr ButtonEventConfig CONFIG{.confirmMs = 20, .minPressMs = 200, .longPressMs = 1000};

// Records what the core reported, in order, so a test can state both what was
// reported and what was not.
class Recorder
{
public:
    void Attach(ButtonEventCore& core, bool withLongPress = true)
    {
        core.SetOnShortPress([this] { m_events.emplace_back("short"); });
        if (withLongPress)
        {
            core.SetOnLongPress([this] { m_events.emplace_back("long"); });
        }
        core.SetOnChanged([this](bool pressed) { m_events.emplace_back(pressed ? "down" : "up"); });
    }

    [[nodiscard]] const std::vector<std::string>& Events() const
    {
        return m_events;
    }

private:
    std::vector<std::string> m_events;
};

TEST(ButtonEventTest, AnEdgeOnlySchedulesTheSample)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnEdge(), ButtonAction::eScheduleConfirm);
    // The level is not read on the edge, so nothing can be reported yet.
    EXPECT_TRUE(recorder.Events().empty());
}

TEST(ButtonEventTest, ReportsAShortPress)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
}

TEST(ButtonEventTest, IgnoresAPressShorterThanMinPress)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnConfirmed(false, CONFIG.minPressMs - 1), ButtonAction::eStopLongPress);

    // The level changed and consumers of the raw signal hear about it; the press
    // itself is not reported.
    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up"}));
}

TEST(ButtonEventTest, ReportsAPressExactlyAtMinPress)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnConfirmed(false, CONFIG.minPressMs), ButtonAction::eStopLongPress);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
}

TEST(ButtonEventTest, ReportsALongPressWhileTheButtonIsStillDown)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "long"}));
}

TEST(ButtonEventTest, DoesNotAlsoReportAShortPressAfterALongOne)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);
    EXPECT_EQ(core.OnConfirmed(false, 1500), ButtonAction::eStopLongPress);

    // One press, one decision.
    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "long", "up"}));
}

TEST(ButtonEventTest, ReportsALongPressOnlyOncePerPress)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "long"}));
}

TEST(ButtonEventTest, IgnoresALongPressTimeoutThatLostTheRaceToTheRelease)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);
    // The timeout was already in flight when the button came up.
    EXPECT_EQ(core.OnLongPressElapsed(false), ButtonAction::eNone);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
}

TEST(ButtonEventTest, IgnoresALongPressTimeoutForAPinThatIsAlreadyBackUp)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    // The button came up but its release has not been confirmed yet, so the press is
    // still open as far as the state machine is concerned. The pin is what settles it:
    // reporting a long press for a button nobody is holding is the bug this prevents.
    EXPECT_EQ(core.OnLongPressElapsed(false), ButtonAction::eNone);
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
}

TEST(ButtonEventTest, IgnoresALongPressTimeoutForAPinThatIsStillDownAfterARelease)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);
    // Pin reads pressed again — a new press whose confirm has not run yet. The
    // timeout belongs to the press that already ended and must not fire.
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
}

TEST(ButtonEventTest, ArmsNoLongPressTimerWithoutALongPressCallback)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core, false);

    // Nothing to arm a timer for, so the caller is told not to.
    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eNone);
    EXPECT_EQ(core.OnConfirmed(false, 5000), ButtonAction::eStopLongPress);

    // Held far past longPressMs and still resolved as a short press.
    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
}

TEST(ButtonEventTest, TreatsASampleAtTheSameLevelAsNoChange)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    // A bounce that settled back where it was: confirmed, but not a change.
    EXPECT_EQ(core.OnConfirmed(true, 50), ButtonAction::eNone);
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "up", "short"}));
    // The press is timed from the first confirm, not from the repeat.
}

TEST(ButtonEventTest, IgnoresAReleaseWhenNoPressWasSeen)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(false, 0), ButtonAction::eNone);
    EXPECT_TRUE(recorder.Events().empty());
}

TEST(ButtonEventTest, StartsTheNextPressFromACleanState)
{
    ButtonEventCore core{CONFIG};
    Recorder recorder;
    recorder.Attach(core);

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);
    EXPECT_EQ(core.OnConfirmed(false, 1500), ButtonAction::eStopLongPress);

    EXPECT_EQ(core.OnConfirmed(true, 2000), ButtonAction::eStartLongPress);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);

    EXPECT_EQ(recorder.Events(), (std::vector<std::string>{"down", "long", "up", "down", "long"}));
}

TEST(ButtonEventTest, TellsWhetherTheButtonIsDown)
{
    ButtonEventCore core{CONFIG};

    EXPECT_FALSE(core.IsPressed());
    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eNone);
    EXPECT_TRUE(core.IsPressed());
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);
    EXPECT_FALSE(core.IsPressed());
}

TEST(ButtonEventTest, WorksWithNoCallbacksAtAll)
{
    ButtonEventCore core{CONFIG};

    EXPECT_EQ(core.OnConfirmed(true, 0), ButtonAction::eNone);
    EXPECT_EQ(core.OnLongPressElapsed(true), ButtonAction::eNone);
    EXPECT_EQ(core.OnConfirmed(false, 300), ButtonAction::eStopLongPress);
}

} // namespace
