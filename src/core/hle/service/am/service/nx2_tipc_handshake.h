// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/logging.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::AM {

// Launch-era Switch 2 SDKs (Mario Kart World 1.0, Donkey Kong Bananza 1.0) open each graphics
// sub-session (syncpt, gpu, vi2, ...) with TIPC command 0 carrying a u64 token and abort unless the
// reply repeats it (reply: result + u64). Newer SDKs skip it, so sessions without their own command
// 0 answer it here instead of failing with NotImplemented.
inline bool TryTipcEchoHandshake(HLERequestContext& ctx, const char* session_name) {
    const u32* packet = ctx.CommandBuffer();
    if (!ctx.IsTipc() || ctx.GetCommand() != 0 || packet[0] != 0x10 || packet[1] != 2) {
        return false;
    }
    const u64 value = u64{packet[2]} | (u64{packet[3]} << 32);
    LOG_DEBUG(Service_AM, "{} TIPC handshake echo value={:#x}", session_name, value);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess.raw);
    rb.Push(value);
    return true;
}

// TIPC reply result word: after the two header words, an optional special header plus its
// process id and handle words.
inline u32 TipcReplyResult(const u32* reply) {
    u32 offset = 2;
    if (reply[1] & 0x80000000U) {
        const u32 special = reply[2];
        offset += 1 + ((special & 1) ? 2 : 0) + ((special >> 1) & 0xf) + ((special >> 5) & 0xf);
    }
    return offset < 64 ? reply[offset] : 0;
}

} // namespace Service::AM
