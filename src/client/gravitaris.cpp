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
#include <gravitaris/game/component/controls.hpp>

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
        m_game->Update();
        m_frameTimeAccumulator -= Game::PHYSICS_DELTA;
    }

    redraw();

    m_ui.Update();
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
    // F1 toggles the dev overlay. Text input is enabled only while it's shown
    // so ImGui text fields work without stealing keystrokes during gameplay.
    if (event.key() == KeyEvent::Key::F1) {
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
        default:
            break;
    }

    std::optional<flecs::entity> maybePlayer = m_game->GetPlayer();
    if (!maybePlayer) return;

    flecs::entity player = *maybePlayer;
    Controls& playerControls = player.get_mut<Controls>();

    switch (event.key()) {
        case KeyEvent::Key::Up:
            playerControls.actionFlags.thrustForward = true;
            break;
        case KeyEvent::Key::Right:
            playerControls.actionFlags.rotateRight = true;
            break;
        case KeyEvent::Key::Left:
            playerControls.actionFlags.rotateLeft = true;
            break;
        case KeyEvent::Key::Space:
            playerControls.actionFlags.fireSecondary = true;
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

    std::optional<flecs::entity> maybePlayer = m_game->GetPlayer();
    if (!maybePlayer) return;

    flecs::entity player = *maybePlayer;
    Controls& playerControls = player.get_mut<Controls>();

    switch (event.key()) {
        case KeyEvent::Key::Up:
            playerControls.actionFlags.thrustForward = false;
            break;
        case KeyEvent::Key::Right:
            playerControls.actionFlags.rotateRight = false;
            break;
        case KeyEvent::Key::Left:
            playerControls.actionFlags.rotateLeft = false;
            break;
        case KeyEvent::Key::Down:
            playerControls.actionFlags.firePrimary = true;
            break;
        case KeyEvent::Key::Space:
            playerControls.actionFlags.fireSecondary = false;
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
