#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/event/shot-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

#include <gravitaris/cgame/net/remote-shot-applier.hpp>

namespace Gravitaris {

RemoteShotApplier::RemoteShotApplier(NetClient& netClient, EntitySpawner& entitySpawner)
        : m_netClient(netClient)
        , m_entitySpawner(entitySpawner)
{}

void RemoteShotApplier::Apply()
{
    for (const SnapshotData& snapshot : m_netClient.GetSnapshotHistory()) {
        for (const Shot& shot : snapshot.shots) {
            if (shot.seq <= m_lastAppliedSeq) continue;
            m_lastAppliedSeq = shot.seq;

            // SpawnBullet rather than SpawnRound: this copy is the drawing,
            // not the round. It resolves no hits, so it records no shot of its
            // own -- a client has nobody to tell.
            const flecs::entity round = m_entitySpawner.SpawnBullet(
                    shot.modelId, Vector2d{static_cast<double>(shot.pos.x()),
                                           static_cast<double>(shot.pos.y())},
                    Vector2d{static_cast<double>(shot.vel.x()), static_cast<double>(shot.vel.y())},
                    /*sensor=*/true, static_cast<double>(shot.rot));

            // The team is what makes it stop on the right hulls
            // (CosmeticBulletDespawner skips the shooter's own side); the
            // damage is zero because nothing here decides anything.
            round.emplace<Bullet>(static_cast<double>(shot.lifetimeSeconds), shot.team, 0.f,
                                  shot.ownerNetId);
        }
    }
}

} // namespace Gravitaris
