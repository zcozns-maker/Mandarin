// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Eden-NS2 hang watchdog: when a Switch 2 title presents no frame for N seconds (per-game setting
// ns2_hang_dump_seconds, or EDEN_NS2_HANG_DUMP_S), log what every guest thread is blocked on.
// Diagnostic only; off (0) by default.

#include <memory>
#include <thread>

namespace Core {
class System;
}

namespace Core::NS2 {

/// Called on every native present.
void NotePresent();

/// Logs scheduler state, every guest thread's state, wait reason, registers and frame chain.
void DumpGuestThreads(System& system);

class HangWatchdog {
public:
    /// Returns nullptr when the watchdog is disabled for this title.
    static std::unique_ptr<HangWatchdog> Create(System& system);
    HangWatchdog(System& system, unsigned seconds);
    ~HangWatchdog();

private:
    std::jthread thread;
};

} // namespace Core::NS2