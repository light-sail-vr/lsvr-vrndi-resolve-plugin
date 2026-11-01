// Keeping Resolve's UI alive while the Linux browse dialog runs (ticket #76).
//
// The browse push-button's instanceChanged arrives on Resolve's main (Qt GUI)
// thread, and the zenity/kdialog helper is a separate process, so waiting
// for it blocked Resolve's event loop: no repaints, and GNOME marked Resolve
// "not responding" (Tier 2 on the box). The macOS and Windows dialogs don't
// have this problem because they run a nested event loop of their own.
//
// The Linux counterpart: while waiting, call the host's
// QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents), which
// repaints and answers the window manager's ping but leaves clicks and keys
// queued until the dialog closes (so the user can't edit the node mid-pick).
// It is looked up by symbol in the already-loaded process (dlsym
// RTLD_DEFAULT), never linked: spec decision 18 rejects linking Resolve's
// private Qt, and a host without the symbol, without a QCoreApplication, or a
// call off the main thread simply gets no pump, i.e. the plain blocking wait.
//
// Pure logic with the lookup injected, so tests/test_linux_file_dialog.cpp
// runs it on any platform.

#ifndef LINUX_HOST_EVENTS_H
#define LINUX_HOST_EVENTS_H

#include <functional>

namespace ndi_linux_host {

// static void QCoreApplication::processEvents(QEventLoop::ProcessEventsFlags)
// and QCoreApplication::self, as exported by Resolve 21.1's libQt5Core 5.15.2
// (nm -D, verified on the box); Qt 6 mangles both the same way. QFlags is one
// int, trivially copyable, so the Itanium ABI passes it in a register: the
// function is callable as void(int).
constexpr const char* kProcessEventsSymbol =
    "_ZN16QCoreApplication13processEventsE6QFlagsIN10QEventLoop17ProcessEventsFlagEE";
constexpr const char* kAppSelfSymbol = "_ZN16QCoreApplication4selfE";
constexpr int kExcludeUserInputEvents = 0x01; // QEventLoop::ExcludeUserInputEvents

using Lookup = std::function<void*(const char* symbol)>;

struct EventPump {
    void (*processEvents)(int) = nullptr;
    void* const* appSelf = nullptr; // &QCoreApplication::self

    // Run one round of the host's non-input events. onMainThread: the caller
    // is the process's main thread, which owns Qt's GUI event loop.
    void pump(bool onMainThread) const
    {
        if (processEvents && appSelf && *appSelf && onMainThread) {
            processEvents(kExcludeUserInputEvents);
        }
    }
};

inline EventPump resolveEventPump(const Lookup& lookup)
{
    EventPump pump;
    pump.processEvents = reinterpret_cast<void (*)(int)>(lookup(kProcessEventsSymbol));
    pump.appSelf = static_cast<void* const*>(lookup(kAppSelfSymbol));
    return pump;
}

} // namespace ndi_linux_host

#endif // LINUX_HOST_EVENTS_H
