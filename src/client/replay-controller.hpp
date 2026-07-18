#pragma once

#include <cstdint>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/input/input-log.hpp>

namespace Gravitaris {

// F5/F6/F7 input record/replay. Client-local (the app owns live keyboard
// state and the actual InputQueue push; this only owns the recorded/replayed
// command streams and their read/write cursors).
class ReplayController {
    InputLog m_recordLog;
    bool m_recording = false;
    std::uint64_t m_recordStartTick = 0;

    InputLog m_replayLog;
    bool m_replaying = false;
    std::size_t m_replayCursor = 0;

    static constexpr const char* REPLAY_PATH = "input-replay.grinput";

public:
    [[nodiscard]] bool IsReplaying() const { return m_replaying; }
    [[nodiscard]] bool IsRecording() const { return m_recording; }

    // Turns recording on (starting fresh, stamped from currentTick) or off
    // (saving to REPLAY_PATH). Stops any active replay first when turning on.
    void ToggleRecording(std::uint64_t currentTick);

    // Loads REPLAY_PATH and starts replaying from it (stopping/flushing an
    // active recording first). Returns false (does nothing else) if the file
    // couldn't be loaded.
    bool StartReplay(std::uint64_t currentTick);

    void StopReplay();

    // Only valid to call while IsReplaying(). Returns this tick's recorded
    // flags and advances the cursor; stops the replay and returns a neutral
    // ControlFlags{} once the log is exhausted.
    ControlFlags NextReplayCommand();

    // Appends `cmd` (tick rewritten relative to the recording's start tick)
    // if currently recording; a no-op otherwise.
    void RecordIfActive(const InputCommand& cmd, std::uint64_t currentTick);
};

} // namespace Gravitaris
