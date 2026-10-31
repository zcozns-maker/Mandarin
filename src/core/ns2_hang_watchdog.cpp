// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/global_scheduler_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/service/service.h"
#include "core/memory.h"
#include "core/ns2_hang_watchdog.h"

namespace Core::NS2 {
namespace {
std::atomic<u64> g_last_present_ms{};

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
} // namespace

void NotePresent() {
    g_last_present_ms.store(NowMs(), std::memory_order_relaxed);
}

// Ported from yuzu_cmd's EDEN_DUMP_THREADS_AT dump. The thread list is read unlocked, which is
// fine for a one-off diagnostic of a title that is already stuck.
void DumpGuestThreads(System& system) {
    auto* process = system.ApplicationProcess();
    if (process == nullptr) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const auto describe_callee_saved = [&memory](const auto& regs) {
        std::string text = " callee:";
        for (int i = 19; i <= 28; ++i) {
            text += fmt::format(" x{}={:#x}", i, regs[i]);
        }
        const auto word = [&memory](u64 address) -> std::string {
            if (address == 0 || (address & 3) != 0 || !memory.IsValidVirtualAddress(address + 3)) {
                return "?";
            }
            return fmt::format("{:#x}", memory.Read32(address));
        };
        text += fmt::format(" [x25]={} [x21+8]={} [x21+0x18]={}", word(regs[25]),
                            word(regs[21] + 8), word(regs[21] + 0x18));
        return text;
    };
    auto& kernel = system.Kernel();
    const auto& scheduler_lock = kernel.GlobalSchedulerContext().SchedulerLock();
    const auto* lock_owner = scheduler_lock.GetOwnerForDebugging();
    LOG_WARNING(Core, "hang dump: scheduler lock owner thread={} count={}",
                lock_owner != nullptr ? static_cast<s64>(lock_owner->GetThreadId()) : -1,
                scheduler_lock.GetLockCountForDebugging());
    for (std::size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core) {
        const auto* current = kernel.Scheduler(core).GetSchedulerCurrentThread();
        const auto* idle = kernel.Scheduler(core).GetIdleThread();
        LOG_WARNING(Core, "hang dump: core {} current thread={} idle={} disable_dispatch={}", core,
                    current != nullptr ? static_cast<s64>(current->GetThreadId()) : -1,
                    current == idle, current != nullptr ? current->GetDisableDispatchCount() : -1);
        Kernel::Svc::ThreadContext live{};
        if (current != nullptr && current != idle &&
            kernel.PhysicalCore(core).GetLiveContextForDebugging(live)) {
            LOG_WARNING(Core, "hang dump: core {} live pc={:#x} lr={:#x} sp={:#x} x0={:#x} x1={:#x} x2={:#x}{}",
                        core, live.pc, live.lr, live.sp, live.r[0], live.r[1], live.r[2],
                        describe_callee_saved(live.r));
        }
    }
    if (const auto in_flight = Service::DescribeInFlightRequests(); !in_flight.empty()) {
        LOG_WARNING(Core, "hang dump: in-flight HLE requests:\n{}", in_flight);
    }
    for (auto& thread : process->GetThreadList()) {
        const auto& ctx = thread.GetContext();
        std::string frames;
        u64 fp = ctx.fp;
        for (int i = 0; i < 12 && fp != 0 && (fp & 7) == 0 && memory.IsValidVirtualAddress(fp + 15); ++i) {
            frames += fmt::format(" {:#x}", memory.Read64(fp + 8));
            const u64 next = memory.Read64(fp);
            if (next <= fp) {
                break;
            }
            fp = next;
        }
        LOG_WARNING(Core,
                    "hang dump: thread id={} prio={} state={:#x} wait={} key={:#x} pc={:#x} lr={:#x} sp={:#x} "
                    "x0={:#x} x1={:#x} x2={:#x} frames:{}{}",
                    thread.GetThreadId(), thread.GetPriority(), static_cast<u32>(thread.GetState()),
                    static_cast<u32>(thread.GetWaitReasonForDebugging()),
                    GetInteger(thread.GetAddressKey()), ctx.pc, ctx.lr, ctx.sp, ctx.r[0], ctx.r[1],
                    ctx.r[2], frames,
                    Kernel::Svc::DescribeDiagSvc(thread.GetThreadId()) +
                        (static_cast<u32>(thread.GetState()) == 2 ? describe_callee_saved(ctx.r)
                                                                  : std::string{}));
    }
    LOG_WARNING(Core, "hang dump: end");
}

std::unique_ptr<HangWatchdog> HangWatchdog::Create(System& system) {
    unsigned seconds = Settings::values.ns2_hang_dump_seconds.GetValue();
    if (const char* value = std::getenv("EDEN_NS2_HANG_DUMP_S")) {
        const long parsed = std::strtol(value, nullptr, 10);
        seconds = parsed > 0 ? static_cast<unsigned>(parsed) : 0;
    }
    if (seconds == 0) {
        return nullptr;
    }
    LOG_INFO(Core, "Hang watchdog on: guest threads are dumped after {} s without a frame", seconds);
    return std::make_unique<HangWatchdog>(system, seconds);
}

HangWatchdog::HangWatchdog(System& system, unsigned seconds) {
    g_last_present_ms.store(0, std::memory_order_relaxed);
    thread = std::jthread([&system, seconds](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        const u64 limit_ms = u64{seconds} * 1000;
        u64 resumed_ms = NowMs();
        bool dumped = false;
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock{mutex};
                cv.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; });
            }
            if (stop.stop_requested()) {
                break;
            }
            const u64 now = NowMs();
            if (system.IsPaused()) {
                resumed_ms = now;
                continue;
            }
            const u64 last = g_last_present_ms.load(std::memory_order_relaxed);
            if (last == 0) {
                continue; // nothing presented yet (still booting)
            }
            const u64 since = now - std::max(last, resumed_ms);
            if (since < limit_ms) {
                dumped = false;
                continue;
            }
            if (!dumped) {
                dumped = true;
                LOG_WARNING(Core, "No frame presented for {} s; dumping guest threads", since / 1000);
                DumpGuestThreads(system);
            }
        }
    });
}

HangWatchdog::~HangWatchdog() = default;

} // namespace Core::NS2