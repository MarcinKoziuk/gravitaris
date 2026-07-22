#include <algorithm>
#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/net/snapshot.hpp>

#include <gravitaris/game/net/own-ship-sync.hpp>

namespace Gravitaris {

namespace {

// The own predicted ship's entry in `snapshot`, or nullptr if this snapshot
// doesn't (yet, or no longer) contain that NetId -- e.g. between a respawn's
// ServerWelcome and the first snapshot that reflects the new ship.
const EntityState* FindOwnShipState(const SnapshotData& snapshot, std::uint32_t yourShipNetId)
{
    const auto it = std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
                                 [&](const EntityState& e) { return e.netId == yourShipNetId; });
    return it != snapshot.entities.end() ? &*it : nullptr;
}

} // namespace

OwnShipSync::OwnShipSync(ClientPrediction& clientPrediction, NetClient& netClient, PredictedTickClock& tickClock)
        : m_clientPrediction(clientPrediction)
        , m_netClient(netClient)
        , m_tickClock(tickClock)
{}

bool OwnShipSync::DropIfStale()
{
    if (!m_clientPrediction.HasOwnShip()) return false;

    const std::optional<SnapshotData>& current = m_netClient.GetLatestSnapshot();
    const bool stillPresent = current && FindOwnShipState(*current, m_netClient.GetYourShipNetId());
    if (stillPresent) return false;

    m_clientPrediction.DestroyOwnShip();
    m_visualCorrectionOffset = {};
    return true;
}

std::optional<flecs::entity> OwnShipSync::SpawnIfConfirmed()
{
    if (m_clientPrediction.HasOwnShip()) return std::nullopt;

    const std::optional<SnapshotData>& snapshot = m_netClient.GetLatestSnapshot();
    if (!snapshot) return std::nullopt;
    const EntityState* ownShip = FindOwnShipState(*snapshot, m_netClient.GetYourShipNetId());
    if (!ownShip) return std::nullopt;

    m_clientPrediction.SpawnOwnShip(
            ownShip->modelId, Vector2d{static_cast<double>(ownShip->pos.x()), static_cast<double>(ownShip->pos.y())},
            m_netClient.GetYourTeam());
    m_tickClock.Reset(m_netClient.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS);
    return m_clientPrediction.GetOwnShip();
}

std::optional<OwnShipSync::ReconcileResult> OwnShipSync::ReconcileIfNeeded()
{
    if (!m_clientPrediction.HasOwnShip()) return std::nullopt;

    const std::optional<SnapshotData>& snapshot = m_netClient.GetLatestSnapshot();
    if (!snapshot || snapshot->tick <= m_lastReconciledTick) return std::nullopt;
    m_lastReconciledTick = snapshot->tick;

    const EntityState* ownShip = FindOwnShipState(*snapshot, m_netClient.GetYourShipNetId());
    if (!ownShip) return std::nullopt;

    const std::optional<Vector2d> preCorrection =
            m_clientPrediction.Reconcile(snapshot->tick, *ownShip, snapshot->entities);
    if (!preCorrection) return std::nullopt;

    // The ship's real Transform now holds the corrected position; blend the
    // visual gap out via the caller's camera (see m_visualCorrectionOffset's
    // doc) instead of touching it.
    const Transform& t = m_clientPrediction.GetOwnShip().get<Transform>();
    const Magnum::Vector2 correctedPos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
    const Magnum::Vector2 preCorrectionPos{static_cast<float>(preCorrection->x()), static_cast<float>(preCorrection->y())};
    m_visualCorrectionOffset += preCorrectionPos - correctedPos;

    return ReconcileResult{snapshot->tick, (preCorrectionPos - correctedPos).length()};
}

void OwnShipSync::DecayCorrection(float dtSeconds)
{
    static constexpr float CORRECTION_SMOOTH_SECONDS = 0.1f;
    m_visualCorrectionOffset *= std::exp(-dtSeconds / CORRECTION_SMOOTH_SECONDS);
}

} // namespace Gravitaris
