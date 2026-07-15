#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>

#include <gravitaris/cgame/resource/audio-clip.hpp>
#include <gravitaris/cgame/audio/audio-system.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// The listener hovers this far above the 2D plane: it keeps hard left/right
// panning gentle for nearby sources and doubles as the minimum distance.
constexpr float LISTENER_HEIGHT = 250.f;

constexpr std::size_t ONE_SHOT_POOL_SIZE = 8;

constexpr float LASER_GAIN = 0.5f;
constexpr float HIT_GAIN = 0.85f;
constexpr float THRUST_GAIN = 0.55f;

} // namespace

AudioSystem::AudioSystem(flecs::world& registry, ResourceLoader& resourceLoader)
        : m_registry(registry)
        , m_resourceLoader(resourceLoader)
{
    m_resourceLoader.OnCreate<AudioClip>().connect(&AudioSystem::HandleClipAdded, this);
    m_resourceLoader.OnDestroy<AudioClip>().connect(&AudioSystem::HandleClipRemoved, this);

    // Loaded unconditionally, before any backend exists: Update()/
    // PlayOneShotById dereference these clips' Id() every frame with no null
    // check, and SetBackendPreference() can enable audio later even if this
    // constructor's own backend attempt below fails -- both need these to
    // always be valid ResourcePtrs (real or Placeholder), never the
    // default-constructed empty state. A failed load silently substitutes
    // AudioClip::Placeholder() (an empty clip) -- same graceful-degrade
    // convention as Model/Body; that one clip just plays silence.
    //
    // HandleClipAdded (connected just above) fires synchronously here but
    // no-ops since m_backend doesn't exist yet -- uploaded explicitly below
    // once (if) one does.
    m_laserClip  = m_resourceLoader.Load<AudioClip>("sounds/laser-1.wav"_id);
    m_thrustClip = m_resourceLoader.Load<AudioClip>("sounds/thrust-1.wav"_id);
    m_hitClip    = m_resourceLoader.Load<AudioClip>("sounds/hit-1.wav"_id);

    m_backendPreference = ResolveAudioBackendPreference(m_backendPreference);
    m_backend = CreateAudioBackend(m_backendPreference);
    m_enabled = bool(m_backend);
    if (!m_enabled) {
        LOG(warning) << "[audio] no usable backend; audio disabled";
        return;
    }

    HandleClipAdded(*m_laserClip, m_laserClip.Id());
    HandleClipAdded(*m_thrustClip, m_thrustClip.Id());
    HandleClipAdded(*m_hitClip, m_hitClip.Id());

    AcquireVoicePool();

    m_bulletObserver = m_registry.observer<Bullet>()
            .event(flecs::OnSet)
            .each([this](flecs::entity ent, Bullet& bullet) {
                if (bullet.team == TeamId::None) return; // shrapnel, not a gunshot
                const Transform* transf = ent.try_get<Transform>();
                if (!transf) return;
                m_pendingShots.push_back(Vector2{static_cast<float>(transf->pos.x()),
                                                 static_cast<float>(transf->pos.y())});
            });
}

AudioSystem::~AudioSystem()
{
    m_resourceLoader.OnCreate<AudioClip>().disconnect(&AudioSystem::HandleClipAdded, this);
    m_resourceLoader.OnDestroy<AudioClip>().disconnect(&AudioSystem::HandleClipRemoved, this);

    // Same teardown reasoning as PhysicsSystem: the observer closes over
    // `this` and must not outlive it.
    if (m_bulletObserver) m_bulletObserver.destruct();

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

void AudioSystem::SetBackendPreference(AudioBackendPreference preference)
{
    if (m_backend) {
        for (const VoiceHandle& voice : m_oneShotPool) m_backend->ReleaseVoice(voice);
        for (auto& [id, loop] : m_thrusters) m_backend->ReleaseVoice(loop.voice);
        for (auto& [id, handle] : m_buffers) m_backend->ReleaseBuffer(handle);
    }
    m_oneShotPool.clear();
    m_thrusters.clear(); // still-thrusting entities re-acquire a fresh voice next Update()
    m_buffers.clear();

    m_backendPreference = ResolveAudioBackendPreference(preference);
    m_backend = CreateAudioBackend(m_backendPreference);
    m_enabled = bool(m_backend);
    if (!m_enabled) {
        LOG(warning) << "[audio] backend switch failed; audio disabled";
        return;
    }

    if (m_laserClip)  HandleClipAdded(*m_laserClip, m_laserClip.Id());
    if (m_thrustClip) HandleClipAdded(*m_thrustClip, m_thrustClip.Id());
    if (m_hitClip)    HandleClipAdded(*m_hitClip, m_hitClip.Id());

    AcquireVoicePool();
}

void AudioSystem::PlayOneShotById(id_t clipId, const Vector2& pos, float gain)
{
    if (m_oneShotPool.empty()) return;
    const auto it = m_buffers.find(clipId);
    if (it == m_buffers.end()) return;

    const VoiceHandle voice = m_oneShotPool[m_poolCursor];
    m_poolCursor = (m_poolCursor + 1) % m_oneShotPool.size();
    m_backend->PlayOneShot(voice, it->second, pos, gain);
}

void AudioSystem::Update(const Vector2& cameraPos)
{
    if (!m_enabled) return;

    m_backend->SetListenerPosition(cameraPos, LISTENER_HEIGHT);

    for (const Vector2& pos : m_pendingShots) {
        PlayOneShotById(m_laserClip.Id(), pos, LASER_GAIN);
    }
    m_pendingShots.clear();

    // Hit sounds on the rising edge of the damage flash (DamageSystem sets it
    // to exactly 1 on every hit and decays it the following ticks).
    m_lastFlashScratch.clear();
    m_registry.each([&](flecs::entity ent, const Transform& transf, const Damageable& dmg) {
        const auto it = m_lastFlash.find(ent.id());
        const float previous = it != m_lastFlash.end() ? it->second : 0.f;
        if (dmg.flashAmount >= 1.f && previous < 1.f) {
            PlayOneShotById(m_hitClip.Id(), Vector2{static_cast<float>(transf.pos.x()),
                                                    static_cast<float>(transf.pos.y())}, HIT_GAIN);
        }
        m_lastFlashScratch.emplace(ent.id(), dmg.flashAmount);
    });
    std::swap(m_lastFlash, m_lastFlashScratch);

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

    // Entities that stopped thrusting (or died) this frame.
    for (auto it = m_thrusters.begin(); it != m_thrusters.end();) {
        if (!it->second.seen) {
            m_backend->ReleaseVoice(it->second.voice);
            it = m_thrusters.erase(it);
        }
        else {
            ++it;
        }
    }
}

} // namespace Gravitaris
