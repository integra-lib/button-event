#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace hwlib::events
{

inline constexpr std::int64_t DEFAULT_CONFIRM_MS    = 20;
inline constexpr std::int64_t DEFAULT_MIN_PRESS_MS  = 200;
inline constexpr std::int64_t DEFAULT_LONG_PRESS_MS = 1000;

struct ButtonEventConfig
{
    /// How long the level has to settle before it is sampled.
    std::int64_t confirmMs{DEFAULT_CONFIRM_MS};
    /// A confirmed press shorter than this is not reported at all — it is a brush
    /// against the button, not a decision by whoever pressed it.
    std::int64_t minPressMs{DEFAULT_MIN_PRESS_MS};
    /// How long the button is held before the press counts as a long one.
    std::int64_t longPressMs{DEFAULT_LONG_PRESS_MS};
};

/// What the caller has to do with its timers after the call returned. The core owns
/// no timer of its own — a timer is a platform object, and keeping it out is what
/// lets the whole press-classification run on a host with nothing but a counter.
enum class ButtonAction : std::uint8_t
{
    eNone,
    /// (Re)start the confirm delay, `confirmMs` from now. Restarting an already
    /// pending one is what debounces the pin: the sample happens once the edges stop.
    eScheduleConfirm,
    eStartLongPress,
    eStopLongPress,
};

/// Debounce and press classification for one button, as a state machine driven from
/// the outside.
///
/// The caller feeds it three things — a pin edge, the level sampled once the confirm
/// delay expired, and the long-press timeout — and acts on the ButtonAction each one
/// returns. Nothing here reads a clock, touches a pin or arms a timer, so the whole
/// behaviour is testable on a host with no hardware and no waiting.
///
/// All three entry points must run on the same context. That is a real constraint,
/// not a formality: on a device the pin edge and the long-press timeout both arrive
/// in interrupt context, and an adapter has to defer them (a work queue) rather than
/// call in directly. Deferring them is also what makes the callbacks below safe to
/// do real work in.
class ButtonEventCore
{
public:
    using Callback        = std::function<void()>;
    using ChangedCallback = std::function<void(bool pressed)>;

    explicit ButtonEventCore(ButtonEventConfig config = {})
        : m_config{config}
    {}

    ButtonEventCore(const ButtonEventCore&)            = delete;
    ButtonEventCore& operator=(const ButtonEventCore&) = delete;
    ButtonEventCore(ButtonEventCore&&)                 = delete;
    ButtonEventCore& operator=(ButtonEventCore&&)      = delete;
    ~ButtonEventCore()                                 = default;

    /// Called after a press that lasted at least `minPressMs` and less than
    /// `longPressMs`, on release.
    void SetOnShortPress(Callback onShortPress)
    {
        m_onShortPress = std::move(onShortPress);
    }

    /// Called once `longPressMs` into a press, while the button is still down. Without
    /// it the long-press timer is never armed and every press resolves as a short one —
    /// the same bargain the original struck, kept because arming a timer nobody listens
    /// to costs a wakeup per press.
    void SetOnLongPress(Callback onLongPress)
    {
        m_onLongPress = std::move(onLongPress);
    }

    /// Called on every debounce-confirmed edge, press and release alike, before the
    /// classification above — for consumers that only need a settled level, such as an
    /// activity or wake signal.
    void SetOnChanged(ChangedCallback onChanged)
    {
        m_onChanged = std::move(onChanged);
    }

    /// A pin edge arrived. The level is deliberately not read here: it is read once,
    /// after the line has settled.
    [[nodiscard]] ButtonAction OnEdge() noexcept
    {
        return ButtonAction::eScheduleConfirm;
    }

    /// The confirm delay expired; `pressed` is the level sampled now. An edge that
    /// bounced back to where it started lands here as the level it already had and
    /// changes nothing.
    [[nodiscard]] ButtonAction OnConfirmed(bool pressed, std::int64_t nowMs)
    {
        if (pressed && !m_inPress)
        {
            m_inPress        = true;
            m_pressStartMs   = nowMs;
            m_longPressFired = false;
            if (m_onChanged)
            {
                m_onChanged(true);
            }
            return m_onLongPress ? ButtonAction::eStartLongPress : ButtonAction::eNone;
        }

        if (!pressed && m_inPress)
        {
            m_inPress = false;
            if (m_onChanged)
            {
                m_onChanged(false);
            }
            // A press already reported as long is not reported again as short: the two
            // are one press, and the release is not a second decision.
            if (!m_longPressFired && (nowMs - m_pressStartMs) >= m_config.minPressMs && m_onShortPress)
            {
                m_onShortPress();
            }
            return ButtonAction::eStopLongPress;
        }

        return ButtonAction::eNone;
    }

    /// The long-press timeout expired. `pressed` is the level right now, because the
    /// timeout races the release: a timer that has already been handed to the caller
    /// cannot be unarmed in time, and firing a long press for a button that is no
    /// longer down is exactly the bug that check prevents.
    [[nodiscard]] ButtonAction OnLongPressElapsed(bool pressed)
    {
        if (!pressed || !m_inPress || m_longPressFired)
        {
            return ButtonAction::eNone;
        }
        m_longPressFired = true;
        if (m_onLongPress)
        {
            m_onLongPress();
        }
        return ButtonAction::eNone;
    }

    [[nodiscard]] bool IsPressed() const noexcept
    {
        return m_inPress;
    }

private:
    const ButtonEventConfig m_config;

    bool m_inPress{false};
    bool m_longPressFired{false};
    std::int64_t m_pressStartMs{0};

    Callback m_onShortPress;
    Callback m_onLongPress;
    ChangedCallback m_onChanged;
};

} // namespace hwlib::events
