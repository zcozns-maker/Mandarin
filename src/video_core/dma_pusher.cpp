// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include "common/ns2_options.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

namespace {
// ZiK build13: GPU-written command data (ns2_pb_flush_all_caches, EDEN_NX2_PB_DIAG=1).
constexpr u32 DirtyTexture = 1;
constexpr u32 DirtyQuery = 2;
constexpr u32 DirtyBuffer = 4;

bool PbDiagOnly() {
    static const bool diag = [] {
        const char* value = std::getenv("EDEN_NX2_PB_DIAG");
        return value != nullptr && value[0] == '1';
    }();
    return diag;
}

std::string DirtyNames(u32 mask, bool marked) {
    std::string names;
    const auto add = [&names](const char* name) {
        if (!names.empty()) {
            names += '+';
        }
        names += name;
    };
    if (mask & DirtyTexture) {
        add("texture");
    }
    if (mask & DirtyQuery) {
        add("query");
    }
    if (mask & DirtyBuffer) {
        add(marked ? "buffer" : "buffer-unmarked");
    }
    return names;
}

std::mutex pb_log_mutex;
std::unordered_set<u64> pb_logged_pages;
u32 pb_log_lines{};
std::atomic<u64> pb_flushed_chunks{};
std::atomic<u64> pb_learned_pages{};
std::atomic<bool> pb_announced{};
} // Anonymous namespace

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

void DmaPusher::DispatchCalls() {
    dma_state.is_last_call = true;
    auto& split = VideoCommon::GpuSplit::Get();
    if (split.Active()) {
        split.SetAfterMethod(channel_state.native_abi ? &FailedNativeWorkHook : nullptr, this);
    }
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
    }
    if (split.Active()) {
        split.Defer([gpu = &system.GPU()] {
            gpu->FlushCommands();
            gpu->OnCommandListEnd();
        });
        split.Publish();
        return;
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

bool DmaPusher::HasFailedNativeWork() const {
    // The failure flags belong to the back thread when the GPU thread is split.
    if (VideoCommon::GpuSplit::Get().Active()) {
        return false;
    }
    return CheckFailedNativeWork();
}

void DmaPusher::FailedNativeWorkHook(void* dma_pusher) {
    static_cast<const DmaPusher*>(dma_pusher)->CheckFailedNativeWork();
}

bool DmaPusher::CheckFailedNativeWork() const {
    const auto& context = channel_state.native_macro_context;
    if (!channel_state.native_abi || !context ||
        !(context->engine.native_execution_failed || context->compute_engine.native_execution_failed)) {
        return false;
    }
    // EDEN_NATIVE_STRICT keeps the channel stalled for debugging; otherwise drop the failed work
    // (a skipped draw or dispatch) and keep the game running instead of hanging on its fences.
    const bool strict = Settings::NS2::NativeStrict();
    if (strict) {
        return true;
    }
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 32) {
        LOG_WARNING(HW_GPU, "Native GPU work failed; skipping it and continuing the channel");
    }
    context->engine.native_execution_failed = false;
    context->compute_engine.native_execution_failed = false;
    return false;
}

bool DmaPusher::Step() {
    if (HasFailedNativeWork()) return false;
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }

    if (!puller.ResumeNativeAcquire(*this)) return false;

    CommandList& command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0) {
        const std::span<const CommandHeader> commands{command_list.prefetch_command_list};
        command_resume_offset += ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        if (puller.HasPendingNativeAcquire() || HasFailedNativeWork()) return false;
        command_resume_offset = 0;
        dma_pushbuffer.pop();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    dma_state.dma_get = header.addr;

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced; });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                auto* const engine_state = subchannels[dma_state.subchannel];
                // Parameters continued in an entry of their own can be arguments the GPU wrote
                // (an indirect draw record a culling pass filled in). Unsafe reads would give the
                // macro the older guest words, so flush those first. Safe reads (below) already
                // flush. EDEN_NX2_PBFLUSH=0 turns this off.
                static const bool flush_parameters = [] {
                    const char* value = std::getenv("EDEN_NX2_PBFLUSH");
                    return value == nullptr || value[0] != '0';
                }();
                const bool flushed_parameters =
                    flush_parameters && words <= 64 &&
                    FlushGpuWrittenCommands(dma_state.dma_get, words * sizeof(u32));
                if (flushed_parameters) {
                    engine_state->current_dirty = false;
                } else if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                    // The caches behind IsMemoryDirty belong to the split's back thread.
                    split.Defer([engine_state, memory = &memory_manager, address = GPUVAddr{dma_state.dma_get},
                                 bytes = words * sizeof(u32)] {
                        engine_state->current_dirty = memory->IsMemoryDirty(address, bytes);
                    });
                } else {
                    engine_state->current_dirty = memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
                }
            }
        }
        // A safe read flushes the caches, which belong to the split's back thread while it runs
        // (the split only starts with unsafe reads; the settings can change mid-game).
        const bool use_safe = !VideoCommon::GpuSplit::Get().Active() &&
                              (Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh() : Settings::IsDMALevelSafe());
        if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            command_resume_offset += ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            // ZiK build13: ns2_pb_flush_all_caches flushes, from every cache, the chunks whose
            // words the GPU may have written (Safe DMA's read, for those chunks only).
            const size_t chunk_bytes = size_t{header.size} * sizeof(u32);
            if (Settings::values.ns2_pb_flush_all_caches.GetValue()) {
                FlushGpuWrittenCommandData(dma_state.dma_get, chunk_bytes, true);
            } else {
                FlushGpuWrittenCommands(dma_state.dma_get, chunk_bytes);
                if (PbDiagOnly()) {
                    FlushGpuWrittenCommandData(dma_state.dma_get, chunk_bytes, false);
                }
            }
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            command_resume_offset += ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        }
    }

    if (puller.HasPendingNativeAcquire() || HasFailedNativeWork()) return false;
    command_resume_offset = 0;
    if (++dma_pushbuffer_subindex >= command_list_size) {
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        VideoCommon::GpuSplit::Get().Drain();
        rasterizer->SignalFence([this]() {
            std::scoped_lock lk(sync_mutex);
            synced = true;
            sync_cv.notify_all();
        });
    }

    return true;
}

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; //must ref (MUltiMethod re)
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            if (channel_state.native_abi && dma_state.method < non_puller_methods) max_write = 1;
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            auto const command_header = commands[index]; //can copy
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            auto const command_header = commands[index]; //can copy
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = u64(-s64(dma_state.dma_get)); // negate to set address as 0
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
        if (puller.HasPendingNativeAcquire() || HasFailedNativeWork()) return index;
    }
    return commands.size();
}


namespace {
// ZiK build11: a guest command sent to a subchannel with no engine bound used to dereference
// null (Pokopia, 22 min in). Log it once per subchannel/method burst and drop the command.
std::atomic<u32> g_null_subchannel_logs{0};
void LogNullSubchannel(u32 subchannel, u32 method, u32 type) {
    const u32 n = g_null_subchannel_logs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || (n & 0xFFF) == 0) {
        LOG_ERROR(HW_GPU,
                  "Dropped GPU method 0x{:X} on subchannel {} with no engine bound (type {}, "
                  "drop #{})",
                  method, subchannel, type, n + 1);
    }
}
} // namespace

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}

void DmaPusher::CallMethod(u32 argument) {
    if (dma_state.method < non_puller_methods) {
        puller.CallPullerMethod(*this, Engines::Puller::MethodCall{
            dma_state.method,
            argument,
            dma_state.subchannel,
            dma_state.method_count,
        });
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            LogNullSubchannel(dma_state.subchannel, dma_state.method,
                              static_cast<u32>(subchannel_type[dma_state.subchannel]));
            return;
        }
        if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMethod(system, split, dma_state.method, argument,
                                                   dma_state.is_last_call);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMethod(system, split, dma_state.method, argument,
                                                 dma_state.is_last_call, &segment);
                        return;
                    }
                }
            }
            split.Method(subchannel, dma_state.method, argument, dma_state.is_last_call,
                         WantsSegment() ? &segment : nullptr);
            return;
        }
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    if (dma_state.method < non_puller_methods) {
        puller.CallMultiMethod(*this, dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            LogNullSubchannel(dma_state.subchannel, dma_state.method,
                              static_cast<u32>(subchannel_type[dma_state.subchannel]));
            return;
        }
        if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                        num_methods, dma_state.method_count);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                      num_methods, dma_state.method_count, &segment);
                        return;
                    }
                }
            }
            split.MultiMethod(subchannel, dma_state.method, base_start, num_methods,
                              dma_state.method_count, WantsSegment() ? &segment : nullptr);
            return;
        }
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

bool DmaPusher::WantsSegment() const {
    // Only macro parameters and compute uploads read the engine's current_dma_segment.
    const auto engine = subchannel_type[dma_state.subchannel];
    return engine == Engines::EngineTypes::KeplerCompute ||
           (engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart);
}

bool DmaPusher::FlushGpuWrittenCommands(GPUVAddr address, size_t size) {
    // Mario Kart World runs small command lists its culling dispatches fill in each frame. Safe
    // reads flush every command list; unsafe ones only flush those on pages the GPU has written,
    // or their draws run with an older frame's words. Only those pages wait for the split's back.
    if (!g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    bool written = false;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page); device && Nx2IsGpuWritten(*device)) {
            written = true;
            break;
        }
    }
    if (!written) {
        return false;
    }
    // The caches belong to the split's back: it first runs what the front queued (the dispatch
    // that writes these words among it).
    VideoCommon::GpuSplit::Get().Drain();
    if (!memory_manager.IsMemoryDirty(address, size, VideoCommon::CacheType::BufferCache)) {
        return false;
    }
    memory_manager.FlushRegion(address, size, VideoCommon::CacheType::BufferCache);
    return true;
}

void DmaPusher::FlushGpuWrittenCommandData(GPUVAddr address, size_t size, bool flush) {
    if (!g_nx2_ampere.load(std::memory_order_relaxed) || size == 0 || rasterizer == nullptr) {
        return;
    }
    if (!pb_announced.exchange(true)) {
        LOG_INFO(HW_GPU, "NS2 PB data: {} (ns2_pb_flush_all_caches / EDEN_NX2_PB_DIAG)",
                 flush ? "targeted command-data flush on" : "diagnostic only, no flushes");
    }
    bool written = false;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page); device && Nx2IsGpuWritten(*device)) {
            written = true;
            break;
        }
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (written || !split.Active()) {
        // The caches belong to the split's back: let it run what was queued (the work that
        // writes these words among it) first. Without the split this thread owns them.
        split.Drain();
        ScanGpuWrittenCommandData(address, size, flush, written ? 'F' : 'I', dma_state.method,
                                  dma_state.subchannel);
        return;
    }
    // Pages not known to be GPU-written: check when the back reaches this point instead of
    // stalling the front for every chunk. A hit marks the pages, so their next read (command
    // lists are rebuilt in place every frame) takes the drain-and-flush path above.
    split.Defer([this, address, size, method = dma_state.method, subchannel = dma_state.subchannel] {
        ScanGpuWrittenCommandData(address, size, false, 'B', method, subchannel);
    });
}

u32 DmaPusher::ScanGpuWrittenCommandData(GPUVAddr address, size_t size, bool flush, char path,
                                         u32 method, u32 subchannel) {
    u32 all = 0;
    const GPUVAddr end = address + size;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < end; page += 0x1000) {
        const GPUVAddr begin = (std::max)(page, address);
        const u64 bytes = (std::min)(page + 0x1000, end) - begin;
        const auto device = memory_manager.GpuToCpuAddress(begin);
        if (!device) {
            continue;
        }
        const u32 mask = rasterizer->Ns2DirtyCaches(*device, bytes);
        if (mask == 0) {
            continue;
        }
        all |= mask;
        const bool marked = Nx2IsGpuWritten(*device);
        // A buffer range on a marked page is what FlushGpuWrittenCommands already handles.
        const bool new_kind = (mask & (DirtyTexture | DirtyQuery)) != 0 || !marked;
        if (!marked) {
            Nx2MarkGpuWritten(*device, bytes);
            pb_learned_pages.fetch_add(1, std::memory_order_relaxed);
        }
        if (!new_kind) {
            continue;
        }
        std::scoped_lock lock{pb_log_mutex};
        if (pb_log_lines < 64 && pb_logged_pages.insert(*device >> 12).second) {
            ++pb_log_lines;
            LOG_WARNING(HW_GPU,
                        "NS2 PB data hit ({}): gpu={:#x} device={:#x} size={:#x} caches={} "
                        "chunk={:#x}+{:#x} method={:#x} subchannel={} flush={}",
                        path == 'F' ? "front, after drain" : path == 'I' ? "inline, no split" : "back, page marked",
                        begin, *device, bytes, DirtyNames(mask, marked), address, size, method,
                        subchannel, flush && path != 'B');
        }
    }
    if (all != 0 && flush) {
        // As Safe DMA's SafeRead: every cache type.
        memory_manager.FlushRegion(address, size);
        const u64 count = pb_flushed_chunks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 20000 == 0) {
            LOG_INFO(HW_GPU, "NS2 PB data: {} command-list chunks flushed so far, {} pages learned",
                     count, pb_learned_pages.load(std::memory_order_relaxed));
        }
    }
    return all;
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

} // namespace Tegra
