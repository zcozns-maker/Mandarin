// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Driver crash guard for graphics pipeline compiles (Eden-NS2).
//
// Some driver compilers crash the whole process on certain pipelines (AMD's amdvlk64.dll
// failing fast with 0xc0000409 while Donkey Kong Bananza loads). Before a pipeline goes to the
// driver its key and stage hashes are written to <shader dir>/<title>/pipelines_in_flight.txt,
// and removed when the driver returns. If Eden dies mid-compile the file survives. On the next
// boot of that title:
//   - every pipeline that was in flight becomes a "suspect": it is built alone (no other compile
//     in parallel), so a crash then names the culprit exactly; a suspect that builds fine is
//     cleared again,
//   - a suspect that crashes the driver again while built alone is marked "bad" and never built
//     again (its draws are skipped, as for an untranslatable shader). Two crashes are needed,
//     so a crash elsewhere (e.g. out of memory) cannot blacklist an innocent pipeline.
// The list is driver_crash_pipelines.txt next to it; delete it to retry everything (e.g. after
// a driver update). EDEN_NS2_NO_PIPELINE_GUARD=1 turns the guard off.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <system_error>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/ns2_options.h"
#include "common/settings.h"

namespace Vulkan::PipelineCrashGuard {

struct InFlight {
    bool solo{};
    std::array<u64, 6> stages{};
};

struct State {
    std::mutex mutex;
    std::shared_mutex build_lock;
    u64 program_id{};
    bool loaded{};
    std::filesystem::path dir;
    std::set<u64> bad;
    std::set<u64> suspects;
    std::map<u64, InFlight> in_flight;
};

inline State& GetState() {
    static State state;
    return state;
}

inline bool Enabled() {
    // Per-game setting ns2_pipeline_guard (on by default); EDEN_NS2_NO_PIPELINE_GUARD=1 forces off.
    return Settings::NS2::PipelineGuard();
}

inline void SaveList(State& s) {
    std::error_code ec;
    const auto path = s.dir / "driver_crash_pipelines.txt";
    if (s.bad.empty() && s.suspects.empty()) {
        std::filesystem::remove(path, ec);
        return;
    }
    std::ofstream file(path, std::ios::trunc);
    for (const u64 hash : s.bad) file << "bad " << fmt::format("{:016x}", hash) << '\n';
    for (const u64 hash : s.suspects) file << "suspect " << fmt::format("{:016x}", hash) << '\n';
}

inline void WriteInFlight(State& s) {
    std::error_code ec;
    const auto path = s.dir / "pipelines_in_flight.txt";
    if (s.in_flight.empty()) {
        std::filesystem::remove(path, ec);
        return;
    }
    std::ofstream file(path, std::ios::trunc);
    for (const auto& [hash, info] : s.in_flight) {
        file << fmt::format("{:016x} {}", hash, info.solo ? 1 : 0);
        for (const u64 stage : info.stages) file << fmt::format(" {:016x}", stage);
        file << '\n';
    }
}

/// Loads the lists for the running title (once per title). Call with s.mutex held.
inline bool EnsureLoaded(State& s) {
    const u64 program_id = Settings::GetCurrentProgramID();
    if (program_id == 0 || !Enabled()) {
        return false;
    }
    if (s.loaded && s.program_id == program_id) {
        return true;
    }
    s.loaded = true;
    s.program_id = program_id;
    s.bad.clear();
    s.suspects.clear();
    s.in_flight.clear();
    s.dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
            fmt::format("{:016x}", program_id);
    std::error_code ec;
    std::filesystem::create_directories(s.dir, ec);
    {
        std::ifstream file(s.dir / "driver_crash_pipelines.txt");
        std::string kind, hex;
        while (file >> kind >> hex) {
            const u64 hash = std::strtoull(hex.c_str(), nullptr, 16);
            (kind == "bad" ? s.bad : s.suspects).insert(hash);
        }
    }
    std::ifstream crashed(s.dir / "pipelines_in_flight.txt");
    if (crashed) {
        std::map<u64, InFlight> previous;
        std::string line;
        while (std::getline(crashed, line)) {
            u64 hash{};
            int solo{};
            InFlight info{};
            std::size_t pos = 0;
            if (line.size() < 18) continue;
            hash = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
            solo = line[17] == '1';
            pos = 19;
            for (u64& stage : info.stages) {
                if (pos + 16 > line.size()) break;
                stage = std::strtoull(line.substr(pos, 16).c_str(), nullptr, 16);
                pos += 17;
            }
            info.solo = solo != 0;
            previous.emplace(hash, info);
        }
        crashed.close();
        for (const auto& [hash, info] : previous) {
            const bool culprit = info.solo;
            LOG_WARNING(Render_Vulkan,
                        "Eden stopped while the driver compiled pipeline {:016x} (stages vs {:016x} "
                        "tcs {:016x} tes {:016x} gs {:016x} fs {:016x}); {}",
                        hash, info.stages[1], info.stages[2], info.stages[3], info.stages[4],
                        info.stages[5],
                        culprit ? "it crashed again while built alone, so it will be skipped"
                                : "it will be built alone next time to confirm");
            if (culprit) {
                s.suspects.erase(hash);
                s.bad.insert(hash);
            } else {
                s.suspects.insert(hash);
            }
        }
        std::filesystem::remove(s.dir / "pipelines_in_flight.txt", ec);
        SaveList(s);
    }
    if (!s.bad.empty() || !s.suspects.empty()) {
        LOG_WARNING(Render_Vulkan,
                    "Pipeline crash guard: skipping {} pipeline(s) that crashed the driver, building "
                    "{} suspect(s) alone (delete {} to retry them)",
                    s.bad.size(), s.suspects.size(),
                    Common::FS::PathToUTF8String(s.dir / "driver_crash_pipelines.txt"));
    }
    return true;
}

/// True when this pipeline crashed the driver in an earlier run (its draws are skipped).
inline bool IsBad(u64 hash) {
    State& s = GetState();
    std::scoped_lock lock{s.mutex};
    if (!EnsureLoaded(s)) {
        return false;
    }
    return s.bad.contains(hash);
}

/// Scope of one driver compile.
class Scope {
public:
    Scope(u64 hash_, const std::array<u64, 6>& stages) : hash{hash_} {
        State& s = GetState();
        bool solo{};
        {
            std::scoped_lock lock{s.mutex};
            active = EnsureLoaded(s);
            solo = active && s.suspects.contains(hash);
        }
        if (!active) {
            return;
        }
        if (solo) {
            exclusive.emplace(s.build_lock);
        } else {
            shared.emplace(s.build_lock);
        }
        std::scoped_lock lock{s.mutex};
        s.in_flight[hash] = InFlight{solo, stages};
        WriteInFlight(s);
    }
    ~Scope() {
        if (!active) {
            return;
        }
        State& s = GetState();
        {
            std::scoped_lock lock{s.mutex};
            s.in_flight.erase(hash);
            // A suspect built alone without crashing is cleared.
            if (exclusive && s.suspects.erase(hash) != 0) {
                SaveList(s);
            }
            WriteInFlight(s);
        }
        exclusive.reset();
        shared.reset();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    u64 hash{};
    bool active{};
    std::optional<std::unique_lock<std::shared_mutex>> exclusive;
    std::optional<std::shared_lock<std::shared_mutex>> shared;
};

} // namespace Vulkan::PipelineCrashGuard
