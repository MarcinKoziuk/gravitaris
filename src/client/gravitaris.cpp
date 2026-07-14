// Gravitaris.cpp : Defines the entry point for the application.
//

#include <memory>
#include <chrono>
#include <cmath>

#include <flecs.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/DefaultFramebuffer.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/Version.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Platform/Sdl2Application.h>
#include <Magnum/Platform/GLContext.h>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/input/input-log.hpp>

#include <gravitaris/cgame/cgame.hpp>
#include <gravitaris/cgame/renderer/glow-post-process.hpp>

#include <gravitaris/ui/ui.hpp>

#include <cgame/ui/debug/debug-ui.hpp>

namespace Gravitaris {

using Magnum::Platform::Application;

bool HasEnteredMain = false;

static double GetTime();
static Application::Configuration CreateConfiguration(const Application::Arguments& arguments);
static Application::GLConfiguration CreateGLConfiguration(const Application::Arguments& arguments);

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
    InputLog     m_recordLog;
    bool         m_recording = false;
    std::uint64_t m_recordStartTick = 0;

    InputLog     m_replayLog;
    bool         m_replaying = false;
    std::size_t  m_replayCursor = 0;

    static constexpr const char* REPLAY_PATH = "input-replay.grinput";

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

    m_ui.Init();

    m_game = std::make_unique<CGame>(m_filesystem);
    m_game->Start();

    m_glow = std::make_unique<GlowPostProcess>(m_filesystem);

    // Dev overlay (hidden until F1). UI size in logical points; framebuffer
    // size keeps fonts crisp on HiDPI.
    m_debugUi = std::make_unique<DebugUi>(*m_game, *m_glow,
                                          Magnum::Vector2{windowSize()}, windowSize(), framebufferSize());
}

void GravitarisApplication::tickEvent()
{
    const double curTime = GetTime();

    // Prevent too big physics step on lag
    const double frameTime = std::min(curTime - m_prevTime, .25);

    m_prevTime = curTime;

    m_frameTimeAccumulator += frameTime;

    if (m_frameTimeAccumulator >= Game::PHYSICS_DELTA) {
        FeedInput();
        m_game->Update();
        m_frameTimeAccumulator -= Game::PHYSICS_DELTA;
    }

    redraw();

    m_ui.Update();
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

    if (m_replaying) {
        if (m_replayCursor >= m_replayLog.Size()) {
            StopReplay();
            cmd.flags = ControlFlags{};
        }
        else {
            cmd.flags = m_replayLog.Commands()[m_replayCursor].flags;
            ++m_replayCursor;
        }
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

    if (m_recording) {
        // Store tick-relative so the log can replay from any starting tick.
        InputCommand rec = cmd;
        rec.tick = tick - m_recordStartTick;
        m_recordLog.Append(rec);
    }

    // One-shot actions apply only for the tick they were pressed on.
    m_currentInput.firePrimary = false;
    m_currentInput.fireSecondary = false;
}

void GravitarisApplication::ToggleRecording()
{
    if (m_recording) {
        m_recording = false;
        if (m_recordLog.Save(REPLAY_PATH)) {
            LOG(info) << "Saved input replay '" << REPLAY_PATH << "' ("
                      << m_recordLog.Size() << " commands)";
        } else {
            LOG(warning) << "Failed to save input replay '" << REPLAY_PATH << "'";
        }
    } else {
        StopReplay();
        m_recordLog.Clear();
        m_recordStartTick = m_game->GetStep();
        m_recording = true;
        LOG(info) << "Recording input at tick " << m_recordStartTick;
    }
}

void GravitarisApplication::StartReplay()
{
    if (m_recording) ToggleRecording(); // stop & flush recording first

    if (!m_replayLog.Load(REPLAY_PATH)) {
        LOG(warning) << "No input replay to load at '" << REPLAY_PATH << "'";
        return;
    }

    m_replaying = true;
    m_replayCursor = 0;
    m_currentInput = ControlFlags{};     // drop any held keys
    LOG(info) << "Replaying input '" << REPLAY_PATH << "' ("
              << m_replayLog.Size() << " commands)";
}

void GravitarisApplication::StopReplay()
{
    if (!m_replaying) return;
    m_replaying = false;
    LOG(info) << "Replay stopped";
}

void GravitarisApplication::RenderUi()
{
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

    // Game renders into the glow pass's offscreen target, not the screen,
    // so it can be blurred/composited.
    m_glow->BeginScene(fbSize, logicalSize);
    const double delta = m_frameTimeAccumulator / Game::PHYSICS_DELTA;
    m_game->Render(delta);

    // Small bounded clock for the CRT shader's sin(); wall-clock epoch time
    // would lose float precision. Wraps every 1000s.
    const float animTime = static_cast<float>(std::fmod(GetTime() - m_startTime, 1000.0));

    if (m_uiInWorld) {
        // UI into the scene before compositing, so it gets bloom + CRT too.
        RenderUi();
        m_glow->EndSceneAndComposite(Magnum::GL::defaultFramebuffer, fbSize, animTime);
    } else {
        m_glow->EndSceneAndComposite(Magnum::GL::defaultFramebuffer, fbSize, animTime);
        RenderUi();
    }

    // Restore viewport to full size; RmlUi sets it to its own context size.
    Magnum::GL::defaultFramebuffer.setViewport({{}, fbSize});

    // Dev overlay on top of everything, crisp (drawn after the CRT present).
    Magnum::GL::defaultFramebuffer.bind();
    m_debugUi->Draw();

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
        default:
            break;
    }

    // While replaying, the recorded command stream drives the ship; ignore
    // live gameplay keys (debug keys above still work).
    if (m_replaying) return;

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
            m_currentInput.firePrimary = true;   // one-shot, cleared after the tick
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

    if (m_replaying) return;

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

    m_game->GetCamera().AddZoomNotches(event.offset().y());
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

static Application::GLConfiguration CreateGLConfiguration(const Application::Arguments&)
{
    Application::GLConfiguration conf;
    conf.setSampleCount(4);
    return conf;
}

} // namespace Gravitaris

MAGNUM_APPLICATION_MAIN(Gravitaris::GravitarisApplication)
