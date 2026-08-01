#include <algorithm>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>
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

// Sized so that simultaneous laser/hit one-shots (~0.15s each) from several
// AI ships at full fire rate rarely exhaust it: stealing a voice that's still
// mid-clip hard-cuts a near-full-scale waveform, which is an audible click.
constexpr std::size_t ONE_SHOT_POOL_SIZE = 24;

// Released thruster loops ramp to silence over this many frames instead of
// hard-stopping mid-waveform (Update() has no dt; at 60 fps this is ~100 ms).
constexpr float THRUST_FADE_FRAMES = 6.f;

constexpr float HIT_GAIN = 0.85f;
constexpr float SHIELD_HIT_GAIN = 0.35f;
constexpr float RESEARCH_GAIN = 0.8f;
constexpr float THRUST_GAIN = 0.55f;

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
    m_hitClip    = m_resourceLoader.Load<AudioClip>("sounds/hit-1.wav"_id);
    // Two clips, not one: a bubble absorbing a round and a metal plate
    // taking one are different events to the player, and the shield type is
    // not recoverable here (a replicated event carries no source entity).
    m_bubbleClip = m_resourceLoader.Load<AudioClip>("sounds/shield-bubble-1.wav"_id);
    m_platingClip = m_resourceLoader.Load<AudioClip>("sounds/shield-plating-1.wav"_id);
    m_researchClip = m_resourceLoader.Load<AudioClip>("sounds/research-1.wav"_id);

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
    HandleClipAdded(*m_hitClip, m_hitClip.Id());
    HandleClipAdded(*m_bubbleClip, m_bubbleClip.Id());
    HandleClipAdded(*m_platingClip, m_platingClip.Id());
    HandleClipAdded(*m_researchClip, m_researchClip.Id());
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

void AudioSystem::Update(const Vector2& cameraPos)
{
    if (!m_enabled) return;

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
                    PlayOneShotById(weapon->soundId, event.pos, weapon->soundGain);
                }
                break;
            case GameEventType::ResearchComplete:
                PlayOneShotById(m_researchClip.Id(), event.pos, RESEARCH_GAIN);
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

    // Thruster loops: one looping voice per entity holding thrust.
    for (auto& [id, loop] : m_thrusters) loop.seen = false;

    const auto thrustBufferIt = m_buffers.find(m_thrustClip.Id());

    m_registry.each([&](flecs::entity ent, const Transform& transf, const Controls& controls) {
        if (!controls.actionFlags.thrustForward) return;

        auto [it, inserted] = m_thrusters.try_emplace(ent.id());
        ThrusterLoop& loop = it->second;
        loop.seen = true;

        const Vector2 pos{static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())};
        if (inserted) {
            loop.voice = m_backend->AcquireVoice();
            if (thrustBufferIt != m_buffers.end()) {
                m_backend->PlayLooping(loop.voice, thrustBufferIt->second, pos, THRUST_GAIN);
            }
        }
        else {
            m_backend->SetVoicePosition(loop.voice, pos);
            if (!m_backend->IsVoicePlaying(loop.voice) && thrustBufferIt != m_buffers.end()) {
                m_backend->PlayLooping(loop.voice, thrustBufferIt->second, pos, THRUST_GAIN);
            }
        }
    });

    // Entities that stopped thrusting (or died) this frame: hand the voice to
    // the fade-out list rather than releasing (= hard-stopping) it here.
    for (auto it = m_thrusters.begin(); it != m_thrusters.end();) {
        if (!it->second.seen) {
            m_fadingVoices.push_back({it->second.voice, THRUST_GAIN});
            it = m_thrusters.erase(it);
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
