#include <gravitaris/game/logging.hpp>

#include "replay-controller.hpp"

//Claude: put this into cgame, client is only
namespace Gravitaris {

void ReplayController::ToggleRecording(std::uint64_t currentTick)
{
    if (m_recording) {
        m_recording = false;
        if (m_recordLog.Save(REPLAY_PATH)) {
            LOG(info) << "Saved input replay '" << REPLAY_PATH << "' ("
                      << m_recordLog.Size() << " commands)";
        } else {
            LOG(warning) << "Failed to save input replay '" << REPLAY_PATH << "'";
        }
    } else {
        StopReplay();
        m_recordLog.Clear();
        m_recordStartTick = currentTick;
        m_recording = true;
        LOG(info) << "Recording input at tick " << m_recordStartTick;
    }
}

bool ReplayController::StartReplay(std::uint64_t currentTick)
{
    if (m_recording) ToggleRecording(currentTick); // stop & flush recording first

    if (!m_replayLog.Load(REPLAY_PATH)) {
        LOG(warning) << "No input replay to load at '" << REPLAY_PATH << "'";
        return false;
    }

    m_replaying = true;
    m_replayCursor = 0;
    LOG(info) << "Replaying input '" << REPLAY_PATH << "' ("
              << m_replayLog.Size() << " commands)";
    return true;
}

void ReplayController::StopReplay()
{
    if (!m_replaying) return;
    m_replaying = false;
    LOG(info) << "Replay stopped";
}

ControlFlags ReplayController::NextReplayCommand()
{
    if (m_replayCursor >= m_replayLog.Size()) {
        StopReplay();
        return ControlFlags{};
    }
    const ControlFlags flags = m_replayLog.Commands()[m_replayCursor].flags;
    ++m_replayCursor;
    return flags;
}

void ReplayController::RecordIfActive(const InputCommand& cmd, std::uint64_t currentTick)
{
    if (!m_recording) return;
    // Store tick-relative so the log can replay from any starting tick.
    InputCommand rec = cmd;
    rec.tick = currentTick - m_recordStartTick;
    m_recordLog.Append(rec);
}

} // namespace Gravitaris
