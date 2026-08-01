#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

#include <gravitaris/cgame/fwd.hpp>
#include <gravitaris/cgame/audio/audio-backend.hpp>

namespace Gravitaris {

using Magnum::Vector2;

// Client-side positional audio, backend-agnostic (see IAudioBackend and
// docs/adr/0003-audio-backend.md). The 2D world maps to the backend's 3D
// space with the listener hovering above the camera position, so left/right
// panning and distance attenuation fall out of the plane geometry. Sound
// assets are AudioClip resources, loaded reactively through ResourceLoader
// (mirrors ModelRenderer2's OnCreate<Model>/OnDestroy<Model> pattern) rather
// than read/parsed ad hoc.
//
// One-shots (gunshots, hits, explosions) come from the sim's GameEventQueue
// via this system's own cursor -- the same stream a network client will
// receive, so audio needs no knowledge of how the sim produced them (it
// previously inferred shots from a Bullet-creation observer and hits from a
// flashAmount edge, both of which break under replication). Thrust stays
// state-driven: a looping per-entity voice while Controls::thrustForward is
// held -- continuous state, not an event.
class AudioSystem {
private:
    struct ThrusterLoop {
        VoiceHandle voice;
        bool seen = false;
    };

    // A looping voice whose owner stopped thrusting: ramped down over a few
    // frames before release, because hard-stopping a rumble mid-waveform
    // clicks.
    struct FadingVoice {
        VoiceHandle voice;
        float gain = 0.f;
    };

    flecs::world& m_registry;
    ResourceLoader& m_resourceLoader;
    const GameEventQueue& m_eventQueue;
    const UpgradeCatalog& m_catalog;
    std::uint32_t m_eventCursor = 0;

    std::unique_ptr<IAudioBackend> m_backend;
    bool m_enabled = false;

    // Reactive cache: AudioClip resource id -> uploaded buffer handle in the
    // CURRENT backend. Populated by HandleClipAdded (ResourceLoader's
    // OnCreate<AudioClip>), erased by HandleClipRemoved (OnDestroy). Mirrors
    // ModelRenderer2::m_baked.
    std::unordered_map<id_t, SoundBufferHandle> m_buffers;

    // Kept alive (and used to re-upload on a backend switch -- OnCreate only
    // fires once per resource lifetime, not every time the backend changes).
    // Only three named clips exist today; if/when sounds become dynamically
    // spawned like Models, generalize this into an id_t-keyed map instead.
    ResourcePtr<const AudioClip> m_thrustClip;
    ResourcePtr<const AudioClip> m_hitClip;
    // Borrows the stock gun's clip at a lower gain until a shield hit gets
    // its own.
    ResourcePtr<const AudioClip> m_bubbleClip;
    ResourcePtr<const AudioClip> m_platingClip;
    // The Lab's chime when a faction's research bar fills.
    ResourcePtr<const AudioClip> m_researchClip;
    // A received chat line. Not a sim event -- see PlayChatBlip.
    ResourcePtr<const AudioClip> m_chatClip;
    // One per distinct sound named by a [[weapon]] in data/upgrades.toml.
    // GameEventType::BulletFired's param is the weapon's id, so a remote
    // shooter's rounds sound right without this side ever seeing its loadout,
    // and a new weapon's sound needs no code here.
    std::vector<ResourcePtr<const AudioClip>> m_weaponClips;

    // Where Update() last put the listener; PlayChatBlip's own position, so a
    // UI sound doesn't pan or attenuate.
    Vector2 m_listenerPos{0.f, 0.f};

    std::vector<VoiceHandle> m_oneShotPool;
    std::size_t m_poolCursor = 0;

    std::unordered_map<flecs::entity_t, ThrusterLoop> m_thrusters;
    std::vector<FadingVoice> m_fadingVoices;

    void HandleClipAdded(const AudioClip& clip, id_t id);
    void HandleClipRemoved(const AudioClip& clip, id_t id);

    void PlayOneShotById(id_t clipId, const Vector2& pos, float gain);

    void AcquireVoicePool();

public:
    AudioSystem(flecs::world& registry, ResourceLoader& resourceLoader,
                const GameEventQueue& eventQueue, const UpgradeCatalog& catalog);

    ~AudioSystem();

    [[nodiscard]] const char* GetBackendName() const { return m_backend ? m_backend->Name() : "none"; }
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    // Drives everything: listener follows the camera, queued shots play,
    // hit flashes trigger, thruster loops start/stop/move. Call once per
    // rendered frame.
    void Update(const Vector2& cameraPos);

    // A chat line arrived. Played at the listener, so it sits centred and at
    // full gain wherever the camera happens to be -- chat is UI, not something
    // that happened somewhere in the world. Called directly rather than driven
    // off the event queue: chat is not sim state and never replays.
    void PlayChatBlip();
};

} // namespace Gravitaris
