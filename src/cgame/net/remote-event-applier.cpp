#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/net/snapshot.hpp>

#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/net/remote-event-applier.hpp>

namespace Gravitaris {

RemoteEventApplier::RemoteEventApplier(NetClient& netClient, GameEventQueue& eventQueue,
                                       CosmeticBulletReaper& bulletReaper)
        : m_netClient(netClient)
        , m_eventQueue(eventQueue)
        , m_bulletReaper(bulletReaper)
{}

void RemoteEventApplier::Apply(const std::function<flecs::entity(std::uint32_t)>& resolveHitTarget)
{
    const std::uint32_t yourShipNetId = m_netClient.GetYourShipNetId();

    for (const SnapshotData& snapshot : m_netClient.GetSnapshotHistory()) {
        for (const GameEvent& event : snapshot.events) {
            if (event.seq <= m_lastAppliedSeq) continue;
            m_lastAppliedSeq = event.seq;

            if (event.type == GameEventType::BulletFired && event.sourceNetId == yourShipNetId) continue;

            m_eventQueue.Emit(event.type, flecs::entity{}, event.pos, event.param);

            // The shooter's own bullet is a purely cosmetic client-predicted
            // copy (ClientPrediction::Step) -- without this it would keep
            // flying through whatever it actually hit, even though the
            // server's real bullet was destroyed the instant it registered
            // the hit. See CosmeticBulletReaper's own class doc comment for
            // why matching by position (not owner id).
            if (event.type == GameEventType::Impact) {
                const Vector2d impactPos{static_cast<double>(event.pos.x()), static_cast<double>(event.pos.y())};
                m_bulletReaper.MatchImpact(impactPos);
            }

            if (event.type != GameEventType::Impact && event.type != GameEventType::LandingCrash) continue;

            const flecs::entity target = resolveHitTarget(event.sourceNetId);
            if (!target.is_alive()) continue; // e.g. the hit killed it this tick

            if (HitFlash* flash = target.try_get_mut<HitFlash>()) {
                flash->amount = 1.f;
            }
        }
    }
}

} // namespace Gravitaris
