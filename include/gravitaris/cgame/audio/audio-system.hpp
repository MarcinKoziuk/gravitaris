#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Audio/Context.h>
#include <Magnum/Audio/Buffer.h>
#include <Magnum/Audio/Source.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Vector2;

// Client-side positional audio (OpenAL via Magnum::Audio). The 2D world maps
// to OpenAL's 3D space with the listener hovering above the camera position,
// so left/right panning and distance attenuation fall out of the plane
// geometry. Event sources:
//  - gunshots: flecs observer on Bullet creation (frag shrapnel, TeamId::None,
//    is skipped -- it's debris, not a gun),
//  - hits: rising edge of Damageable::flashAmount (set to 1 on every hit),
//  - thrust: looping per-entity source while Controls::thrustForward is held.
class AudioSystem {
private:
    struct ThrusterLoop {
        Magnum::Audio::Source source;
        bool seen = false;
    };

    flecs::world& m_registry;

    Magnum::Audio::Context m_context;
    bool m_enabled = false;

    // Optional because Audio::Buffer's constructor calls alGenBuffers, which
    // needs a current AL context -- these must be created after tryCreate(),
    // not as eagerly-constructed members.
    std::optional<Magnum::Audio::Buffer> m_laserBuffer;
    std::optional<Magnum::Audio::Buffer> m_thrustBuffer;
    std::optional<Magnum::Audio::Buffer> m_hitBuffer;

    // Round-robin pool for one-shot sounds (shots, hits).
    std::vector<Magnum::Audio::Source> m_oneShotPool;
    std::size_t m_poolCursor = 0;

    std::unordered_map<flecs::entity_t, ThrusterLoop> m_thrusters;

    // Rising-edge tracking for Damageable::flashAmount; double-buffered so
    // entries for dead entities don't accumulate.
    std::unordered_map<flecs::entity_t, float> m_lastFlash;
    std::unordered_map<flecs::entity_t, float> m_lastFlashScratch;

    flecs::observer m_bulletObserver;
    std::vector<Vector2> m_pendingShots;

    bool LoadBuffer(IFilesystem& filesystem, const char* path,
                    std::optional<Magnum::Audio::Buffer>& buffer);

    void PlayOneShot(Magnum::Audio::Buffer& buffer, const Vector2& pos, float gain);

public:
    AudioSystem(flecs::world& registry, IFilesystem& filesystem);

    ~AudioSystem();

    // Drives everything: listener follows the camera, queued shots play,
    // hit flashes trigger, thruster loops start/stop/move. Call once per
    // rendered frame.
    void Update(const Vector2& cameraPos);
};

} // namespace Gravitaris
