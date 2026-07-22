#pragma once

#include <cstdint>
#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/net/client-prediction.hpp>
#include <gravitaris/game/net/net-client.hpp>
#include <gravitaris/game/net/predicted-tick-clock.hpp>

namespace Gravitaris {

// Owns the full lifecycle of a net-client's own locally-predicted ship: the
// staleness/spawn gate (DropIfStale/SpawnIfConfirmed, previously split
// across two branches of the same check in CGame::TickNetClient),
// reconciliation against the latest snapshot (ReconcileIfNeeded), and the
// visual-only correction offset that blends a reconciliation snap in over
// time instead of popping (DecayCorrection/GetCorrectionOffset) -- applied
// to the camera and this frame's draw position only, never the real
// simulated Transform, which must stay exactly correct for the next
// predicted tick to build on.
class OwnShipSync {
public:
    OwnShipSync(ClientPrediction& clientPrediction, NetClient& netClient, PredictedTickClock& tickClock);

    // The ship this client is predicting must still be the one the server
    // has for it. It won't be if it died (crashed into a sun --
    // ClientPrediction has no collision damage of its own, so this is the
    // only way the local ship finds out) or was just replaced by a respawn
    // under a new NetId (NetClient::GetYourShipNetId() changes the instant
    // the re-welcome packet arrives, ahead of any snapshot reflecting the
    // new ship). Either way, an authoritative snapshot no longer containing
    // this NetId means the local prediction is stale and must be dropped --
    // SpawnIfConfirmed then waits for a fresh snapshot with the (possibly
    // new) NetId, same as the very first spawn. Returns true if it did just
    // drop the ship -- the caller owns the player-entity handle (e.g.
    // CGame::m_player) and must reset it itself.
    bool DropIfStale();

    // Waits for a snapshot that actually confirms this NetId and where it
    // is, so the predicted ship spawns at the real position instead of
    // popping in from the origin once the first reconciliation runs. No-op
    // (returns nullopt) if there's already an own ship, or none confirmed
    // yet. Also resets the tick clock to the current server-tick estimate,
    // since a fresh ship needs a fresh prediction baseline.
    std::optional<flecs::entity> SpawnIfConfirmed();

    struct ReconcileResult {
        std::uint64_t tick;
        float correctionMagnitude;
    };

    // Reconciles against the latest snapshot if it's new (i.e. not already
    // processed), accumulating the visual correction offset. Returns
    // nullopt if there's no own ship, no new snapshot, or the snapshot
    // doesn't (yet) confirm this NetId.
    std::optional<ReconcileResult> ReconcileIfNeeded();

    // Blends the correction offset toward zero -- call once per rendered
    // frame, before reading GetCorrectionOffset() for this frame's camera/
    // draw-position use.
    void DecayCorrection(float dtSeconds);

    [[nodiscard]] Magnum::Vector2 GetCorrectionOffset() const { return m_visualCorrectionOffset; }

private:
    ClientPrediction& m_clientPrediction;
    NetClient& m_netClient;
    PredictedTickClock& m_tickClock;

    // Newest snapshot tick already reconciled against, so a snapshot
    // arriving before the next rendered frame (or one with nothing new)
    // isn't reprocessed.
    std::uint64_t m_lastReconciledTick = 0;
    Magnum::Vector2 m_visualCorrectionOffset{0.f, 0.f};
};

} // namespace Gravitaris
