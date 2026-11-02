// Re-entrancy guards for the Linux browse dialog (ticket #76).
//
// The Linux dialog runs Resolve's Qt event loop while the zenity/kdialog
// helper is up (LinuxHostEvents.h). ExcludeUserInputEvents holds back clicks
// and keys, but timers, expose events and the window manager's close event
// still run, so Resolve's quit flow, or a nested exec() such as a
// save-project prompt, can destroy the very instance whose instanceChanged
// is waiting in the dialog. The macOS and Windows dialogs share the risk in
// principle (their nested loops run timers too), but only Linux arms these
// guards (NDI_BROWSE_REENTRANT in NDIOutputPlugin.cpp), so their behaviour
// is unchanged.
//
// LiveInstances: which instances are alive, by pointer plus a generation, so
// the code after the dialog can tell "still mine" from "freed" and from "a
// new instance that happens to reuse the address". The plugin adds an
// instance in createInstance and removes it in destroyInstance, takes a
// token before the dialog and re-checks it before touching the instance.
//
// Pure logic, so tests/test_linux_file_dialog.cpp runs it on any platform.

#ifndef LINUX_BROWSE_GUARD_H
#define LINUX_BROWSE_GUARD_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ndi_linuxdlg {

class LiveInstances {
public:
    struct Token {
        const void* instance = nullptr;
        std::uint64_t generation = 0; // 0: the instance wasn't live
    };

    // Registers instance with a fresh generation (an address reused by a new
    // instance never matches an old token).
    void add(const void* instance)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        live_[instance] = nextGeneration_++;
    }

    void remove(const void* instance)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.erase(instance);
    }

    Token token(const void* instance) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Token t;
        t.instance = instance;
        const auto it = live_.find(instance);
        if (it != live_.end()) t.generation = it->second;
        return t;
    }

    // The token's instance is still the one it was taken from. An instance
    // that wasn't registered is never live: the post-dialog code then writes
    // nothing, the safe side.
    bool isLive(const Token& t) const
    {
        if (t.generation == 0) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = live_.find(t.instance);
        return it != live_.end() && it->second == t.generation;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<const void*, std::uint64_t> live_;
    std::uint64_t nextGeneration_ = 1;
};

// Whether a Browse may open a dialog now.
enum class Admission {
    kOpen,      // yes; the caller holds the gate until it closes
    kBusy,      // a dialog is already open: refused, with a reason
    kDebounced, // a replayed click (below): ignored silently
};

// One dialog at a time, and no dialog straight after one.
//
// One at a time: the pump never delivers a click, so a second Browse can't
// normally arrive while one is open; if one does (another thread, a host
// that delivers input anyway), it is refused rather than stacked.
//
// Debounce: the clicks the pump held back are delivered once the action
// that ran the dialog returns, so a second click on Browse made while the
// dialog was up would open a second dialog the moment the first is done.
// A request within debounceMs of the last close is therefore ignored. The
// close is stamped twice: when the helper exits (close), and again when the
// instanceChanged that ran it returns (settle), because the replay comes
// after the post-dialog work (loading the picked STMap can take a second),
// not after the helper. A deliberate new click comes later than that.
class DialogGate {
public:
    static constexpr std::int64_t kDebounceMs = 300;

    explicit DialogGate(std::int64_t debounceMs = kDebounceMs) : debounceMs_(debounceMs) {}

    Admission tryOpen(std::int64_t nowMs)
    {
        if (open_.load()) return Admission::kBusy;
        if (everClosed_.load() && nowMs - lastClosedMs_.load() < debounceMs_.load()) {
            return Admission::kDebounced;
        }
        bool expected = false;
        return open_.compare_exchange_strong(expected, true) ? Admission::kOpen : Admission::kBusy;
    }

    void close(std::int64_t nowMs)
    {
        lastClosedMs_.store(nowMs);
        everClosed_.store(true);
        settlePending_.store(true);
        open_.store(false);
    }

    // The action that ran a dialog has returned: restart the debounce from
    // now. Does nothing for an action that ran no dialog.
    void settle(std::int64_t nowMs)
    {
        if (settlePending_.exchange(false)) lastClosedMs_.store(nowMs);
    }

    bool isOpen() const { return open_.load(); }
    void setDebounceMs(std::int64_t ms) { debounceMs_.store(ms); }

private:
    std::atomic<bool> open_{false};
    std::atomic<bool> everClosed_{false};
    std::atomic<bool> settlePending_{false};
    std::atomic<std::int64_t> lastClosedMs_{0};
    std::atomic<std::int64_t> debounceMs_;
};

// Holds the gate for one dialog and releases it on every way out, an
// exception included, so nothing can lock Browse out for the session.
// now: a monotonic millisecond clock (injected for the tests).
class DialogSlot {
public:
    using Clock = std::int64_t (*)();
    DialogSlot(DialogGate& gate, Clock now) : gate_(gate), now_(now), admission_(gate.tryOpen(now())) {}
    ~DialogSlot()
    {
        if (held()) gate_.close(now_());
    }
    DialogSlot(const DialogSlot&) = delete;
    DialogSlot& operator=(const DialogSlot&) = delete;
    bool held() const { return admission_ == Admission::kOpen; }
    Admission admission() const { return admission_; }

private:
    DialogGate& gate_;
    const Clock now_;
    const Admission admission_;
};

} // namespace ndi_linuxdlg

#endif // LINUX_BROWSE_GUARD_H
