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

} // namespace ndi_linuxdlg

#endif // LINUX_BROWSE_GUARD_H
