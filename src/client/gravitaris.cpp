// Gravitaris.cpp : Defines the entry point for the application.
//

#include <algorithm>
#include <memory>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

#include <flecs.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/DefaultFramebuffer.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/Version.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Platform/Sdl2Application.h>
#include <Magnum/Platform/GLContext.h>

// CORRADE_TARGET_EMSCRIPTEN isn't defined until Corrade/configure.h (pulled
// in transitively above) has been seen, so this must come after the Magnum
// includes, not before.
#ifdef CORRADE_TARGET_EMSCRIPTEN
#include <emscripten/emscripten.h>
#endif

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/perf-monitor.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/input/input-command.hpp>

#include <gravitaris/cgame/cgame.hpp>
#include <gravitaris/cgame/renderer/glow-post-process.hpp>

#include <gravitaris/ui/ui.hpp>

#include <cgame/ui/debug/debug-ui.hpp>

#include "replay-controller.hpp"

namespace Gravitaris {

using Magnum::Platform::Application;

bool HasEnteredMain = false;

static double GetTime();
static Application::Configuration CreateConfiguration(const Application::Arguments& arguments);
static Application::GLConfiguration CreateGLConfiguration(const Application::Arguments& arguments);
// Native: scans argv for "--connect <ws-url>". Emscripten: reads ?connect=
// from the page's own URL (argv is never populated from it). Empty string
// means single-player.
static std::string GetConnectUrl(const Application::Arguments& arguments);

class GravitarisApplication : public Magnum::Platform::Application {
private:
    FilesystemPhysFS m_filesystem;

    std::unique_ptr<CGame> m_game;
    std::unique_ptr<GlowPostProcess> m_glow;
    std::unique_ptr<DebugUi> m_debugUi;

    UI m_ui;

public:
    explicit GravitarisApplication(const Arguments& arguments);

private:
    double m_prevTime;
    double m_frameTimeAccumulator;
    double m_startTime;

    bool m_uiInWorld = true; // render UI into the scene so it gets bloom + CRT

    // Live keyboard action state; FeedInput() turns it into one tick-stamped
    // command per sim tick. The sim never reads the keyboard directly.
    ControlFlags m_currentInput{};

    // Record/replay: F5 toggles recording to disk, F6 replays it back, F7 stops.
    ReplayController m_replay;

    void FeedInput();
    void ToggleRecording();
    void StartReplay();
    void StopReplay();

    void tickEvent() override;
    void drawEvent() override;
    void viewportEvent(ViewportEvent& event) override;
    void keyPressEvent(KeyEvent& event) override;
    void keyReleaseEvent(KeyEvent& event) override;
    void textInputEvent(TextInputEvent& event) override;
    void scrollEvent(ScrollEvent& event) override;
    void pointerPressEvent(PointerEvent& event) override;
    void pointerReleaseEvent(PointerEvent& event) override;
    void pointerMoveEvent(PointerMoveEvent& event) override;

    // Real HiDPI scale is reported differently per platform: on macOS,
    // framebufferSize() differs from windowSize() while dpiScaling() stays
    // {1,1}; on Windows it's the opposite -- framebufferSize()/windowSize()
    // are always identical (per Magnum's Sdl2Application docs) and the real
    // scale (e.g. 1.5 for 150% display scaling) shows up in dpiScaling()
    // instead. Whichever ratio is platform-meaningless is always 1, so take
    // the elementwise max of both to get the real scale on either platform.
    Magnum::Vector2 PixelScale() const
    {
        const Magnum::Vector2 fbRatio = Magnum::Vector2{framebufferSize()} / Magnum::Vector2{windowSize()};
        return Magnum::Math::max(fbRatio, dpiScaling());
    }

    static int RmlButtonIndex(Pointer pointer);
    void RenderUi();
};

GravitarisApplication::GravitarisApplication(const Arguments& arguments)
    : Magnum::Platform::Application{arguments, CreateConfiguration(arguments), CreateGLConfiguration(arguments)}
    , m_prevTime(GetTime())
    , m_frameTimeAccumulator(0.)
    , m_startTime(GetTime())
    , m_ui(m_filesystem)
{
    HasEnteredMain = true;

    using namespace Magnum::Math::Literals;
    Magnum::GL::Renderer::setClearColor(0x000000_rgbf);

    setWindowTitle(GRAVITARIS_NAME);

    m_filesystem.Init();

    // Game before UI: the HUD document (ui/hud.rml) references the minimap's
    // live texture, which must be registered before RmlUi first resolves it.
    m_game = std::make_unique<CGame>(m_filesystem);

    // docs/networking-plan.md 3.5.3: --connect ws://host:port (native) or
    // ?connect=ws://host:port (wasm, read from the page URL) switches into
    // multiplayer-client mode instead of the usual local single-player sim.
    const std::string connectUrl = GetConnectUrl(arguments);
    if (!connectUrl.empty()) {
        m_game->ConnectToServer(connectUrl);
    }
    else {
        m_game->Start();
    }

    m_ui.RegisterLiveTexture("minimap", m_game->GetMinimapRenderer().TextureId(),
                             MinimapRenderer::TextureSize().x(), MinimapRenderer::TextureSize().y());
    m_ui.Init();

    m_glow = std::make_unique<GlowPostProcess>(m_filesystem);

    // Dev overlay (hidden until F1). UI size in logical points; framebuffer
    // size keeps fonts crisp on HiDPI.
    m_debugUi = std::make_unique<DebugUi>(*m_game, *m_glow,
                                          Magnum::Vector2{windowSize()}, windowSize(), framebufferSize());
}

    // Claude: there is stuff here that would belong to cgame. client/ is only for wiring everything up and maybe later platform- specific startup  logic
void GravitarisApplication::tickEvent()
{
    const double curTime = GetTime();
    const double rawFrameTime = curTime - m_prevTime;

    // Real (unclamped) wall-clock delta between ticks -- the FPS the
    // performance overlay reports, as opposed to the clamped delta below that
    // protects the physics step from a lag spike.
    m_game->GetPerfMonitor().Record("Frame", static_cast<float>(rawFrameTime * 1000.0));

    // Prevent too big physics step on lag
    const double frameTime = std::min(rawFrameTime, .25);

    m_prevTime = curTime;

    // Multiplayer client: the server is authoritative, but the own ship is
    // locally predicted (docs/networking-plan.md Phase 5) and needs the
    // same fixed-step catch-up single-player physics gets, so predicted
    // ticks stay exactly Game::PHYSICS_DELTA apart regardless of frame
    // rate. Render() still drives NetClient::Update()/reconciliation/
    // remote-entity interpolation itself.
    if (m_game->IsNetClient()) {
        m_frameTimeAccumulator += frameTime;
        static constexpr int MAX_STEPS_PER_FRAME = 5;
        int steps = 0;
        while (m_frameTimeAccumulator >= Game::PHYSICS_DELTA) {
            if (steps >= MAX_STEPS_PER_FRAME) {
                m_frameTimeAccumulator = 0.0;
                break;
            }
            m_game->TickNetClient(m_currentInput);
            m_frameTimeAccumulator -= Game::PHYSICS_DELTA;
            ++steps;
        }
        m_currentInput.fireSecondary = false; // one-shot, same as FeedInput()
        redraw();
        ScopedPerfTimer timer(m_game->GetPerfMonitor(), "UI Update");
        m_ui.Update();
        return;
    }

    m_frameTimeAccumulator += frameTime;

    // Fixed-step catch-up: a slow frame runs multiple sim steps so the sim
    // keeps real time instead of slowing down with the frame rate (a server
    // must hold its tick rate regardless of render load -- see
    // docs/networking-plan.md Phase 0). FeedInput runs once per step, since
    // each tick needs its own command. Capped so a debugger pause or huge
    // stall doesn't spiral; past the cap the leftover accumulated time is
    // dropped (a one-time hitch, not a permanent slowdown).
    static constexpr int MAX_STEPS_PER_FRAME = 5;
    int steps = 0;
    while (m_frameTimeAccumulator >= Game::PHYSICS_DELTA) {
        if (steps >= MAX_STEPS_PER_FRAME) {
            m_frameTimeAccumulator = 0.0;
            static bool warnedStepCap = false;
            if (!warnedStepCap) {
                warnedStepCap = true;
                LOG(warning) << "sim step cap (" << MAX_STEPS_PER_FRAME
                             << "/frame) hit; dropping accumulated time (frame rate below "
                             << (1.0 / (MAX_STEPS_PER_FRAME * Game::PHYSICS_DELTA)) << " fps)";
            }
            break;
        }
        FeedInput();
        m_game->Update();
        m_frameTimeAccumulator -= Game::PHYSICS_DELTA;
        ++steps;
    }

    redraw();

    {
        ScopedPerfTimer timer(m_game->GetPerfMonitor(), "UI Update");
        m_ui.Update();
    }
}

// One command for the tick Update() is about to run: keyboard, autopilot and
// replay all feed the player's InputQueue here.
void GravitarisApplication::FeedInput()
{
    std::optional<flecs::entity> maybePlayer = m_game->GetPlayer();
    if (!maybePlayer) return;

    const std::uint64_t tick = m_game->GetStep();

    InputCommand cmd;
    cmd.tick = tick;

    if (m_replay.IsReplaying()) {
        cmd.flags = m_replay.NextReplayCommand();
    }
    else {
        cmd.flags = m_currentInput;
        // Autopilot overrides movement but not fire.
        if (std::optional<ControlFlags> autopilot = m_game->ComputeAutopilotControls()) {
            cmd.flags = *autopilot;
            cmd.flags.firePrimary = m_currentInput.firePrimary;
            cmd.flags.fireSecondary = m_currentInput.fireSecondary;
        }
    }

    maybePlayer->get_mut<InputQueue>().Push(cmd);

    m_replay.RecordIfActive(cmd, tick);

    // One-shot actions apply only for the tick they were pressed on.
    // (firePrimary is held; released on key-up.)
    m_currentInput.fireSecondary = false;
}

void GravitarisApplication::ToggleRecording()
{
    m_replay.ToggleRecording(m_game->GetStep());
}

void GravitarisApplication::StartReplay()
{
    if (m_replay.StartReplay(m_game->GetStep())) {
        m_currentInput = ControlFlags{}; // drop any held keys
    }
}

void GravitarisApplication::StopReplay()
{
    m_replay.StopReplay();
}

void GravitarisApplication::RenderUi()
{
    ScopedPerfTimer timer(m_game->GetPerfMonitor(), "UI Render");

    // RmlUi uses raw OpenGL calls that bypass Magnum's state cache.
    Magnum::GL::Context::current().resetState(Magnum::GL::Context::State::EnterExternal);
    m_ui.Render();
    Magnum::GL::Context::current().resetState(Magnum::GL::Context::State::ExitExternal);
}

void GravitarisApplication::drawEvent()
{
    // framebufferSize(), not windowSize() -- differ on HiDPI/Retina.
    const Magnum::Vector2i fbSize = framebufferSize();
    const Magnum::Vector2i logicalSize = windowSize();

    m_game->SetViewportSize(Magnum::Vector2{fbSize});
    m_game->SetPixelScale(PixelScale().x());
    m_ui.SetDimensions(fbSize.x(), fbSize.y());
    m_ui.SetDensityIndependentPixelRatio(PixelScale().x());

    PerfMonitor& perf = m_game->GetPerfMonitor();

    // Minimap first: it renders to its own offscreen texture (sampled later
    // by the RmlUi pass), and must not disturb the glow scene target below.
    {
        ScopedPerfTimer timer(perf, "Minimap");
        m_game->RenderMinimap();
    }

    // Game renders into the glow pass's offscreen target, not the screen,
    // so it can be blurred/composited.
    {
        ScopedPerfTimer timer(perf, "Post-process Begin");
        m_glow->BeginScene(fbSize, logicalSize);
    }
    const double delta = m_frameTimeAccumulator / Game::PHYSICS_DELTA;
    m_game->Render(delta);

    // Small bounded clock for the CRT shader's sin(); wall-clock epoch time
    // would lose float precision. Wraps every 1000s.
    const float animTime = static_cast<float>(std::fmod(GetTime() - m_startTime, 1000.0));

    if (m_uiInWorld) {
        // UI into the scene before compositing, so it gets bloom + CRT too.
        RenderUi();
        ScopedPerfTimer timer(perf, "Post-process Composite");
        m_glow->EndSceneAndComposite(Magnum::GL::defaultFramebuffer, fbSize, animTime);
    } else {
        {
            ScopedPerfTimer timer(perf, "Post-process Composite");
            m_glow->EndSceneAndComposite(Magnum::GL::defaultFramebuffer, fbSize, animTime);
        }
        RenderUi();
    }

    // Restore viewport to full size; RmlUi sets it to its own context size.
    Magnum::GL::defaultFramebuffer.setViewport({{}, fbSize});

    // Dev overlay on top of everything, crisp (drawn after the CRT present).
    Magnum::GL::defaultFramebuffer.bind();
    {
        ScopedPerfTimer timer(perf, "Debug UI");
        m_debugUi->Draw();
    }

    // The context is double-buffered, swap buffers
    swapBuffers();
}

void GravitarisApplication::viewportEvent(ViewportEvent& event)
{
    Magnum::GL::defaultFramebuffer.setViewport({{}, event.framebufferSize()});
    m_debugUi->Relayout(Magnum::Vector2{event.windowSize()}, event.windowSize(), event.framebufferSize());
}

void GravitarisApplication::keyPressEvent(Magnum::Platform::Sdl2Application::KeyEvent& event)
{
    // F1 toggles the dev overlay; H is an alternate on the same action, since
    // bare F1 is a hardware brightness key on Mac laptops and never reaches
    // the app without holding Fn. Text input is enabled only while the
    // overlay is shown so ImGui text fields work without stealing keystrokes
    // during gameplay.
    if (event.key() == KeyEvent::Key::F1 || event.key() == KeyEvent::Key::H) {
        m_debugUi->Toggle();
        if (m_debugUi->IsVisible()) startTextInput();
        else stopTextInput();
        event.setAccepted();
        return;
    }

    // When the overlay has keyboard focus (e.g. an active widget), route keys
    // to it and keep them away from gameplay.
    if (m_debugUi->HandleKeyPress(event)) {
        event.setAccepted();
        return;
    }

    constexpr float LINE_WIDTH_STEP = 0.5f;
    switch (event.key()) {
        case KeyEvent::Key::NumAdd:
            m_game->AddLineWidth(LINE_WIDTH_STEP);
            return;
        case KeyEvent::Key::NumSubtract:
            m_game->AddLineWidth(-LINE_WIDTH_STEP);
            return;
        case KeyEvent::Key::C:
            m_game->ToggleDebugForceFacetedCircles();
            return;
        case KeyEvent::Key::B:
            m_glow->SetEnabled(!m_glow->IsEnabled());
            return;
        case KeyEvent::Key::V:
            m_glow->SetCrtEnabled(!m_glow->IsCrtEnabled());
            return;
        case KeyEvent::Key::RightBracket:
            m_glow->AddIntensity(0.25f);
            return;
        case KeyEvent::Key::LeftBracket:
            m_glow->AddIntensity(-0.25f);
            return;
        case KeyEvent::Key::U:
            m_uiInWorld = !m_uiInWorld;
            return;
        case KeyEvent::Key::F:
            m_game->ToggleCameraFollow();
            return;
        case KeyEvent::Key::F8:
            m_ui.ToggleDebugger();
            return;
        case KeyEvent::Key::F5:
            ToggleRecording();
            return;
        case KeyEvent::Key::F6:
            StartReplay();
            return;
        case KeyEvent::Key::F7:
            StopReplay();
            return;
        case KeyEvent::Key::K:
            m_game->ToggleAutopilotMode(AutopilotMode::KillVelocity);
            return;
        case KeyEvent::Key::P:
            m_game->ToggleAutopilotMode(AutopilotMode::HoldPosition);
            return;
        case KeyEvent::Key::G:
            m_game->ToggleAutopilotMode(AutopilotMode::GotoPoint);
            return;
        case KeyEvent::Key::O:
            m_game->ToggleAutopilotMode(AutopilotMode::Orbit);
            return;
        case KeyEvent::Key::J:
            m_game->SpawnRandomAIShip();
            return;
        default:
            break;
    }

    // While replaying, the recorded command stream drives the ship; ignore
    // live gameplay keys (debug keys above still work).
    if (m_replay.IsReplaying()) return;

    switch (event.key()) {
        // Manual movement input disengages the autopilot.
        case KeyEvent::Key::Up:
            m_game->SetAutopilotMode(AutopilotMode::Off);
            m_currentInput.thrustForward = true;
            break;
        case KeyEvent::Key::Right:
            m_game->SetAutopilotMode(AutopilotMode::Off);
            m_currentInput.rotateRight = true;
            break;
        case KeyEvent::Key::Left:
            m_game->SetAutopilotMode(AutopilotMode::Off);
            m_currentInput.rotateLeft = true;
            break;
        case KeyEvent::Key::Down:
            m_currentInput.firePrimary = true;   // held; cadence paced by the sim
            break;
        case KeyEvent::Key::Space:
            m_currentInput.fireSecondary = true; // one-shot, cleared after the tick
            break;
        default:
            (void)0;
    }
}

void GravitarisApplication::keyReleaseEvent(Magnum::Platform::Sdl2Application::KeyEvent& event)
{
    if (m_debugUi->HandleKeyRelease(event)) {
        event.setAccepted();
        return;
    }

    if (m_replay.IsReplaying()) return;

    switch (event.key()) {
        case KeyEvent::Key::Up:
            m_currentInput.thrustForward = false;
            break;
        case KeyEvent::Key::Right:
            m_currentInput.rotateRight = false;
            break;
        case KeyEvent::Key::Left:
            m_currentInput.rotateLeft = false;
            break;
        case KeyEvent::Key::Down:
            m_currentInput.firePrimary = false;
            break;
        default:
            (void)0;
    }
}

void GravitarisApplication::textInputEvent(TextInputEvent& event)
{
    if (m_debugUi->HandleTextInput(event)) {
        event.setAccepted();
    }
}

void GravitarisApplication::scrollEvent(ScrollEvent& event)
{
    if (m_debugUi->HandleScroll(event)) {
        event.setAccepted();
        return;
    }

    // Clamp a single event's magnitude: NudgeManualZoom scales zoom
    // multiplicatively (1.15^notches), and inertial/momentum scroll (trackpad
    // flings, some wheel drivers) can report one outsized offset -- unclamped,
    // that compounds into a jump straight to min/max zoom that reads as
    // "runaway" scrolling. Purely client-side; unrelated to the sim tick loop.
    const float notches = std::clamp(event.offset().y(), -3.f, 3.f);
    m_game->NudgeManualZoom(notches);
    event.setAccepted();
}

int GravitarisApplication::RmlButtonIndex(Pointer pointer)
{
    // RmlUi convention: 0 = left, 1 = right, 2 = middle.
    switch (pointer) {
        case Pointer::MouseRight:  return 1;
        case Pointer::MouseMiddle: return 2;
        default:                   return 0; // MouseLeft / anything else
    }
}

void GravitarisApplication::pointerPressEvent(PointerEvent& event)
{
    if (m_debugUi->HandlePointerPress(event)) {
        event.setAccepted();
        return;
    }

    // event.position() is in logical points; UI context is in physical pixels.
    const Vector2 p = event.position() * PixelScale();
    m_ui.ProcessMouseMove(static_cast<int>(p.x()), static_cast<int>(p.y()));
    if (m_ui.ProcessMouseButton(RmlButtonIndex(event.pointer()), true)) {
        event.setAccepted();
    }
}

void GravitarisApplication::pointerReleaseEvent(PointerEvent& event)
{
    if (m_debugUi->HandlePointerRelease(event)) {
        event.setAccepted();
        return;
    }

    if (m_ui.ProcessMouseButton(RmlButtonIndex(event.pointer()), false)) {
        event.setAccepted();
    }
}

void GravitarisApplication::pointerMoveEvent(PointerMoveEvent& event)
{
    if (m_debugUi->HandlePointerMove(event)) {
        event.setAccepted();
        return;
    }

    const Vector2 p = event.position() * PixelScale();
    if (m_ui.ProcessMouseMove(static_cast<int>(p.x()), static_cast<int>(p.y()))) {
        event.setAccepted();
    }
}

static double GetTime()
{
    auto time = std::chrono::high_resolution_clock::now();
    auto epoch = time.time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();
    return (double)micros / 1000. / 1000.;
}

static Application::Configuration CreateConfiguration(const Application::Arguments&)
{
    Application::Configuration conf;
#ifdef CORRADE_TARGET_WINDOWS
    // Force 1:1 scaling so the window/framebuffer is always exactly the
    // requested 1920x1080, regardless of the display's scaling setting.
    // Windows' virtual DPI scaling would otherwise inflate the real pixel
    // size (e.g. to 2880x1620 at 150%), and the line-width/CRT-scanline
    // DPI compensation would then proportionally thicken everything to
    // "look the same apparent size" -- which reads as blown up relative to
    // the reference look tuned at 1920x1080. macOS's Retina scaling (a
    // genuinely higher-density framebuffer at the *same* window size) is
    // unaffected by this and keeps using its own Framebuffer DPI policy.
    conf.setSize({1920, 1080}, Magnum::Vector2{1.0f});
#else
    conf.setSize({1920, 1080});
#endif
    conf.setWindowFlags(Application::Configuration::WindowFlag::Resizable);
    return conf;
}

#ifdef CORRADE_TARGET_EMSCRIPTEN
static std::string GetConnectUrl(const Application::Arguments&)
{
    // argv is never populated from the page URL under Emscripten, so this is
    // the only way in: read ?connect=... from window.location.search
    // ourselves. EM_ASM_PTR hands back a malloc'd C string (per Emscripten's
    // own convention for returning strings across the JS/C++ boundary);
    // freed immediately after copying into the std::string we actually keep.
    // The code block contains a comma (stringToUTF8's arg list) -- per
    // em_asm.h's own doc comment, that requires wrapping the whole block in
    // an extra layer of parens so the C preprocessor doesn't split it into
    // separate macro arguments.
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR(({
        const params = new URLSearchParams(window.location.search);
        const value = params.get("connect") || "";
        const bytes = lengthBytesUTF8(value) + 1;
        const ptr = _malloc(bytes);
        stringToUTF8(value, ptr, bytes);
        return ptr;
    })));
    std::string url(raw);
    free(raw);
    return url;
}
#else
static std::string GetConnectUrl(const Application::Arguments& arguments)
{
    for (int i = 1; i + 1 < arguments.argc; ++i) {
        if (std::string_view{arguments.argv[i]} == "--connect") return arguments.argv[i + 1];
    }
    return {};
}
#endif

static Application::GLConfiguration CreateGLConfiguration(const Application::Arguments&)
{
    Application::GLConfiguration conf;
#ifndef CORRADE_TARGET_EMSCRIPTEN
    // GlowPostProcess composites its single-sampled result into the default
    // framebuffer via AbstractFramebuffer::blit() every frame (see
    // EndSceneAndComposite). Under GLES3/WebGL2 that's an INVALID_OPERATION
    // if the *destination* is multisampled -- unlike some desktop GL drivers,
    // WebGL2 actually enforces the spec's "blit cannot target a multisampled
    // framebuffer" rule strictly. Lines already get analytic edge AA in their
    // own shader (see line2.f.glsl), so skipping window MSAA here costs very
    // little.
    conf.setSampleCount(4);
#endif
    return conf;
}

} // namespace Gravitaris

MAGNUM_APPLICATION_MAIN(Gravitaris::GravitarisApplication)
