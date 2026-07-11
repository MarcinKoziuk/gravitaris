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

    // framebufferSize() / windowSize(). NOT dpiScaling() -- that stays {1,1}
    // on macOS even when the two sizes genuinely differ.
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
        case KeyEvent::Key::F:
            m_game->ToggleCameraFollow();
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
    // event.position() is in logical points; UI context is in physical pixels.
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
