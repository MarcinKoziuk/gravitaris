#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/component/team.hpp>

namespace Rml {
class Context;
class Element;
class ElementDocument;
class Event;
class EventListener;
}

namespace Gravitaris {

class SystemInterface;
class FileInterface;
class RenderInterfaceGL3;

class UI {
private:
    Rml::Context* m_context;
    Rml::ElementDocument* m_document = nullptr;

    std::unique_ptr<SystemInterface> m_systemInterface;

    std::unique_ptr<FileInterface> m_fileInterface;

    std::unique_ptr<RenderInterfaceGL3> m_renderInterfaceGl3;

    // Owned for the context's lifetime, since the documents hold raw pointers
    // to them.
    std::vector<std::unique_ptr<Rml::EventListener>> m_listeners;

    std::function<void(TeamId)> m_onIntroConfirm;
    std::function<void(float, float)> m_onMinimapClick;
    std::function<void()> m_onRecenter;

    // The side picked in the intro dialog's dropdown; committed when OK is
    // clicked. Must match the option marked `selected` in main.rml.
    TeamId m_introTeam = TeamId::Blue;

    Rml::Element* m_minimap = nullptr;
    Rml::Element* m_recenterButton = nullptr;
    bool m_recenterVisible = true;

    Rml::Element* m_hudStatus = nullptr;
    std::string m_hudStatusText;

    Rml::Element* m_hudPing = nullptr;
    std::string m_hudPingText;

    Rml::Element* m_sidebar = nullptr;
    Rml::Element* m_healthFill = nullptr;
    Rml::Element* m_healthValue = nullptr;
    float m_hullFraction = -1.f;

    Rml::Element* m_missileTicks = nullptr;
    Rml::Element* m_missileValue = nullptr;
    int m_missileAmmo = -2; // no valid count, not even "no subject" (-1)

    Rml::Element* m_speedReadout = nullptr;
    Rml::Element* m_headingReadout = nullptr;
    Rml::Element* m_gwellReadout = nullptr;
    std::string m_speedText;
    std::string m_headingText;
    std::string m_gwellText;

    int m_width = 1280;
    int m_height = 720;
    float m_dpRatio = 1.f;

    // Attaches `handler` to `element`, keeping the listener alive in
    // m_listeners for as long as this UI exists.
    void Listen(Rml::Element& element, const char* event, std::function<void(Rml::Event&)> handler);

    // Shared by the minimap's mousedown and drag events: both just report
    // where the cursor currently is on the panel.
    void HandleMinimapPoint(Rml::Event& event);

public:
    UI(IFilesystem& filesystem);

    ~UI();

    void Update();

    void Render();

    bool Init();

    // Both take effect immediately once Init() has run, and are remembered
    // until then. Call them before Init(): documents are laid out as they load,
    // so a context still on the placeholder size or ratio lays the HUD and the
    // intro dialog out wrong and they jump when the first frame corrects it.
    void SetDimensions(int width, int height);

    void SetDensityIndependentPixelRatio(float ratio);

    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int rmlButtonIndex, bool down);

    // Sidebar footer: build identity on one line, ping on the next (empty
    // when there's no server). The caller formats both; this layer holds no
    // game or net state. Repeated identical text is ignored, so calling it
    // every frame is fine.
    void SetHudStatus(const std::string& build, const std::string& ping);

    // Hull bar fill, 0..1; negative means "no subject" and blanks the bar.
    // Unchanged values are ignored, so calling it every frame is fine.
    void SetHullFraction(float fraction);

    // Missiles on the rack: the count as a number, plus one tick per round
    // with the rest of `capacity` drawn empty. Negative blanks the row.
    // Unchanged values are ignored, so calling it every frame is fine.
    void SetMissileAmmo(int ammo, int capacity);

    // Sidebar telemetry row: speed in world units/s, heading as a 0..360
    // bearing, and gravity field strength in units/s^2. An empty optional
    // blanks that cell. Repeated identical text is ignored, so calling it every
    // frame is fine.
    void SetHudTelemetry(std::optional<float> speed, std::optional<float> heading,
                         std::optional<float> gravityAccel);

    // Fired when the intro dialog's OK is clicked, with the side the player
    // picked. Set before Init(), which is what shows the dialog.
    void SetIntroConfirmCallback(std::function<void(TeamId)> callback);

    // Fired while the minimap panel is pressed or dragged, with the cursor as
    // -1..1 across the map in each axis, +Y up. The UI knows the panel's
    // pixels; mapping that to the world is the caller's job.
    void SetMinimapClickCallback(std::function<void(float, float)> callback);

    // Fired by the recenter button.
    void SetRecenterCallback(std::function<void()> callback);

    // The recenter button only earns its place while the view is off the
    // player's ship. Repeated identical values are ignored.
    void SetRecenterVisible(bool visible);

    // Laid-out width of the gameplay sidebar in context pixels, which the
    // caller insets the game viewport by so the scene never renders under it.
    // The width itself is declared in ui/hud.rml; this reads it back rather
    // than duplicating the number in code. Zero until the first Update().
    [[nodiscard]] int GetSidebarWidthPx() const;

    // Exposes an engine-owned GL texture to RML/RCSS as src="live://name"
    // (see RenderInterfaceGL3::RegisterLiveTexture). Register before the
    // first frame that renders a document referencing it.
    void RegisterLiveTexture(const std::string& name, unsigned glTextureId, int width, int height);

    void ToggleDebugger();
};

} // namespace Gravitaris
