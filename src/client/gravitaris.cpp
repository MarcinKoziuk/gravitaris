// Gravitaris.cpp : Defines the entry point for the application.
//

#include <memory>
#include <chrono>
#include <cmath>

#include <entt/entt.hpp>

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
    void keyPressEvent(KeyEvent& event) override;
    void keyReleaseEvent(KeyEvent& event) override;
    void scrollEvent(ScrollEvent& event) override;
    void pointerPressEvent(PointerEvent& event) override;
    void pointerReleaseEvent(PointerEvent& event) override;
    void pointerMoveEvent(PointerMoveEvent& event) override;

    // The real backing-store scale: framebufferSize() / windowSize().
    // NOT the same as dpiScaling() -- on macOS that stays {1, 1} by default
    // even when the two sizes genuinely differ (confirmed via [DPI DEBUG]
    // log: windowSize=1920x1080, framebufferSize=3840x2160, dpiScaling=1,1).
    Magnum::Vector2 PixelScale() const
    {
        return Magnum::Vector2{framebufferSize()} / Magnum::Vector2{windowSize()};
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

    redraw(); // ?

    m_ui.Update(); // ? Claude: now the normal game doesn't show any more
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
    // Use the real framebuffer size (pixels), not windowSize() (logical
    // points). On HiDPI/Retina displays the two differ by the DPI scale
    // factor; sizing viewports/offscreen targets and RmlUi's canvas off the
    // smaller logical size left the actual framebuffer mostly uninitialized
    // (visible as a small rendered region in one corner and black
    // elsewhere), and the postprocess pass sampling across that boundary is
    // what showed up as glitching once glow was enabled.
    const Magnum::Vector2i fbSize = framebufferSize();

    m_game->SetViewportSize(Magnum::Vector2{fbSize});
    m_ui.SetDimensions(fbSize.x(), fbSize.y());
    // Context dimensions are physical pixels (fbSize); without a matching
    // dp-ratio, RCSS px units render at their raw pixel size and end up
    // visually shrunk by the display's backing-store scale on HiDPI/Retina
    // screens. Deliberately PixelScale(), not dpiScaling() -- see PixelScale().
    m_ui.SetDensityIndependentPixelRatio(PixelScale().x());

    // Render the game world into the glow pass's offscreen scene target
    // instead of directly onto the screen, so it can be blurred/composited.
    m_glow->BeginScene(fbSize);
    const double delta = m_frameTimeAccumulator / Game::PHYSICS_DELTA;
    m_game->Render(delta);

    // Bounded, small-magnitude clock for the CRT wiggle's sin() — NOT raw
    // wall-clock epoch time, which is large enough that converting to float
    // loses precision and makes sin() unstable. Wraps every 1000s; the wrap
    // isn't an exact multiple of the wiggle's period, so there's a
    // once-every-~16-minutes micro-jump, imperceptible for a cosmetic effect.
    const float animTime = static_cast<float>(std::fmod(GetTime() - m_startTime, 1000.0));

    if (m_uiInWorld) {
        // Draw the UI into the still-bound scene target BEFORE compositing, so
        // its bright cyan lines pick up the same bloom + CRT as the game.
        RenderUi();
        m_glow->EndSceneAndComposite(Magnum::GL::defaultFramebuffer, fbSize, animTime);
    } else {
        // Composite first, then draw the UI crisply on top of the result.
        m_glow->EndSceneAndComposite(Magnum::GL::defaultFramebuffer, fbSize, animTime);
        RenderUi();
    }

    // Restore viewport to full size; RmlUi sets it to its own context size.
    Magnum::GL::defaultFramebuffer.setViewport({{}, fbSize});

    // The context is double-buffered, swap buffers
    swapBuffers();
}

void GravitarisApplication::keyPressEvent(Magnum::Platform::Sdl2Application::KeyEvent& event)
{
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
        case KeyEvent::Key::F8:
            m_ui.ToggleDebugger();
            return;
        default:
            break;
    }

    std::optional<entt::entity> maybePlayer = m_game->GetPlayer();
    if (!maybePlayer) return;

    entt::entity player = *maybePlayer;
    Controls& playerControls = m_game->GetRegistry().get<Controls>(player);

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
    std::optional<entt::entity> maybePlayer = m_game->GetPlayer();
    if (!maybePlayer) return;

    entt::entity player = *maybePlayer;
    Controls& playerControls = m_game->GetRegistry().get<Controls>(player);

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

void GravitarisApplication::scrollEvent(ScrollEvent& event)
{
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
    // event.position() is in logical window points; the UI context is sized
    // in physical framebuffer pixels (see drawEvent), so scale up to match.
    const Vector2 p = event.position() * PixelScale();
    m_ui.ProcessMouseMove(static_cast<int>(p.x()), static_cast<int>(p.y()));
    if (m_ui.ProcessMouseButton(RmlButtonIndex(event.pointer()), true)) {
        event.setAccepted();
    }
}

void GravitarisApplication::pointerReleaseEvent(PointerEvent& event)
{
    if (m_ui.ProcessMouseButton(RmlButtonIndex(event.pointer()), false)) {
        event.setAccepted();
    }
}

void GravitarisApplication::pointerMoveEvent(PointerMoveEvent& event)
{
    // See pointerPressEvent: scale logical points up to physical pixels.
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
    conf.setSize({1920, 1080});
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
