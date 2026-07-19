#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/net/snapshot.hpp>

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
    , m_minimapRenderer(m_registry, m_physicsSystem, filesystem)
    , m_audioSystem(m_registry, m_resourceLoader, m_eventQueue)
    , m_hitFlashSystem(m_registry, m_eventQueue, *m_entitySpawner)
    , m_cameraDirector(m_registry, m_physicsSystem, Defaults::cameraZoom)
    , m_indicatorRenderer(m_registry, m_resourceLoader, m_modelRenderer2)
    , m_clientPrediction(m_registry, m_physicsSystem, *m_entitySpawner)
    , m_autopilot(m_registry, m_physicsSystem)
{
    m_modelRenderer2.SetReferenceZoom(Defaults::cameraZoom);
    m_mirrorRenderer2.SetReferenceZoom(Defaults::cameraZoom);

    // This game's tuned default (Game's own default is 1 = unmodified): a
    // lighter ship reads better against the solar system's gravity wells.
    // Headless Games (sim-test) never call this, so their determinism is
    // unaffected by this specific value.
    SetShipWeightMultiplier(0.667f);
}

void CGame::RenderMinimap()
{
    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) return; // between death and respawn: freeze the last frame

    const Camera& camera = m_cameraDirector.GetCamera();
    const Magnum::Vector2 playerPos{static_cast<float>(transform->pos.x()),
                                    static_cast<float>(transform->pos.y())};
    const Magnum::Vector2 viewHalfExtent = m_viewportSize / (2.f * std::max(camera.GetZoom(), 1e-3f));

    // Static, not player-centered: the solar system is laid out symmetrically
    // around the origin (see Game::Start), so that's the whole map's center.
    m_minimapRenderer.Render(Magnum::Vector2{0.f, 0.f}, playerPos, camera.GetPosition(), viewHalfExtent);
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
                it->modelId, Vector2d{static_cast<double>(it->pos.x()), static_cast<double>(it->pos.y())});
        m_player = m_clientPrediction.GetOwnShip();
        m_nextPredictedTick = m_netClient->EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS;
    }

    const std::uint64_t tick = m_nextPredictedTick++;
    m_netClient->SendInput(tick, flags);

    if (const std::optional<SnapshotData>& snapshot = m_netClient->GetLatestSnapshot()) {
        m_clientPrediction.Step(tick, flags, snapshot->entities);
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
}

void CGame::RenderNetClient(float dtSeconds)
{
    m_netClient->Update();

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
        m_snapshotApplier.Apply(*interpolated);
    }

    // Real single-player camera logic against m_registry -- works for the
    // own ship (dead-zone follow, dynamic zoom); enemy/planet framing won't
    // engage since those only exist in the mirror world, not here.
    m_cameraDirector.Update(GetPlayer(), m_viewportSize, dtSeconds);
    Camera& camera = m_cameraDirector.GetCamera();

    // Blend out any reconciliation snap over ~100ms by nudging the camera,
    // never the real simulated Transform (which must stay exactly correct
    // for the next predicted tick to build on).
    static constexpr float CORRECTION_SMOOTH_SECONDS = 0.1f;
    m_visualCorrectionOffset *= std::exp(-dtSeconds / CORRECTION_SMOOTH_SECONDS);
    camera.SetPosition(camera.GetPosition() - m_visualCorrectionOffset);

    m_starfieldRenderer.SetZoom(camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(camera.GetPosition());
    m_starfieldRenderer.Render();

    m_mirrorRenderer2.SetZoom(camera.GetZoom());
    m_mirrorRenderer2.SetCameraPosition(camera.GetPosition());
    m_mirrorRenderer2.SetLineWidth(m_lineWidthPixels);
    m_mirrorRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
    m_mirrorRenderer2.Render(0.0);

    // Own ship: real local sim, drawn through the same renderer/world
    // single-player uses -- m_registry holds nothing else in this mode.
    m_modelRenderer2.SetZoom(camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
    m_modelRenderer2.Render(0.0);
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
