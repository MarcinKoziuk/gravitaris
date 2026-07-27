#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gravitaris/game/logging.hpp>

#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/net/snapshot.hpp>

#include <gravitaris/cgame/fx/hit-flash-system.hpp>
#include <gravitaris/cgame/team-color.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

namespace {

// How far behind the estimated server tick remote entities render (see
// CGame::m_interpDelaySeconds) -- smooths jitter at the cost of latency.
// Fractional throughout, and fed by the smoothed clock rather than the
// integer one: see SnapshotInterpolator::Compute's `renderTick` doc comment.
double ComputeRenderTick(double estimatedServerTick, float interpDelaySeconds, std::uint32_t tickRate)
{
    return std::max(estimatedServerTick - static_cast<double>(interpDelaySeconds) * tickRate, 0.0);
}

} // namespace

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_mirrorRenderer2(m_mirrorWorld, filesystem, m_resourceLoader)
    , m_snapshotApplier(m_mirrorWorld, m_resourceLoader)
    , m_starfieldRenderer(filesystem)
    , m_minimapRenderer(filesystem)
    , m_audioSystem(m_registry, m_resourceLoader, m_eventQueue)
    , m_hitFlashSystem(m_registry, m_eventQueue, *m_entitySpawner)
    , m_cameraDirector(Defaults::cameraZoom)
    , m_indicatorRenderer(m_resourceLoader)
    , m_clientPrediction(m_registry, m_physicsSystem, *m_entitySpawner, m_eventQueue, m_resourceLoader)
    , m_cosmeticBulletDespawner(m_registry, m_mirrorWorld)
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

SceneView CGame::CurrentSceneView()
{
    if (m_netClient) return SceneView{m_registry, &m_mirrorWorld, &m_mirrorRenderer2};
    return SceneView{m_registry, nullptr, &m_modelRenderer2};
}

std::optional<flecs::entity> CGame::CameraSubject()
{
    if (m_spectateTarget.is_alive()) return m_spectateTarget;
    return GetPlayer();
}

std::optional<float> CGame::GetHullFraction()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const Damageable* damageable = subject->try_get<Damageable>();
    if (!damageable || damageable->maxHp <= 0.f) return std::nullopt;

    return std::clamp(damageable->hp / damageable->maxHp, 0.f, 1.f);
}

std::optional<int> CGame::GetMissileAmmo()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const ShipLoadout* loadout = subject->try_get<ShipLoadout>();
    if (!loadout) return std::nullopt; // a structure/planet being spectated has no rack

    return static_cast<int>(loadout->missileAmmo);
}

void CGame::CycleSpectate(int direction)
{
    // NetId order, so the roster reads the same however flecs happens to
    // iterate and whichever world an entity lives in.
    std::vector<std::pair<std::uint32_t, flecs::entity>> units;
    CurrentSceneView().Each([&](flecs::entity ent, const Transform&, const Team&, const Damageable&,
                                const NetId& netId) {
        if (ent.has<Structure>() || ent.has<Planet>()) return;
        units.emplace_back(netId.value, ent);
    });
    if (units.empty()) return;
    std::sort(units.begin(), units.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const std::optional<flecs::entity> subject = CameraSubject();
    std::size_t index = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (subject && units[i].second == *subject) index = i;
    }

    const int count = static_cast<int>(units.size());
    const int next = ((static_cast<int>(index) + direction) % count + count) % count;

    const std::optional<flecs::entity> player = GetPlayer();
    m_spectateTarget = (player && units[next].second == *player) ? flecs::entity() : units[next].second;
}

void CGame::SubmitPlanetOwnershipMarkers(const SceneView& view)
{
    if (!view.overlays) return;

    static constexpr float MARKER_WORLD_SIZE = 22.f;
    view.Each([&](const Planet&, const Transform& t, const Team& team) {
        if (team.id == TeamId::None) return;
        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const Matrix3 transform =
                Matrix3::translation(pos) * Matrix3::scaling({MARKER_WORLD_SIZE, MARKER_WORLD_SIZE});
        view.overlays->SubmitOverlay(m_teamMarkerModel.Id(), transform, Magnum::Vector3{TeamColor(team.id)});
    });
}

void CGame::RenderMinimap()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    const Transform* transform = subject ? subject->try_get<Transform>() : nullptr;
    if (!transform) return; // between death and respawn (or MP: no snapshot yet): freeze the last frame

    const Camera& camera = m_cameraDirector.GetCamera();
    const Magnum::Vector2 subjectPos{static_cast<float>(transform->pos.x()),
                                     static_cast<float>(transform->pos.y())};
    const Magnum::Vector2 viewHalfExtent = m_viewportSize / (2.f * std::max(camera.GetZoom(), 1e-3f));

    // In MP, everything but the own ship lives in m_mirrorWorld (see
    // m_netClient's field comment) -- sweep it too so remote ships/planets
    // show up exactly like single-player's real registry entities do.
    const SceneView view = CurrentSceneView();
    m_minimapRenderer.Render(view, MinimapCenter(), subjectPos, camera.GetPosition(), viewHalfExtent);
}

void CGame::LookAtMapPoint(const Magnum::Vector2& normalized)
{
    const float worldRadius = std::max(m_minimapRenderer.GetParams().worldRadius, 1.f);
    m_cameraDirector.LookAt(MinimapCenter() + normalized * worldRadius);
}

void CGame::ConnectToServer(const std::string& wsUrl, TeamId requestedTeam)
{
    m_netTransport = std::make_unique<WebRtcTransport>(WebRtcTransport::Role::Offerer);
    m_simulatedTransport = std::make_unique<SimulatedNetTransport>(*m_netTransport);
    m_netClient = std::make_unique<NetClient>(*m_simulatedTransport, "gravitaris-client");
    m_netClient->SetRequestedTeam(requestedTeam);
    m_ownShipSync.emplace(m_clientPrediction, *m_netClient, m_predictedTickClock);
    m_remoteEventApplier.emplace(*m_netClient, m_eventQueue, m_cosmeticBulletDespawner);
    m_netTransport->ConnectSignaling(wsUrl);
}

void CGame::TickNetClient(const ControlFlags& flags)
{
    if (!m_netClient->IsWelcomed()) return;

    if (m_ownShipSync->DropIfStale()) {
        m_player.reset();
    }
    if (const std::optional<flecs::entity> spawned = m_ownShipSync->SpawnIfConfirmed()) {
        m_player = *spawned;
    }
    if (!m_clientPrediction.HasOwnShip()) return;

    // Smoothed clock, not the integer estimate: the target is compared
    // against a free-running counter a fraction of a tick at a time now (see
    // PredictedTickClock::Advance), so feeding it the raw staircase would
    // read its steps as drift.
    const double target =
            m_netClient->EstimateCurrentServerTickF() + static_cast<double>(m_netClient->GetInputLeadTicks());
    const PredictedTickClock::AdvanceResult advance = m_predictedTickClock.Advance(target);
    if (advance.skip) {
        // Running ahead of the target -- usually because the input lead was
        // just lowered under us. Give the tick back by simply not existing
        // this call: no step, no input stamped, one frame of the own ship
        // holding still while wall clock closes the gap.
        ++m_netDiagnostics.tickSkipCount;
        return;
    }
    if (advance.resyncDrift) {
        // A lost tick is permanent backward drift vs. the server's
        // wall-clock-paced step (see PredictedTickClock's own doc comment),
        // and once it exceeds the input-lead window, every input this
        // client sends is stamped in the server's past -- InputSystem drops
        // it as stale, repeat-last-command latches the last consumed flags,
        // and the ship spins/freezes server-side forever while local
        // prediction (and silently-failing reconciliation) keep this client
        // feeling fine.
        LOG(info) << "net: predicted-tick drift of " << *advance.resyncDrift << " ticks (throttled tab?), resyncing "
                  << " -> " << target;
        ++m_netDiagnostics.resyncEventCount;
        m_netDiagnostics.lastResyncDriftTicks = *advance.resyncDrift;
        m_netDiagnostics.driftHistory.Record(static_cast<float>(*advance.resyncDrift));
    }

    const std::uint64_t tick = advance.tick;
    m_netClient->SendInput(tick, flags);

    if (const std::optional<SnapshotData>& snapshot = m_netClient->GetLatestSnapshot()) {
        m_clientPrediction.Step(tick, flags, snapshot->entities, snapshot->tick, m_netClient->GetYourShipNetId());
        m_bulletLifetimeSystem.Update(PHYSICS_DELTA);

        // The own ship is predicted locally, so it never passes through
        // SnapshotApplier -- its ammo has to be copied off the wire here or
        // the sidebar would report the count it spawned with forever.
        // Missile fire itself is not predicted, so this arriving a round trip
        // late is the whole story of the readout's latency.
        const std::uint32_t yourShipNetId = m_netClient->GetYourShipNetId();
        if (const std::optional<flecs::entity> player = GetPlayer(); player && yourShipNetId != 0) {
            for (const EntityState& state : snapshot->entities) {
                if (state.netId != yourShipNetId) continue;
                if (ShipLoadout* loadout = player->try_get_mut<ShipLoadout>()) {
                    loadout->missileAmmo = state.missileAmmo;
                }
                break;
            }
        }
    }
}

void CGame::ReconcileOwnShipIfNeeded()
{
    const std::optional<OwnShipSync::ReconcileResult> result = m_ownShipSync->ReconcileIfNeeded();
    if (!result) return;

    // Diagnostic (2026-07-19): correlate correction magnitude/frequency
    // against NetServer's "peer N input timed out"/stale-input logs to
    // check whether corrections this large are caused by input arriving
    // past its stamped tick (INPUT_LEAD_TICKS too tight for real RTT/
    // jitter -- dropped server-side, repeat-last-command diverges from
    // what was predicted) rather than ordinary f32/quantization noise.
    LOG(trace) << "net: reconciled tick " << result->tick << ", correction magnitude "
              << result->correctionMagnitude << " world units";
    m_netDiagnostics.correctionHistory.Record(result->correctionMagnitude);
    // Recorded on the same "genuinely new snapshot" gate as the correction
    // above, so this lines up 1:1 with it in the Net debug tab's graphs --
    // a real network gap shows up here; a local main-thread stall shows up
    // as a drift/resync event above with this staying flat instead.
    m_netDiagnostics.snapshotIntervalHistory.Record(m_netClient->GetLastSnapshotIntervalMs());
}

void CGame::ApplyRemoteEvents()
{
    const std::uint32_t yourShipNetId = m_netClient->GetYourShipNetId();
    m_remoteEventApplier->Apply([&](std::uint32_t sourceNetId) -> flecs::entity {
        return sourceNetId == yourShipNetId ? GetPlayer().value_or(flecs::entity{})
                                            : m_snapshotApplier.EntityForNetId(sourceNetId);
    });
}

void CGame::RenderNetClient(float dtSeconds, double tickFraction)
{
    m_netClient->Update();
    ApplyRemoteEvents();

    const std::uint64_t estimatedServerTick = m_netClient->EstimateCurrentServerTick();
    const double renderTick = ComputeRenderTick(m_netClient->EstimateCurrentServerTickF(), m_interpDelaySeconds,
                                                m_netClient->GetTickRate());
    m_lastEstimatedServerTick = estimatedServerTick;
    m_lastRenderTick = renderTick;

    ReconcileOwnShipIfNeeded();

    // Planets must be rendered against the same tick ClientPrediction's
    // gravity/collision proxies last used (TickNetClient's `tick`, i.e.
    // `m_predictedTickClock.Current() - 1` -- the most recent tick actually
    // stepped), not `renderTick` (delayed behind the estimated server tick
    // by the interpolation-delay setting) -- see SnapshotInterpolator::
    // Compute's `planetTick` doc comment. Falls back to `renderTick`
    // (nullopt) before the own ship exists yet, when nothing has
    // stepped/synced a proxy to desync from in the first place.
    // Plus `tickFraction`, so bodies re-derived from it move continuously
    // instead of stepping once per simulated tick while the ship and camera
    // move on real time -- see the `planetTick` doc comment.
    const std::optional<double> planetTick =
            m_clientPrediction.HasOwnShip()
                    ? std::optional<double>(static_cast<double>(m_predictedTickClock.Current() - 1)
                                            + std::clamp(tickFraction, 0.0, 1.0))
                    : std::nullopt;

    // Remote entities only (the own ship is real m_registry state now,
    // Phase 5) via Phase 4 interpolation into the mirror world.
    if (const std::optional<SnapshotData> interpolated =
                SnapshotInterpolator::Compute(m_netClient->GetSnapshotHistory(), renderTick,
                                              m_netClient->GetYourShipNetId(),
                                              static_cast<float>(m_netClient->GetTickRate()), m_interpParams,
                                              planetTick)) {
        m_snapshotApplier.Apply(*interpolated, dtSeconds);
    }

    // Right after the mirror world's ship positions were just refreshed,
    // not from TickNetClient's fixed-step loop (moved here 2026-07-21):
    // that loop can run several ticks back-to-back after a stall (catch-up,
    // MAX_STEPS_PER_FRAME in gravitaris.cpp), but the mirror world only
    // updates once per render frame -- checking mid-catch-up compared the
    // bullet's *already-advanced* position against an up-to-several-ticks
    // -stale enemy position, silently under-triggering hits exactly when
    // real network jitter (this client's actual bug report) made the
    // catch-up happen in the first place. Both are now as fresh as they
    // ever get, at the same instant.
    m_cosmeticBulletDespawner.CheckLocalHits();

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
    m_ownShipSync->DecayCorrection(dtSeconds);

    // Own ship: rendered `tickFraction` of the way from the previous
    // predicted tick to the current one, not at the raw current one. Its
    // simulated position only advances on whole fixed-step ticks, while
    // everything it's judged against on screen moves on real time -- the
    // camera eases with wall-clock dt, and remote entities now run on a
    // continuous clock (SnapshotInterpolator). Drawing whole ticks against
    // those makes the ship stall on a frame that ran no tick and lurch on
    // one that ran two, by up to a tick of travel each way, which is why it
    // scales with speed. Costs up to one tick (~17ms) of render latency on
    // the own ship, the standard fixed-step interpolation trade.
    Magnum::Vector2 smoothedPlayerPos;
    if (const std::optional<flecs::entity> player = GetPlayer()) {
        const Transform& t = player->get<Transform>();
        const Vector2d rendered = t.prevPos + (t.pos - t.prevPos) * std::clamp(tickFraction, 0.0, 1.0);
        smoothedPlayerPos = Magnum::Vector2{static_cast<float>(rendered.x()), static_cast<float>(rendered.y())}
                + m_ownShipSync->GetCorrectionOffset();
    }

    // Real single-player camera logic against m_registry -- works for the
    // own ship (dead-zone follow, dynamic zoom); m_mirrorWorld is swept
    // alongside it for enemy/planet framing, since every entity but the own
    // ship lives there in this mode (see m_netClient's field comment).
    const SceneView view = CurrentSceneView();
    // The smoothed-position override belongs to the own predicted ship alone;
    // a spectated unit's Transform is never reconciled, so it needs none.
    m_cameraDirector.Update(view, CameraSubject(), m_viewportSize, dtSeconds,
                            IsSpectating() ? std::nullopt : std::optional<Magnum::Vector2>(smoothedPlayerPos));
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

    SubmitPlanetOwnershipMarkers(view);
    m_indicatorRenderer.Update(view, CameraSubject(), camera.GetPosition(), camera.GetZoom(), m_viewportSize,
                               m_pixelScale);
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

    m_renderTimeSeconds += dtSeconds;
    m_modelRenderer2.SetTime(m_renderTimeSeconds);
    m_mirrorRenderer2.SetTime(m_renderTimeSeconds);

    if (m_netClient) {
        RenderNetClient(dtSeconds, delta);
        return;
    }

    const SceneView view = CurrentSceneView();
    m_cameraDirector.Update(view, CameraSubject(), m_viewportSize, dtSeconds);
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
        m_indicatorRenderer.Update(view, CameraSubject(), camera.GetPosition(), camera.GetZoom(), m_viewportSize,
                                   m_pixelScale);
        SubmitPlanetOwnershipMarkers(view);
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
