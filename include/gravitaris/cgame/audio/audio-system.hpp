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
// One-shots (hits, explosions) and gunfire come from the sim's GameEventQueue
// via this system's own cursor -- the same stream a network client will
// receive, so audio needs no knowledge of how the sim produced them (it
// previously inferred shots from a Bullet-creation observer and hits from a
// flashAmount edge, both of which break under replication). Thrust stays
// state-driven: a looping per-entity voice while Controls::thrustForward is
// held -- continuous state, not an event.
class AudioSystem {
private:
    // Which held sound a looping voice is: one ship can be running its engine
    // and burning its beams at the same time, and those are two voices rather
    // than one interrupting the other.
    enum class LoopKind : std::uint8_t {
        Thruster,
        Beam,
    };

    // Held loops are keyed by world as well as entity: a net client keeps its
    // own ship in the real world and everyone else in the mirror one, and the
    // two number their entities independently.
    struct LoopKey {
        const flecs::world_t* world = nullptr;
        flecs::entity_t entity = 0;
        LoopKind kind = LoopKind::Thruster;

        bool operator==(const LoopKey& other) const
        { return world == other.world && entity == other.entity && kind == other.kind; }
    };

    struct LoopKeyHash {
        std::size_t operator()(const LoopKey& key) const
        {
            const auto world = reinterpret_cast<std::uintptr_t>(key.world);
            return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(world)
                                              ^ (key.entity * 0x9e3779b97f4a7c15ull)
                                              ^ static_cast<std::uint64_t>(key.kind));
        }
    };

    struct HeldLoop {
        VoiceHandle voice;
        bool seen = false;
        // Which clip the voice is currently looping. The overburn is a
        // different engine note, so crossing that boundary restarts the loop
        // on the other buffer rather than leaving the stock rumble running.
        id_t clipId = 0;
        // What it is playing at, so the fade-out ramps from the right place
        // whichever kind of loop this is.
        float gain = 0.f;
        // Consecutive frames not seen running. Kept below the release grace
        // period, a real stop is distinguished from the on/off "bang-bang"
        // burn pattern flight control produces while cruising -- see the
        // grace constant in the .cpp.
        std::uint32_t offFrames = 0;
    };

    // A looping voice whose owner stopped thrusting: ramped down over a few
    // frames before release, because hard-stopping a rumble mid-waveform
    // clicks.
    struct FadingVoice {
        VoiceHandle voice;
        float gain = 0.f;
    };

    // One shooter firing one weapon. Keyed by both because a ship launching a
    // missile while its guns are running is two sounds, not one interrupting
    // the other.
    struct MuzzleKey {
        std::uint32_t shooter = 0; // the shooter's NetId
        id_t clip = 0;

        bool operator==(const MuzzleKey& other) const
        { return shooter == other.shooter && clip == other.clip; }
    };

    struct MuzzleKeyHash {
        std::size_t operator()(const MuzzleKey& key) const
        {
            return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(key.shooter)
                                              ^ (static_cast<std::uint64_t>(key.clip)
                                                 * 0x9e3779b97f4a7c15ull));
        }
    };

    // A muzzle's own voice, held between rounds. Retired once it has been
    // quiet long enough that the gun has plainly stopped -- see
    // MUZZLE_IDLE_FRAMES.
    struct MuzzleVoice {
        VoiceHandle voice;
        std::uint32_t idleFrames = 0;
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
    // The same loop with the injectors open, played instead of m_thrustClip
    // while a ship is boosting (Controls::boosting).
    ResourcePtr<const AudioClip> m_thrustBoostClip;
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

    std::unordered_map<LoopKey, HeldLoop, LoopKeyHash> m_loops;
    std::vector<FadingVoice> m_fadingVoices;
    std::unordered_map<MuzzleKey, MuzzleVoice, MuzzleKeyHash> m_muzzles;

    void HandleClipAdded(const AudioClip& clip, id_t id);
    void HandleClipRemoved(const AudioClip& clip, id_t id);

    void PlayOneShotById(id_t clipId, const Vector2& pos, float gain);

    // A round out of one muzzle. Unlike PlayOneShotById this does not take a
    // fresh voice per round: a held trigger restarts the muzzle's own voice, so
    // sustained fire is one sound repeating at the gun's cadence rather than a
    // dozen copies of the clip overlapping each other out of the pool.
    void PlayShot(std::uint32_t shooter, id_t clipId, const Vector2& pos, float gain);

    // Hands back the voice of any muzzle that has gone quiet.
    void SweepMuzzles();

    void AcquireVoicePool();

    // Starts/moves a looping voice for every entity in `world` holding
    // thrust, and marks it seen. Update() calls this once per world it was
    // given before retiring whatever went unseen.
    void SweepThrusters(flecs::world& world);

    // The same, for every entity whose beams the capacitor is actually feeding
    // (Controls::laserFiring). Held exactly as thrust is, so it shares the
    // grace period and the fade rather than clicking on and off with the
    // trigger.
    void SweepBeams(flecs::world& world);

    // Starts or re-points one held loop, and reports it seen this frame.
    void HoldLoop(flecs::world& world, flecs::entity ent, LoopKind kind, id_t clipId,
                  const Vector2& pos, float gain);

public:
    AudioSystem(flecs::world& registry, ResourceLoader& resourceLoader,
                const GameEventQueue& eventQueue, const UpgradeCatalog& catalog);

    ~AudioSystem();

    [[nodiscard]] const char* GetBackendName() const { return m_backend ? m_backend->Name() : "none"; }
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    // Drives everything: listener follows the camera, queued shots play,
    // hit flashes trigger, thruster loops start/stop/move. Call once per
    // rendered frame.
    //
    // `mirrorWorld` is the net client's replicated world, which holds every
    // ship but your own -- without it nobody else's thrusters can be heard.
    void Update(const Vector2& cameraPos, flecs::world* mirrorWorld = nullptr);

    // A chat line arrived. Played at the listener, so it sits centred and at
    // full gain wherever the camera happens to be -- chat is UI, not something
    // that happened somewhere in the world. Called directly rather than driven
    // off the event queue: chat is not sim state and never replays.
    void PlayChatBlip();
};

} // namespace Gravitaris
