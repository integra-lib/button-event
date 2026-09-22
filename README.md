# button-event

Debounce one button and classify the press: short, long, or too brief to mean anything.

Part of [integra-lib](https://github.com/integra-lib) — architecture-independent C++20
components shared between firmware projects. Header-only,
no exceptions, no RTTI.

## Use it

```bash
git submodule add git@github.com:integra-lib/button-event.git external/integra/button-event
```

```cmake
add_subdirectory(external/integra/button-event)
target_link_libraries(app PRIVATE Integra::button_event)
```

```cpp
#include <integra/button_event.hpp>
```

Each component carries its own include directory, so this header stays unreachable
until the component is linked: a forgotten dependency is a compile error rather than
a build that happens to work.

## What it does and does not do

`integra::ButtonEventCore` is the state machine only. It owns no pin, no timer and no
clock — it is fed three things and answers what the caller's timers should do next:

```cpp
[[nodiscard]] ButtonAction OnEdge();                                 // a pin edge arrived
[[nodiscard]] ButtonAction OnConfirmed(bool pressed, std::int64_t nowMs);
[[nodiscard]] ButtonAction OnLongPressElapsed(bool pressed);
```

`ButtonAction` is `eNone`, `eScheduleConfirm`, `eStartLongPress` or `eStopLongPress`.
Acting on it is the caller's half of the contract, which is why it is `[[nodiscard]]`.

```cpp
integra::ButtonEventCore button{{.confirmMs = 20, .minPressMs = 200, .longPressMs = 1000}};
button.SetOnShortPress([] { ToggleView(); });
button.SetOnLongPress([] { EnterService(); });
button.SetOnChanged([](bool pressed) { NotifyActivity(pressed); });
```

The three timings are what the press means: an edge is sampled once the line has been
quiet for `confirmMs`, a confirmed press shorter than `minPressMs` is a brush against
the button and is not reported at all, and a press still down after `longPressMs` is a
long one. A press reported as long is not also reported as short on release — one
press, one decision.

Without a long-press callback the core answers `eNone` instead of `eStartLongPress`,
so no timer is armed and no wakeup is spent on a press nobody classifies.

## One context, and why it matters

All three entry points must run on the same context, and the callbacks run on
whichever one called in.

On a device both the pin edge and the long-press timeout arrive in interrupt context,
so an adapter defers them — a work queue — instead of calling in directly. That is
what lets the state be plain `bool`s here rather than atomics, and it is also what
makes the callbacks safe to do real work in.

A Zephyr adapter is the other half of what used to be one class:

```cpp
class ZephyrButton
{
public:
    ZephyrButton(IGpioInputPin& pin, integra::ButtonEventConfig config)
        : m_pin{pin}, m_core{config}
    {
        k_work_init_delayable(&m_confirm.work, ConfirmHandler);
        k_work_init(&m_longPress.work, LongPressHandler);
        m_confirm.self = this;
        m_longPress.self = this;
        // ISR context: nothing but a reschedule.
        m_pin.SetInterruptCallback([this] { Apply(m_core.OnEdge()); });
    }

private:
    void Apply(integra::ButtonAction action)
    {
        switch (action)
        {
        case integra::ButtonAction::eScheduleConfirm:
            k_work_reschedule(&m_confirm.work, K_MSEC(m_config.confirmMs));
            break;
        case integra::ButtonAction::eStartLongPress:
            m_longPressTimer.Start(K_MSEC(m_config.longPressMs));
            break;
        case integra::ButtonAction::eStopLongPress: m_longPressTimer.Stop(); break;
        case integra::ButtonAction::eNone: break;
        }
    }
    // The confirm work runs on the system work queue and samples the pin there;
    // the long-press timer expires in ISR context and submits m_longPress, so
    // OnLongPressElapsed() reaches the core on the same work queue.
};
```

## Versioning

Every component is released on its own, tagged `vX.Y.Z`. Pre-1.0, a minor release may
break the API, which is why dependants accept a single minor.

```bash
git -C external/integra/button-event fetch --tags
git -C external/integra/button-event checkout v0.2.0
git add external/integra/button-event && git commit -m "build: bump button-event to v0.2.0"
```

## Coming from a174-hardware's button-event

The behaviour is unchanged, including the two checks that are easy to lose: a
long-press timeout that arrives after the release does nothing, and it does nothing
whether the state machine has already seen the release or only the pin has come back
up. What moved out of the class:

* `IGpioInputPin` — the adapter samples the pin and passes the level in, so the
  library needs no GPIO interface at all;
* `k_work`, `k_work_delayable`, `CONTAINER_OF` and `util::ZephyrTimer` — replaced by
  the returned `ButtonAction`;
* `k_uptime_get()` — replaced by the `nowMs` parameter;
* the two `std::atomic` flags — the context contract above replaces them. Keeping
  them would have suggested the core is safe to call from two contexts, which it is
  not, and never was: the original's `m_pressTime` was a plain `int64_t` all along.

## In a consumer's CI

The component is an ordinary submodule, so the build needs it checked out. On GitLab
that means `GIT_SUBMODULE_STRATEGY: normal` (or `recursive`) on every job that builds —
not only on the ones that run unit tests.

## Develop it

```bash
git submodule update --init          # ci-shared, needed by pre-commit
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

Tests are built only when this repository is the top-level project, so a consumer
never builds them and never fetches GoogleTest.

The style configs are symlinks into the `ci-shared` submodule, and the pipeline comes
from the same place. On GitHub this repository carries a self-contained build-and-test
workflow instead: a workflow token cannot read another private repository, so neither
a shared workflow nor the submodule is reachable there. The shared setup is what
GitLab will use.
