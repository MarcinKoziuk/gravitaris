#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include <gravitaris/game/logging.hpp>

#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/net/snapshot.hpp>

#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>
#include <gravitaris/cgame/team-color.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_mirrorRenderer2(m_mirrorWorld, filesystem, m_resourceLoader)
    , m_snapshotApplier(m_mirrorWorld, m_resourceLoader)
    , m_starfieldRenderer(filesystem)
    , m_minimapRenderer(m_registry, filesystem)
    , m_audioSystem(m_registry, m_resourceLoader, m_eventQueue)
    , m_hitFlashSystem(m_registry, m_eventQueue, *m_entitySpawner)
    , m_cameraDirector(m_registry, Defaults::cameraZoom)
    , m_indicatorRenderer(m_registry, m_resourceLoader, m_modelRenderer2)
    , m_clientPrediction(m_registry, m_physicsSystem, *m_entitySpawner, m_eventQueue, m_resourceLoader)
    , m_autopilot(m_registry, m_physicsSystem)
{
    m_modelRenderer2.SetReferenceZoom(Defaults::cameraZoom);
    m_mirrorRenderer2.SetReferenceZoom(Defaults::cameraZoom);

    // This game's tuned default (Game's own default is 1 = unmodified): a
    // lighter ship reads better against the solar system's gravity wells.
    // Headless Games (sim-test) never call this, so their determinism is
    // unaffected by this specific value.
    SetShipWeightMultiplier(0.667f);

    // Loaded here (after both renderers' OnCreate<Model> observers exist, in
    // their own constructors above) so both m_modelRenderer2 and
    // m_mirrorRenderer2 bake it for SubmitPlanetOwnershipMarkers.
    m_teamMarkerModel = m_resourceLoader.Load<Model>("models/ui/team-marker"_id);
}

void CGame::SubmitPlanetOwnershipMarkers(flecs::world& world, ModelRenderer2& renderer)
{
    static constexpr float MARKER_WORLD_SIZE = 22.f;
    world.each([&](const Planet&, const Transform& t, const Team& team) {
        if (team.id == TeamId::None) return;
        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const Matrix3 transform =
                Matrix3::translation(pos) * Matrix3::scaling({MARKER_WORLD_SIZE, MARKER_WORLD_SIZE});
        renderer.SubmitOverlay(m_teamMarkerModel.Id(), transform, Magnum::Vector3{TeamColor(team.id)});
    });
}

void CGame::RenderMinimap()
{
    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) return; // between death and respawn (or MP: no snapshot yet): freeze the last frame

    const Camera& camera = m_cameraDirector.GetCamera();
    const Magnum::Vector2 playerPos{static_cast<float>(transform->pos.x()),
                                    static_cast<float>(transform->pos.y())};
    const Magnum::Vector2 viewHalfExtent = m_viewportSize / (2.f * std::max(camera.GetZoom(), 1e-3f));

    // Static, not player-centered: the solar system is laid out symmetrically
    // around the origin (see Game::Start), so that's the whole map's center.
    // In MP, everything but the own ship lives in m_mirrorWorld (see
    // m_netClient's field comment) -- sweep it too so remote ships/planets
    // show up exactly like single-player's real registry entities do.
    m_minimapRenderer.Render(Magnum::Vector2{0.f, 0.f}, playerPos, camera.GetPosition(), viewHalfExtent,
                             m_netClient ? &m_mirrorWorld : nullptr);
}

void CGame::ConnectToServer(const std::string& wsUrl)
{
    m_netTransport = std::make_unique<WebRtcTransport>(WebRtcTransport::Role::Offerer);
    m_netClient = std::make_unique<NetClient>(*m_netTransport, "gravitaris-client");
    m_netTransport->ConnectSignaling(wsUrl);
}

void CGame::TickNetClient(const ControlFlags& flags)
{
    if (!m_netClient->IsWelcomed()) return;

    if (m_clientPrediction.HasOwnShip()) {
        // The ship this client is predicting must still be the one the
        // server has for it. It won't be if it died (crashed into a sun --
        // ClientPrediction has no collision damage of its own, so this is
        // the only way the local ship finds out) or was just replaced by a
        // respawn under a new NetId (GetYourShipNetId() changes the instant
        // the re-welcome packet arrives, ahead of any snapshot reflecting
        // the new ship). Either way, an authoritative snapshot no longer
        // containing this NetId means the local prediction is stale and
        // must be dropped -- the spawn gate below then waits for a fresh
        // snapshot with the (possibly new) NetId, same as the very first
        // spawn.
        const std::optional<SnapshotData>& current = m_netClient->GetLatestSnapshot();
        const bool stillPresent = current && std::any_of(current->entities.begin(), current->entities.end(),
                [&](const EntityState& e) { return e.netId == m_netClient->GetYourShipNetId(); });
        if (!stillPresent) {
            m_clientPrediction.DestroyOwnShip();
            m_player.reset();
            m_visualCorrectionOffset = {};
        }
    }

    if (!m_clientPrediction.HasOwnShip()) {
        // Wait for a snapshot that actually confirms this NetId and where
        // it is, so the predicted ship spawns at the real position instead
        // of popping in from the origin once the first reconciliation runs.
        const std::optional<SnapshotData>& snapshot = m_netClient->GetLatestSnapshot();
        if (!snapshot) return;
        const auto it = std::find_if(snapshot->entities.begin(), snapshot->entities.end(),
                                     [&](const EntityState& e) { return e.netId == m_netClient->GetYourShipNetId(); });
        if (it == snapshot->entities.end()) return;

        m_clientPrediction.SpawnOwnShip(
                it->modelId, Vector2d{static_cast<double>(it->pos.x()), static_cast<double>(it->pos.y())},
                m_netClient->GetYourTeam());
        m_player = m_clientPrediction.GetOwnShip();
        m_nextPredictedTick = m_netClient->EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS;
    }

    // Drift guard on the free-running tick counter. It advances once per
    // *executed* tick, but the browser doesn't guarantee ticks execute: rAF
    // throttling on a backgrounded tab, GC hitches, and tickEvent's own
    // step cap (which discards the excess backlog rather than replaying it)
    // all lose wall-clock time that this counter never sees. Every lost
    // tick is permanent backward drift vs. the server's wall-clock-paced
    // step, and once the drift exceeds INPUT_LEAD_TICKS, every input this
    // client sends is stamped in the server's past -- InputSystem drops it
    // as stale, repeat-last-command latches the last consumed flags, and
    // the ship spins/freezes server-side forever while local prediction
    // (and silently-failing reconciliation) keep this client feeling fine.
    // Re-seed from the wall-clock estimate when the drift grows past the
    // estimate's own jitter; small drift is left alone so consecutive
    // predicted ticks normally stay exactly one PHYSICS_DELTA apart (see
    // m_nextPredictedTick's field comment for why that matters).
    {
        const std::uint64_t target = m_netClient->EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS;
        static constexpr std::uint64_t RESYNC_THRESHOLD_TICKS = 5; // ~83ms; > estimate jitter, < perceptible lag
        const std::uint64_t drift = target > m_nextPredictedTick ? target - m_nextPredictedTick
                                                                 : m_nextPredictedTick - target;
        if (drift > RESYNC_THRESHOLD_TICKS) {
            LOG(info) << "net: predicted-tick drift of " << drift << " ticks (throttled tab?), resyncing "
                      << m_nextPredictedTick << " -> " << target;
            m_nextPredictedTick = target;
        }
    }

    const std::uint64_t tick = m_nextPredictedTick++;
    m_netClient->SendInput(tick, flags);

    if (const std::optional<SnapshotData>& snapshot = m_netClient->GetLatestSnapshot()) {
        m_clientPrediction.Step(tick, flags, snapshot->entities);
        m_bulletLifetimeSystem.Update(PHYSICS_DELTA);
    }
}

void CGame::ReconcileOwnShipIfNeeded()
{
    if (!m_clientPrediction.HasOwnShip()) return;

    const std::optional<SnapshotData>& snapshot = m_netClient->GetLatestSnapshot();
    if (!snapshot || snapshot->tick <= m_lastReconciledTick) return;
    m_lastReconciledTick = snapshot->tick;

    const auto it = std::find_if(snapshot->entities.begin(), snapshot->entities.end(),
                                 [&](const EntityState& e) { return e.netId == m_netClient->GetYourShipNetId(); });
    if (it == snapshot->entities.end()) return;

    const std::optional<Vector2d> preCorrection = m_clientPrediction.Reconcile(snapshot->tick, *it, snapshot->entities);
    if (!preCorrection) return;

    // The ship's real Transform now holds the corrected position; blend the
    // visual gap out via the camera (see m_visualCorrectionOffset's doc)
    // instead of touching it.
    const Transform& t = m_clientPrediction.GetOwnShip().get<Transform>();
    const Magnum::Vector2 correctedPos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
    const Magnum::Vector2 preCorrectionPos{static_cast<float>(preCorrection->x()), static_cast<float>(preCorrection->y())};
    m_visualCorrectionOffset += preCorrectionPos - correctedPos;

    // Diagnostic (2026-07-19): correlate correction magnitude/frequency
    // against NetServer's "peer N input timed out"/stale-input logs to
    // check whether corrections this large are caused by input arriving
    // past its stamped tick (INPUT_LEAD_TICKS too tight for real RTT/
    // jitter -- dropped server-side, repeat-last-command diverges from
    // what was predicted) rather than ordinary f32/quantization noise.
    LOG(trace) << "net: reconciled tick " << snapshot->tick << ", correction magnitude "
              << (preCorrectionPos - correctedPos).length() << " world units";
}

void CGame::ApplyRemoteEvents()
{
    const std::uint32_t yourShipNetId = m_netClient->GetYourShipNetId();

    for (const SnapshotData& snapshot : m_netClient->GetSnapshotHistory()) {
        for (const GameEvent& event : snapshot.events) {
            if (event.seq <= m_lastAppliedRemoteEventSeq) continue;
            m_lastAppliedRemoteEventSeq = event.seq;

            if (event.type == GameEventType::BulletFired && event.sourceNetId == yourShipNetId) continue;

            m_eventQueue.Emit(event.type, flecs::entity{}, event.pos, event.param);

            if (event.type != GameEventType::Impact && event.type != GameEventType::LandingCrash) continue;

            const flecs::entity target = (event.sourceNetId == yourShipNetId)
                    ? GetPlayer().value_or(flecs::entity{})
                    : m_snapshotApplier.EntityForNetId(event.sourceNetId);
            if (!target.is_alive()) continue; // e.g. the hit killed it this tick

            if (HitFlash* flash = target.try_get_mut<HitFlash>()) {
                flash->amount = 1.f;
            }
        }
    }
}

void CGame::RenderNetClient(float dtSeconds)
{
    m_netClient->Update();
    ApplyRemoteEvents();

    const std::uint64_t estimatedServerTick = m_netClient->EstimateCurrentServerTick();
    const auto interpDelayTicks =
            static_cast<std::uint64_t>(m_interpDelaySeconds * static_cast<float>(m_netClient->GetTickRate()));
    const std::uint64_t renderTick =
            estimatedServerTick > interpDelayTicks ? estimatedServerTick - interpDelayTicks : 0;
    m_lastEstimatedServerTick = estimatedServerTick;
    m_lastRenderTick = renderTick;

    ReconcileOwnShipIfNeeded();

    // Remote entities only (the own ship is real m_registry state now,
    // Phase 5) via Phase 4 interpolation into the mirror world.
    if (const std::optional<SnapshotData> interpolated =
                SnapshotInterpolator::Compute(m_netClient->GetSnapshotHistory(), renderTick,
                                              m_netClient->GetYourShipNetId(),
                                              static_cast<float>(m_netClient->GetTickRate()), m_interpParams)) {
        m_snapshotApplier.Apply(*interpolated, dtSeconds);
    }

    // Blend out any reconciliation snap over ~100ms by decaying the offset
    // *before* camera framing runs, then feeding it in as an already
    // -smoothed position override -- not by nudging the camera's own output
    // position afterward (the previous approach). That ordering mattered:
    // dead-zone follow/enemy-framing/planet-framing all read "where is the
    // player" once, at the top of CameraDirector::Update, so a correction
    // applied only after the fact left every one of them reacting to the
    // raw, still-discontinuous snap this frame -- a fast, repeating jitter
    // in position framing that had nothing to do with network lag in synced
    // enemy/planet data (that's smooth by design; see SnapshotInterpolator).
    // The real simulated Transform itself is never touched here -- it must
    // stay exactly correct for the next predicted tick to build on.
    static constexpr float CORRECTION_SMOOTH_SECONDS = 0.1f;
    m_visualCorrectionOffset *= std::exp(-dtSeconds / CORRECTION_SMOOTH_SECONDS);

    Magnum::Vector2 smoothedPlayerPos;
    if (const std::optional<flecs::entity> player = GetPlayer()) {
        const Transform& t = player->get<Transform>();
        smoothedPlayerPos = Magnum::Vector2{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())}
                + m_visualCorrectionOffset;
    }

    // Real single-player camera logic against m_registry -- works for the
    // own ship (dead-zone follow, dynamic zoom); m_mirrorWorld is swept
    // alongside it for enemy/planet framing, since every entity but the own
    // ship lives there in this mode (see m_netClient's field comment).
    m_cameraDirector.Update(GetPlayer(), m_viewportSize, dtSeconds, &m_mirrorWorld, smoothedPlayerPos);
    Camera& camera = m_cameraDirector.GetCamera();

    // Decays HitFlash on both worlds; ApplyRemoteEvents above is what sets
    // it (own ship directly, everyone else via the mirror world), since
    // m_hitFlashSystem's own event consumption never finds a match here --
    // the events it sees via m_eventQueue were re-emitted with no source
    // entity (see ApplyRemoteEvents), same as ClientPrediction's own local
    // BulletFired ones.
    m_hitFlashSystem.Update(dtSeconds);
    HitFlashSystem::Decay(m_mirrorWorld, dtSeconds);

    // Own-ship one-shots (BulletLifetimeSystem-tracked cosmetic bullets emit
    // BulletFired via m_clientPrediction) and thruster loop, via the same
    // event-driven path single-player uses -- m_registry only ever holds the
    // own ship in this mode, so nothing else's audio can leak in.
    m_audioSystem.Update(camera.GetPosition());

    m_starfieldRenderer.SetZoom(camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(camera.GetPosition());
    m_starfieldRenderer.Render();

    SubmitPlanetOwnershipMarkers(m_mirrorWorld, m_mirrorRenderer2);
    m_mirrorRenderer2.SetZoom(camera.GetZoom());
    m_mirrorRenderer2.SetCameraPosition(camera.GetPosition());
    m_mirrorRenderer2.SetLineWidth(m_lineWidthPixels);
    m_mirrorRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
    m_mirrorRenderer2.Render(0.0);

    // Own ship: real local sim, drawn through the same renderer/world
    // single-player uses -- m_registry holds nothing else in this mode.
    // ModelRenderer2::Render draws Transform::pos directly with no
    // interpolation of its own (its `delta` parameter is unused -- unlike a
    // typical fixed-tick renderer, there's no prevPos/pos blend here at
    // all), so a reconciliation snap would make the ship itself visibly
    // teleport even though the camera above already glides past it via
    // `smoothedPlayerPos`. Draw at that same smoothed position instead: save
    // the real Transform::pos, overwrite it just for this one render call,
    // then restore it immediately after -- the actual simulated state (what
    // the next predicted tick builds on) is never touched, only one frame's
    // worth of what gets drawn.
    m_modelRenderer2.SetZoom(camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
    if (const std::optional<flecs::entity> player = GetPlayer()) {
        Transform& t = player->get_mut<Transform>();
        const Vector2d realPos = t.pos;
        t.pos = Vector2d{smoothedPlayerPos};
        m_modelRenderer2.Render(0.0);
        t.pos = realPos;
    } else {
        m_modelRenderer2.Render(0.0);
    }
}

void CGame::Render(double delta)
{
    // Real wall-clock dt for the camera director (Render's `delta` is a fixed-
    // step interpolation fraction, not seconds). Clamped so a stall doesn't
    // snap the camera. Computed up front (not just in the local-sim path
    // below) since the net-client path needs it too, for its own zoom easing.
    const auto now = std::chrono::steady_clock::now();
    float dtSeconds = 1.f / 60.f;
    if (m_cameraTimeValid) {
        dtSeconds = std::chrono::duration<float>(now - m_lastCameraTime).count();
        dtSeconds = std::clamp(dtSeconds, 0.f, 0.1f);
    }
    m_lastCameraTime = now;
    m_cameraTimeValid = true;

    if (m_netClient) {
        RenderNetClient(dtSeconds);
        return;
    }

    m_cameraDirector.Update(GetPlayer(), m_viewportSize, dtSeconds);
    m_hitFlashSystem.Update(dtSeconds);

    const Camera& camera = m_cameraDirector.GetCamera();
    m_simpleModelRenderer.SetZoom(camera.GetZoom());
    m_simpleModelRenderer.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetZoom(camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);

    m_starfieldRenderer.SetZoom(camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(camera.GetPosition());

    // Overlays ride m_modelRenderer2's instanced draw (which clears them at
    // the end of its Render), so only submit them when that renderer actually
    // runs this frame -- otherwise the overlay scratch grows unboundedly.
    if (m_activeRenderer == RendererKind::Baked) {
        m_indicatorRenderer.Update(GetPlayer(), camera.GetPosition(), camera.GetZoom(), m_viewportSize, m_pixelScale);
        SubmitPlanetOwnershipMarkers(m_registry, m_modelRenderer2);
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Starfield");
        m_starfieldRenderer.Render();
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Rendering");
        // Renderers are mutually exclusive; the debug UI picks the active one.
        switch (m_activeRenderer) {
            case RendererKind::Simple:
                m_simpleModelRenderer.Render(delta);
                break;
            case RendererKind::Baked:
                m_modelRenderer2.Render(delta);
                break;
            case RendererKind::Mirror: {
                // Round-trip the live sim through the full snapshot path
                // (world -> bytes -> parse -> apply), then draw the mirror
                // world instead of the real one (networking-plan 2.5).
                {
                    ScopedPerfTimer mirrorTimer(m_perfMonitor, "Snapshot Mirror");
                    m_snapshotScratch.Clear();
                    WriteSnapshot(m_registry, m_eventQueue, GetStep(), m_mirrorEventCursor,
                                  m_snapshotScratch);
                    ByteReader reader(m_snapshotScratch.Data(), m_snapshotScratch.Size());
                    SnapshotData snapshot;
                    if (ReadSnapshot(reader, snapshot)) {
                        m_snapshotApplier.Apply(snapshot);
                        if (!snapshot.events.empty()) {
                            m_mirrorEventCursor = snapshot.events.back().seq;
                        }
                    }
                }
                m_mirrorRenderer2.SetZoom(camera.GetZoom());
                m_mirrorRenderer2.SetCameraPosition(camera.GetPosition());
                m_mirrorRenderer2.SetLineWidth(m_lineWidthPixels);
                m_mirrorRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
                m_mirrorRenderer2.Render(delta);
                break;
            }
        }
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Audio");
        m_audioSystem.Update(camera.GetPosition());
    }
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

} // namespace Gravitaris
