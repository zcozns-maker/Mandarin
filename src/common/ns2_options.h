// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Switch 2 / Eden-NS2 options: per-game settings (Settings::values.ns2_*) with the older
// environment variables still taking priority when they are set, for debugging.
// Values are read on every call (cheap) so a per-game value applies to the game being booted,
// even when several games are started in one Eden session.

#include <cstdlib>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/settings.h"

namespace Settings::NS2 {

namespace detail {
inline bool ListContains(const char* list, std::string_view group) {
    std::string_view rest{list};
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        std::string_view item = rest.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item == group || item == "all") {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return false;
}
} // namespace detail

// Off-by-default shader lowering groups. EDEN_NATIVE_LOWER_ENABLE, when set, replaces the settings.
inline bool LowerGroupEnabled(std::string_view group) {
    static const char* const env = std::getenv("EDEN_NATIVE_LOWER_ENABLE");
    if (env != nullptr) {
        return detail::ListContains(env, group);
    }
    auto& v = Settings::values;
    if (group == "isbe_attr") return v.ns2_lower_isbe_attr.GetValue();
    if (group == "warpsync_relaxed") return v.ns2_lower_warpsync_relaxed.GetValue();
    if (group == "surface_atomic") return v.ns2_lower_surface_atomic.GetValue();
    if (group == "surface_dims") return v.ns2_lower_surface_dims.GetValue();
    if (group == "local_vector_spill") return v.ns2_lower_local_vector_spill.GetValue();
    if (group == "uniform_vote") return v.ns2_lower_uniform_vote.GetValue();
    if (group == "isbewr_drop") return v.ns2_lower_isbewr_drop.GetValue();
    if (group == "audit_fsel") return v.ns2_lower_audit_fsel.GetValue();
    if (group == "ret_branch") return v.ns2_lower_ret_branch.GetValue();
    if (group == "extra_forms") return v.ns2_lower_extra_forms.GetValue();
    return false;
}

// Bounded nvhost_ctrl fence wait in milliseconds; 0 waits forever (original behaviour).
inline u32 FenceTimeoutMs() {
    static const char* const env = std::getenv("EDEN_NS2_FENCE_TIMEOUT_MS");
    if (env != nullptr) {
        const long value = std::strtol(env, nullptr, 10);
        return value > 0 ? static_cast<u32>(value) : 0;
    }
    return Settings::values.ns2_fence_timeout_ms.GetValue();
}

// Stall on native GPU work that cannot be translated instead of skipping it.
inline bool NativeStrict() {
    static const bool env = std::getenv("EDEN_NATIVE_STRICT") != nullptr;
    return env || Settings::values.ns2_native_strict.GetValue();
}

// Driver crash guard for graphics pipeline compiles. EDEN_NS2_NO_PIPELINE_GUARD=1 forces it off.
inline bool PipelineGuard() {
    static const bool env_off = std::getenv("EDEN_NS2_NO_PIPELINE_GUARD") != nullptr;
    return !env_off && Settings::values.ns2_pipeline_guard.GetValue();
}

// Undefined shader values read as 0 instead of OpUndef (Eglantine bb353609e6, AMD colour
// garbage). Per-game setting ns2_undef_zero, on by default; EDEN_NX2_UNDEF_ZERO=0 forces off.
// MiB of VRAM heap budget below which unused textures are freed early (0 = off).
// EDEN_NS2_VRAM_FREE_EARLY_MIB=<n> overrides the per-game setting ns2_vram_free_early_mib.
inline u32 VramFreeEarlyMiB() {
    if (const char* env = std::getenv("EDEN_NS2_VRAM_FREE_EARLY_MIB")) {
        const long value = std::strtol(env, nullptr, 10);
        return value > 0 ? static_cast<u32>(value) : 0;
    }
    return Settings::values.ns2_vram_free_early_mib.GetValue();
}

// Read traps on GPU-written buffer pages (per-game ns2_read_traps, off by default).
// EDEN_NX2_READ_TRAPS=0/1 forces them off/on.
inline bool ReadTraps() {
    static const int forced = [] {
        const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
        return value == nullptr ? -1 : (value[0] == '0' ? 0 : 1);
    }();
    if (forced >= 0) {
        return forced == 1;
    }
    return Settings::values.ns2_read_traps.GetValue();
}

inline bool DropCrashedGeometryShader() {
    static const bool env = std::getenv("EDEN_NS2_DROP_CRASHED_GS") != nullptr;
    return env || Settings::values.ns2_drop_crashed_gs.GetValue();
}

// Per-game ns2_crashed_gs_single_lane: retry crash-guard pipelines with a single-lane geometry shader.
inline bool CrashedGsSingleLane() {
    return Settings::values.ns2_crashed_gs_single_lane.GetValue();
}

inline bool UndefZero() {
    static const bool env_off = [] {
        const char* const value = std::getenv("EDEN_NX2_UNDEF_ZERO");
        return value != nullptr && value[0] == '0';
    }();
    return !env_off && Settings::values.ns2_undef_zero.GetValue();
}

} // namespace Settings::NS2