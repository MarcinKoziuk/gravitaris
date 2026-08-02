#include <optional>
#include <vector>

#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/resource/body-query.hpp>

#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/component/hit-outline.hpp>
#include <gravitaris/cgame/component/shield-flash.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>
#include <gravitaris/cgame/net/cosmetic-bullet-despawner.hpp>

namespace Gravitaris {

CosmeticBulletDespawner::CosmeticBulletDespawner(flecs::world& registry, flecs::world& mirrorWorld)
        : m_registry(registry)
        , m_mirrorWorld(mirrorWorld)
{}

void CosmeticBulletDespawner::CheckLocalHits()
{
    struct Hit {
        flecs::entity bullet;
        flecs::entity target;
        BodyHit where;
    };
    std::vector<Hit> hits;

    m_registry.each([&](flecs::entity bulletEnt, const Bullet& bullet, const Transform& bulletTransf) {
        if (bulletTransf.pos == bulletTransf.prevPos) return;

        std::optional<Hit> nearest;
        m_mirrorWorld.each([&](flecs::entity ship, const HitOutline& outline, const Transform& shipTransf) {
            // Same three rejections DamageSystem makes before it will call
            // anything a hit: no friendly fire, nothing that can't be damaged
            // (a planet, which a shot passes straight through -- and which has
            // no HitOutline here in the first place), and no shield element
            // that is spent or not the emitter this ship is carrying, so a
            // round threading the gap between two plates lands on the hull
            // behind them instead of stopping short.
            if (!ship.is_alive() || !ship.has<Damageable>()) return;
            const Team* shipTeam = ship.try_get<Team>();
            if (shipTeam && shipTeam->id == bullet.team) return;

            const std::optional<BodyHit> hit =
                    QueryBodySegment(*outline.body, shipTransf.pos, static_cast<double>(shipTransf.rot),
                                     shipTransf.scale, bulletTransf.prevPos, bulletTransf.pos,
                                     BULLET_QUERY_RADIUS);
            if (!hit) return;
            if (hit->shieldElement) {
                const ShipLoadout* loadout = ship.try_get<ShipLoadout>();
                if (!loadout || !ShieldElementLive(*loadout, *hit->shieldElement)) return;
            }
            if (nearest && nearest->where.alpha <= hit->alpha) return;

            nearest = Hit{bulletEnt, ship, *hit};
        });

        if (nearest) hits.push_back(*nearest);
    });

    for (const Hit& hit : hits) {
        const Magnum::Vector2 point{static_cast<float>(hit.where.point.x()),
                                    static_cast<float>(hit.where.point.y())};
        if (hit.where.shieldElement) {
            const std::int8_t plate = *hit.where.shieldElement == SHIELD_BUBBLE_ELEMENT
                    ? ShieldFlash::BUBBLE
                    : static_cast<std::int8_t>(*hit.where.shieldElement);
            HitFlashSystem::ApplyShieldHit(hit.target, point, plate);
        }
        else if (HitFlash* flash = hit.target.try_get_mut<HitFlash>()) {
            flash->amount = 1.f;
        }
        hit.bullet.destruct();
    }
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
