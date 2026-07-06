// Gravitaris.cpp : Defines the entry point for the application.
//

#include <memory>
#include <chrono>

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

    UI m_ui;

public:
    explicit GravitarisApplication(const Arguments& arguments);

private:
    double m_prevTime;
    double m_frameTimeAccumulator;

    void tickEvent() override;
    void drawEvent() override;
    void keyPressEvent(KeyEvent& event) override;
    void keyReleaseEvent(KeyEvent& event) override;
};

GravitarisApplication::GravitarisApplication(const Arguments& arguments)
    : Magnum::Platform::Application{arguments, CreateConfiguration(arguments), CreateGLConfiguration(arguments)}
    , m_prevTime(GetTime())
    , m_frameTimeAccumulator(0.)
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

void GravitarisApplication::drawEvent()
{
    // Clear the window and restore viewport to full size;
    // RmlUi sets the viewport to its context size and doesn't restore it
    Magnum::GL::defaultFramebuffer.clear(Magnum::GL::FramebufferClear::Color);
    Magnum::GL::defaultFramebuffer.setViewport({{}, windowSize()});

    // Render
    const double delta = m_frameTimeAccumulator / Game::PHYSICS_DELTA;
    m_game->Render(delta);

    // RmlUi uses raw OpenGL calls that bypass Magnum's state cache
    Magnum::GL::Context::current().resetState(Magnum::GL::Context::State::EnterExternal);
    m_ui.Render();
    Magnum::GL::Context::current().resetState(Magnum::GL::Context::State::ExitExternal);

    // The context is double-buffered, swap buffers
    swapBuffers();
}

void GravitarisApplication::keyPressEvent(Magnum::Platform::Sdl2Application::KeyEvent& event)
{
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
