#include <algorithm>
#include <vector>

#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>

#include <gravitaris/cgame/net/cosmetic-bullet-despawner.hpp>

namespace Gravitaris {

CosmeticBulletDespawner::CosmeticBulletDespawner(flecs::world& registry, flecs::world& mirrorWorld)
        : m_registry(registry)
        , m_mirrorWorld(mirrorWorld)
{}

void CosmeticBulletDespawner::CheckLocalHits()
{
    std::vector<flecs::entity> hitBullets;
    m_registry.each([&](flecs::entity bulletEnt, const Bullet& bullet, const Transform& bulletTransf) {
        const Vector2d& a = bulletTransf.prevPos;
        const Vector2d& b = bulletTransf.pos;
        const Vector2d ab = b - a;
        const double abLengthSq = ab.dot();

        bool hit = false;
        m_mirrorWorld.each([&](flecs::entity, const Team& shipTeam, const Controls&, const Transform& shipTransf) {
            if (hit) return;
            if (shipTeam.id == bullet.team) return; // no friendly fire

            double distSq;
            if (abLengthSq < 1e-9) {
                distSq = (shipTransf.pos - a).dot();
            } else {
                const double t = std::clamp(Magnum::Math::dot(shipTransf.pos - a, ab) / abLengthSq, 0.0, 1.0);
                distSq = (shipTransf.pos - (a + ab * t)).dot();
            }
            if (distSq <= LOCAL_HIT_RADIUS * LOCAL_HIT_RADIUS) hit = true;
        });
        if (hit) hitBullets.push_back(bulletEnt);
    });

    for (flecs::entity bulletEnt : hitBullets) bulletEnt.destruct();
}

// No ownerNetId filter needed (fixed 2026-07-21 -- an earlier version had
// one, and it silently never matched, so this whole check was dead code on
// a real client). The bug: ClientPrediction::Step stamps Bullet::ownerNetId
// from `m_ownShip.get<NetId>().value` -- but m_ownShip is spawned locally
// via EntitySpawner::SpawnPlayer, which assigns NetId from *this client's
// own* AssignNetId counter, a value with no relation to
// NetClient::GetYourShipNetId() (the *server's* NetId for this ship,
// received in ServerWelcome). Comparing one against the other can never
// match. The actual fix doesn't need either value: m_registry (passed in as
// `registry`) is this client's own local prediction registry, and in
// net-client mode it only ever holds this client's own ship, its own
// cosmetic bullets, and Phase 7's planet proxies -- nothing else is ever
// spawned into it. So every Bullet found here is definitionally this
// client's own; there is nothing to filter.
void CosmeticBulletDespawner::MatchImpact(const Vector2d& impactPos)
{
    std::vector<flecs::entity> matchedBullets;
    m_registry.each([&](flecs::entity bulletEnt, const Bullet&, const Transform& transf) {
        if ((transf.pos - impactPos).length() > BULLET_IMPACT_MATCH_RADIUS) return;
        matchedBullets.push_back(bulletEnt);
    });
    for (flecs::entity bulletEnt : matchedBullets) bulletEnt.destruct();
}

} // namespace Gravitaris
