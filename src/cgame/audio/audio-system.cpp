#include <algorithm>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>

#include <gravitaris/cgame/resource/audio-clip.hpp>
#include <gravitaris/cgame/audio/backend/miniaudio-backend.hpp>
#include <gravitaris/cgame/audio/audio-system.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// The listener hovers this far above the 2D plane: it keeps hard left/right
// panning gentle for nearby sources and doubles as the minimum distance.
constexpr float LISTENER_HEIGHT = 250.f;

// Sized so that simultaneous hit/explosion one-shots (~0.15s each) from a busy
// fight rarely exhaust it: stealing a voice that's still mid-clip hard-cuts a
// near-full-scale waveform, which is an audible click. Gunfire does not draw
// on this at all -- a muzzle holds its own voice (see PlayShot).
constexpr std::size_t ONE_SHOT_POOL_SIZE = 24;

// Released thruster loops ramp to silence over this many frames instead of
// hard-stopping mid-waveform (Update() has no dt; at 60 fps this is ~100 ms).
constexpr float THRUST_FADE_FRAMES = 6.f;

// A thrust loop keeps playing through gaps this short before it's handed to
// the fade-out list. FlightController's min-burn/min-coast hysteresis makes
// a normal cruise toggle Controls::thrustForward on and off every
// 80-400 ms (see FlightControllerParams); without this grace period every
// one of those coasts tore the voice down and restarted it from the top,
// which is audible as the loop "bursting" instead of running continuously.
// A genuine stop (arrival, death, disengage) still fades out, just a beat
// later.
constexpr std::uint32_t THRUST_RELEASE_GRACE_FRAMES = 24;

// How long a muzzle keeps its own voice after its last round. Long enough to
// cover the slowest weapon's cadence several times over (a missile rack cycles
// every 26-36 ticks) so a steady rate of fire never gives the voice up
// mid-burst, short enough that a wing that has broken off is not still holding
// one voice each.
constexpr std::uint32_t MUZZLE_IDLE_FRAMES = 90;

constexpr float HIT_GAIN = 0.85f;
constexpr float SHIELD_HIT_GAIN = 0.35f;
constexpr float RESEARCH_GAIN = 0.8f;
// Off until research earns an announcement again; the sidebar countdown and
// bar it went with are commented out of hud.rml for the same reason.
constexpr bool RESEARCH_CHIME = false;
constexpr float CHAT_GAIN = 0.6f;
constexpr float THRUST_GAIN = 0.55f;
// On top of the emitter's own authored gain. Held rather than struck, and a
// ship burning a beam is usually also under thrust, so the two have to sit
// together -- which the per-shot figure a weapon carries is not scaled for.
constexpr float BEAM_GAIN_SCALE = 0.8f;

} // namespace

AudioSystem::AudioSystem(flecs::world& registry, ResourceLoader& resourceLoader,
                         const GameEventQueue& eventQueue, const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_resourceLoader(resourceLoader)
        , m_eventQueue(eventQueue)
        , m_catalog(catalog)
{
    m_resourceLoader.OnCreate<AudioClip>().connect(&AudioSystem::HandleClipAdded, this);
    m_resourceLoader.OnDestroy<AudioClip>().connect(&AudioSystem::HandleClipRemoved, this);

    // Loaded unconditionally, before any backend exists: Update()/
    // PlayOneShotById dereference these clips' Id() every frame with no null
    // check, so they need to be valid ResourcePtrs (real or Placeholder),
    // never the default-constructed empty state, even if the backend attempt
    // below fails and audio stays disabled. A failed load silently substitutes
    // AudioClip::Placeholder() (an empty clip) -- same graceful-degrade
    // convention as Model/Body; that one clip just plays silence.
    //
    // HandleClipAdded (connected just above) fires synchronously here but
    // no-ops since m_backend doesn't exist yet -- uploaded explicitly below
    // once (if) one does.
    m_thrustClip = m_resourceLoader.Load<AudioClip>("sounds/thrust-1.wav"_id);
    m_thrustBoostClip = m_resourceLoader.Load<AudioClip>("sounds/thrust-boost-1.wav"_id);
    m_hitClip    = m_resourceLoader.Load<AudioClip>("sounds/hit-1.wav"_id);
    // Two clips, not one: a bubble absorbing a round and a metal plate
    // taking one are different events to the player, and the shield type is
    // not recoverable here (a replicated event carries no source entity).
    m_bubbleClip = m_resourceLoader.Load<AudioClip>("sounds/shield-bubble-1.wav"_id);
    m_platingClip = m_resourceLoader.Load<AudioClip>("sounds/shield-plating-1.wav"_id);
    m_researchClip = m_resourceLoader.Load<AudioClip>("sounds/research-1.wav"_id);
    m_chatClip = m_resourceLoader.Load<AudioClip>("sounds/chat-1.wav"_id);

    // One clip per distinct sound the weapon table names. Deduplicated: the
    // whole gatling line shares a file, and loading it once per tier would
    // upload the same buffer four times.
    for (const WeaponDef& weapon : m_catalog.Weapons()) {
        if (weapon.soundId == 0) continue;
        const auto already = std::find_if(m_weaponClips.begin(), m_weaponClips.end(),
                                          [&](const ResourcePtr<const AudioClip>& clip) {
                                              return clip.Id() == weapon.soundId;
                                          });
        if (already != m_weaponClips.end()) continue;
        m_weaponClips.push_back(m_resourceLoader.Load<AudioClip>(weapon.soundId));
    }

    auto miniaudio = std::make_unique<MiniaudioBackend>();
    if (miniaudio->Init()) {
        LOG(info) << "[audio] using backend: " << miniaudio->Name();
        m_backend = std::move(miniaudio);
    }
    m_enabled = bool(m_backend);
    if (!m_enabled) {
        LOG(warning) << "[audio] miniaudio backend failed to initialize; audio disabled";
        return;
    }

    HandleClipAdded(*m_thrustClip, m_thrustClip.Id());
    HandleClipAdded(*m_thrustBoostClip, m_thrustBoostClip.Id());
    HandleClipAdded(*m_hitClip, m_hitClip.Id());
    HandleClipAdded(*m_bubbleClip, m_bubbleClip.Id());
    HandleClipAdded(*m_platingClip, m_platingClip.Id());
    HandleClipAdded(*m_researchClip, m_researchClip.Id());
    HandleClipAdded(*m_chatClip, m_chatClip.Id());
    for (const ResourcePtr<const AudioClip>& clip : m_weaponClips) HandleClipAdded(*clip, clip.Id());

    AcquireVoicePool();
}

AudioSystem::~AudioSystem()
{
    m_resourceLoader.OnCreate<AudioClip>().disconnect(&AudioSystem::HandleClipAdded, this);
    m_resourceLoader.OnDestroy<AudioClip>().disconnect(&AudioSystem::HandleClipRemoved, this);

    // m_backend's own destructor tears down every voice/buffer it still owns.
}

void AudioSystem::HandleClipAdded(const AudioClip& clip, id_t id)
{
    if (!m_backend) return;
    m_buffers[id] = m_backend->UploadBuffer(clip.GetSamples().data(), clip.GetSamples().size(), clip.GetSampleRate());
}

void AudioSystem::HandleClipRemoved(const AudioClip&, id_t id)
{
    const auto it = m_buffers.find(id);
    if (it == m_buffers.end()) return;
    if (m_backend) m_backend->ReleaseBuffer(it->second);
    m_buffers.erase(it);
}

void AudioSystem::AcquireVoicePool()
{
    m_oneShotPool.clear();
    m_oneShotPool.reserve(ONE_SHOT_POOL_SIZE);
    for (std::size_t i = 0; i < ONE_SHOT_POOL_SIZE; ++i) {
        m_oneShotPool.push_back(m_backend->AcquireVoice());
    }
}

void AudioSystem::PlayOneShotById(id_t clipId, const Vector2& pos, float gain)
{
    if (m_oneShotPool.empty()) return;
    const auto it = m_buffers.find(clipId);
    if (it == m_buffers.end()) return;

    // Prefer an idle voice; steal one that's still playing (a click -- see
    // ONE_SHOT_POOL_SIZE) only when the whole pool is busy.
    std::size_t chosen = m_poolCursor;
    for (std::size_t i = 0; i < m_oneShotPool.size(); ++i) {
        const std::size_t candidate = (m_poolCursor + i) % m_oneShotPool.size();
        if (!m_backend->IsVoicePlaying(m_oneShotPool[candidate])) {
            chosen = candidate;
            break;
        }
    }
    m_poolCursor = (chosen + 1) % m_oneShotPool.size();
    m_backend->PlayOneShot(m_oneShotPool[chosen], it->second, pos, gain);
}

void AudioSystem::PlayShot(std::uint32_t shooter, id_t clipId, const Vector2& pos, float gain)
{
    // No NetId behind the shot -- nothing to hold a voice against, so it goes
    // through the pool the way a hit or an explosion does.
    if (shooter == 0) {
        PlayOneShotById(clipId, pos, gain);
        return;
    }

    const auto bufferIt = m_buffers.find(clipId);
    if (bufferIt == m_buffers.end()) return;

    auto [it, inserted] = m_muzzles.try_emplace(MuzzleKey{shooter, clipId});
    MuzzleVoice& muzzle = it->second;
    if (inserted) muzzle.voice = m_backend->AcquireVoice();
    muzzle.idleFrames = 0;

    // Rewinds rather than layering: the backend replays a voice already holding
    // this clip from its first frame (MiniaudioBackend::RebindVoice), which is
    // what turns a stream of rounds into one running gun.
    m_backend->PlayOneShot(muzzle.voice, bufferIt->second, pos, gain);
}

void AudioSystem::SweepMuzzles()
{
    for (auto it = m_muzzles.begin(); it != m_muzzles.end();) {
        // A one-shot ends on its own, so there is nothing to fade here -- the
        // voice is only held so the next round can reuse it.
        if (++it->second.idleFrames >= MUZZLE_IDLE_FRAMES
                && !m_backend->IsVoicePlaying(it->second.voice)) {
            m_backend->ReleaseVoice(it->second.voice);
            it = m_muzzles.erase(it);
        }
        else {
            ++it;
        }
    }
}

void AudioSystem::HoldLoop(flecs::world& world, flecs::entity ent, LoopKind kind, id_t clipId,
                           const Vector2& pos, float gain)
{
    const auto bufferIt = m_buffers.find(clipId);

    auto [it, inserted] = m_loops.try_emplace(LoopKey{world.c_ptr(), ent.id(), kind});
    HeldLoop& loop = it->second;
    loop.seen = true;
    loop.offFrames = 0;
    loop.gain = gain;

    const bool restart = inserted || loop.clipId != clipId || !m_backend->IsVoicePlaying(loop.voice);

    if (inserted) loop.voice = m_backend->AcquireVoice();
    else m_backend->SetVoicePosition(loop.voice, pos);

    if (restart && bufferIt != m_buffers.end()) {
        m_backend->PlayLooping(loop.voice, bufferIt->second, pos, gain);
        loop.clipId = clipId;
    }
}

void AudioSystem::SweepThrusters(flecs::world& world)
{
    world.each([&](flecs::entity ent, const Transform& transf, const Controls& controls) {
        if (!controls.actionFlags.thrustForward && !controls.boosting) return;

        // A ship running the overburn is a different engine note. Picked per
        // entity per frame rather than once for the sweep: a wing can have
        // some of its ships boosting and the rest cruising.
        const id_t wanted = controls.boosting ? m_thrustBoostClip.Id() : m_thrustClip.Id();
        const Vector2 pos{static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())};

        HoldLoop(world, ent, LoopKind::Thruster, wanted, pos, THRUST_GAIN);
    });
}

void AudioSystem::SweepBeams(flecs::world& world)
{
    world.each([&](flecs::entity ent, const Transform& transf, const Controls& controls,
                   const ShipLoadout& loadout) {
        // What the bank actually granted, not what the trigger asked for -- a
        // dry capacitor should go quiet, which is most of how a pilot hears
        // that they have run out.
        if (!controls.laserFiring) return;

        // The emitter's own clip, the way a gun's round carries its own: a
        // rank that sounds heavier is then a data edit. Resolved off the
        // replicated loadout, so a remote ship's beam sounds right here
        // without this side ever seeing its owner's purchases.
        const ShipStats stats = m_catalog.ResolveStats(loadout.levels);
        if (!stats.laser || !stats.laser->IsBeam() || stats.laser->soundId == 0) return;

        const Vector2 pos{static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())};
        HoldLoop(world, ent, LoopKind::Beam, stats.laser->soundId, pos,
                 stats.laser->soundGain * BEAM_GAIN_SCALE);
    });
}

void AudioSystem::PlayChatBlip()
{
    if (!m_enabled) return;
    PlayOneShotById(m_chatClip.Id(), m_listenerPos, CHAT_GAIN);
}

void AudioSystem::Update(const Vector2& cameraPos, flecs::world* mirrorWorld)
{
    if (!m_enabled) return;

    m_listenerPos = cameraPos;
    m_backend->SetListenerPosition(cameraPos, LISTENER_HEIGHT);

    // One-shots straight off the sim's event stream. Frag shrapnel is
    // naturally silent -- DeathSystem spawns it without a BulletFired event.
    // Explosion/LandingCrash borrow the hit sound until they get their own
    // clips (docs/networking-plan.md 1.4).
    m_eventCursor = m_eventQueue.ConsumeSince(m_eventCursor, [&](const GameEvent& event) {
        switch (event.type) {
            case GameEventType::BulletFired:
                // param is the weapon's id, so a weapon brings its own sound
                // with it -- nothing here knows one gun from another.
                if (const WeaponDef* weapon = m_catalog.FindWeapon(event.param)) {
                    PlayShot(event.sourceNetId, weapon->soundId, event.pos, weapon->soundGain);
                }
                break;
            case GameEventType::ResearchComplete:
                if (RESEARCH_CHIME) {
                    PlayOneShotById(m_researchClip.Id(), event.pos, RESEARCH_GAIN);
                }
                break;
            case GameEventType::ShieldHit:
                // Quieter and thinner than a hull hit -- the tell is that the
                // hull did NOT take it.
                PlayOneShotById(m_bubbleClip.Id(), event.pos, SHIELD_HIT_GAIN);
                break;
            case GameEventType::PlatingHit:
                PlayOneShotById(m_platingClip.Id(), event.pos, SHIELD_HIT_GAIN);
                break;
            case GameEventType::Impact:
            case GameEventType::LandingCrash:
            case GameEventType::Explosion:
                PlayOneShotById(m_hitClip.Id(), event.pos, HIT_GAIN);
                break;
            default:
                break; // no sound assigned (PlanetClaimed etc.)
        }
    });

    SweepMuzzles();

    // Held loops: one looping voice per entity per held sound. A net client's
    // own ship is the only one in m_registry -- everybody else lives in the
    // mirror world, so both are swept.
    for (auto& [key, loop] : m_loops) loop.seen = false;

    SweepThrusters(m_registry);
    SweepBeams(m_registry);
    if (mirrorWorld) {
        SweepThrusters(*mirrorWorld);
        SweepBeams(*mirrorWorld);
    }

    // Entities that stopped (or died) this frame: give the gap a short grace
    // period (below) before handing the voice to the fade-out list rather than
    // releasing (= hard-stopping) it here.
    for (auto it = m_loops.begin(); it != m_loops.end();) {
        if (!it->second.seen && ++it->second.offFrames >= THRUST_RELEASE_GRACE_FRAMES) {
            m_fadingVoices.push_back({it->second.voice, it->second.gain});
            it = m_loops.erase(it);
        }
        else {
            ++it;
        }
    }

    const float fadeStep = THRUST_GAIN / THRUST_FADE_FRAMES;
    for (auto it = m_fadingVoices.begin(); it != m_fadingVoices.end();) {
        it->gain -= fadeStep;
        if (it->gain <= 0.f) {
            m_backend->ReleaseVoice(it->voice);
            it = m_fadingVoices.erase(it);
        }
        else {
            m_backend->SetVoiceGain(it->voice, it->gain);
            ++it;
        }
    }
}

} // namespace Gravitaris
