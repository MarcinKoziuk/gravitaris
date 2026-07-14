#include <cstring>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Audio/BufferFormat.h>
#include <Magnum/Audio/Renderer.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>

#include <gravitaris/cgame/audio/audio-system.hpp>

namespace Gravitaris {

using namespace Magnum;

// The listener hovers this far above the 2D plane: it keeps hard left/right
// panning gentle for nearby sources and doubles as the minimum distance.
static constexpr float LISTENER_HEIGHT = 250.f;

// Distance at which a source plays at full gain; attenuation follows OpenAL's
// default inverse-clamped model beyond it. World scale: AI engage range 500.
static constexpr float REFERENCE_DISTANCE = 250.f;
static constexpr float MAX_DISTANCE = 3000.f;

static constexpr std::size_t ONE_SHOT_POOL_SIZE = 8;

static constexpr float LASER_GAIN = 0.5f;
static constexpr float HIT_GAIN = 0.85f;
static constexpr float THRUST_GAIN = 0.55f;

namespace {

// Only what our own generated assets need: canonical RIFF/WAVE, PCM 16-bit,
// mono or stereo (stereo is downmixed -- OpenAL only pans mono sources).
// Anything fancier should go through a real decoder (dr_wav/stb_vorbis).
struct WavData {
    std::vector<std::int16_t> samples; // mono
    std::uint32_t sampleRate = 0;
};

bool ParseWav(const std::vector<std::uint8_t>& bytes, WavData* out)
{
    auto u16 = [&](std::size_t o) { return std::uint16_t(bytes[o] | bytes[o + 1] << 8); };
    auto u32 = [&](std::size_t o) {
        return std::uint32_t(bytes[o] | bytes[o + 1] << 8 | bytes[o + 2] << 16 | bytes[o + 3] << 24);
    };

    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    std::uint16_t channels = 0, bitsPerSample = 0, format = 0;
    std::uint32_t sampleRate = 0;
    std::size_t dataOffset = 0, dataSize = 0;

    // Walk chunks; fmt may be followed by LIST/fact/etc. before data.
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint32_t chunkSize = u32(pos + 4);
        if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0 && pos + 24 <= bytes.size()) {
            format = u16(pos + 8);
            channels = u16(pos + 10);
            sampleRate = u32(pos + 12);
            bitsPerSample = u16(pos + 22);
        }
        else if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize = chunkSize;
        }
        pos += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
    }

    if (format != 1 || bitsPerSample != 16 || channels < 1 || channels > 2
        || dataOffset == 0 || dataOffset + dataSize > bytes.size()) {
        return false;
    }

    const auto* src = reinterpret_cast<const std::int16_t*>(bytes.data() + dataOffset);
    const std::size_t frames = dataSize / (2 * channels);

    out->sampleRate = sampleRate;
    out->samples.resize(frames);
    if (channels == 1) {
        std::memcpy(out->samples.data(), src, frames * 2);
    }
    else {
        for (std::size_t i = 0; i < frames; ++i) {
            out->samples[i] = static_cast<std::int16_t>((int(src[i * 2]) + int(src[i * 2 + 1])) / 2);
        }
    }
    return true;
}

void ConfigurePositional(Audio::Source& source)
{
    source.setReferenceDistance(REFERENCE_DISTANCE)
          .setRolloffFactor(1.f)
          .setMaxDistance(MAX_DISTANCE);
}

} // namespace

AudioSystem::AudioSystem(flecs::world& registry, IFilesystem& filesystem)
        : m_registry(registry)
        , m_context(Magnum::NoCreate)
{
    if (!m_context.tryCreate(Audio::Context::Configuration{})) {
        LOG(warning) << "[audio] no OpenAL device; audio disabled";
        return;
    }
    m_enabled = true;

    m_enabled &= LoadBuffer(filesystem, "sounds/laser-1.wav", m_laserBuffer);
    m_enabled &= LoadBuffer(filesystem, "sounds/thrust-1.wav", m_thrustBuffer);
    m_enabled &= LoadBuffer(filesystem, "sounds/hit-1.wav", m_hitBuffer);
    if (!m_enabled) {
        LOG(warning) << "[audio] sound assets failed to load; audio disabled";
        return;
    }

    m_oneShotPool.reserve(ONE_SHOT_POOL_SIZE);
    for (std::size_t i = 0; i < ONE_SHOT_POOL_SIZE; ++i) {
        ConfigurePositional(m_oneShotPool.emplace_back());
    }

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
    // Same teardown reasoning as PhysicsSystem: the observer closes over
    // `this` and must not outlive it.
    if (m_bulletObserver) m_bulletObserver.destruct();
}

bool AudioSystem::LoadBuffer(IFilesystem& filesystem, const char* path,
                             std::optional<Audio::Buffer>& buffer)
{
    std::vector<std::uint8_t> bytes;
    if (!filesystem.ReadBytes(std::string{path}, &bytes)) {
        LOG(error) << "[audio] could not read " << path;
        return false;
    }

    WavData wav;
    if (!ParseWav(bytes, &wav)) {
        LOG(error) << "[audio] unsupported wav (want PCM16 mono/stereo): " << path;
        return false;
    }

    buffer.emplace();
    buffer->setData(Audio::BufferFormat::Mono16,
                   Corrade::Containers::ArrayView<const void>{
                           wav.samples.data(), wav.samples.size() * sizeof(std::int16_t)},
                   static_cast<ALsizei>(wav.sampleRate));
    return true;
}

void AudioSystem::PlayOneShot(Audio::Buffer& buffer, const Vector2& pos, float gain)
{
    Audio::Source& source = m_oneShotPool[m_poolCursor];
    m_poolCursor = (m_poolCursor + 1) % m_oneShotPool.size();

    source.stop();
    source.setBuffer(&buffer)
          .setLooping(false)
          .setGain(gain)
          .setPosition(Vector3{pos.x(), pos.y(), 0.f})
          .play();
}

void AudioSystem::Update(const Vector2& cameraPos)
{
    if (!m_enabled) return;

    Audio::Renderer::setListenerPosition(Vector3{cameraPos.x(), cameraPos.y(), LISTENER_HEIGHT});

    for (const Vector2& pos : m_pendingShots) {
        PlayOneShot(*m_laserBuffer, pos, LASER_GAIN);
    }
    m_pendingShots.clear();

    // Hit sounds on the rising edge of the damage flash (DamageSystem sets it
    // to exactly 1 on every hit and decays it the following ticks).
    m_lastFlashScratch.clear();
    m_registry.each([&](flecs::entity ent, const Transform& transf, const Damageable& dmg) {
        const auto it = m_lastFlash.find(ent.id());
        const float previous = it != m_lastFlash.end() ? it->second : 0.f;
        if (dmg.flashAmount >= 1.f && previous < 1.f) {
            PlayOneShot(*m_hitBuffer, Vector2{static_cast<float>(transf.pos.x()),
                                              static_cast<float>(transf.pos.y())}, HIT_GAIN);
        }
        m_lastFlashScratch.emplace(ent.id(), dmg.flashAmount);
    });
    std::swap(m_lastFlash, m_lastFlashScratch);

    // Thruster loops: one looping source per entity holding thrust.
    for (auto& [id, loop] : m_thrusters) loop.seen = false;

    m_registry.each([&](flecs::entity ent, const Transform& transf, const Controls& controls) {
        if (!controls.actionFlags.thrustForward) return;

        auto [it, inserted] = m_thrusters.try_emplace(ent.id());
        ThrusterLoop& loop = it->second;
        loop.seen = true;
        if (inserted) {
            ConfigurePositional(loop.source);
            loop.source.setBuffer(&*m_thrustBuffer)
                       .setLooping(true)
                       .setGain(THRUST_GAIN);
        }
        loop.source.setPosition(Vector3{static_cast<float>(transf.pos.x()),
                                        static_cast<float>(transf.pos.y()), 0.f});
        if (loop.source.state() != Audio::Source::State::Playing) {
            loop.source.play();
        }
    });

    // Entities that stopped thrusting (or died) this frame.
    for (auto it = m_thrusters.begin(); it != m_thrusters.end();) {
        if (!it->second.seen) {
            it = m_thrusters.erase(it); // Source dtor stops playback
        }
        else {
            ++it;
        }
    }
}

} // namespace Gravitaris
