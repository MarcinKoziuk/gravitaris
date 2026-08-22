#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/net/net-client.hpp>

namespace Gravitaris {

// Replicated Shots (see game/event/shot-stream.hpp): spawns this client's own
// copy of every unguided round anybody else fired. A round is not replicated
// as an entity any more -- it arrives once as a spawn instruction and is flown
// here, against the same physics the server is running it against.
//
// The rounds land in the client's own prediction registry rather than the
// mirror world, because that is where a round has to be to move: the mirror
// world has no PhysicsSystem behind it, and this client already flies its own
// predicted shots there (ClientPrediction::Step). Cosmetic, like those --
// zero damage, no DamageSystem on this client -- so what these are for is
// being seen, and CosmeticBulletDespawner is what takes them off the screen
// when they arrive somewhere.
//
// Walks NetClient's buffered snapshot history rather than only the newest
// snapshot, and dedupes on the shot's own seq: two snapshots can land between
// two client ticks, and the same snapshot can be read on two of them.
//
// No own-ship filter. The server already leaves a peer's own rounds out of
// that peer's snapshots (GatherSnapshot's suppressOwnedBy), which is what
// stops the shot being drawn twice -- a second check here could never fire,
// and a check that can never fire is the kind that quietly stops being true.
class RemoteShotApplier {
public:
    RemoteShotApplier(NetClient& netClient, EntitySpawner& entitySpawner);

    void Apply();

private:
    NetClient& m_netClient;
    EntitySpawner& m_entitySpawner;
    std::uint32_t m_lastAppliedSeq = 0;
};

} // namespace Gravitaris
