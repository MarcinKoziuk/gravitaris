#pragma once

#include <functional>
#include <memory>
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

    // The side highlighted in the intro dialog; committed when OK is clicked.
    TeamId m_introTeam = TeamId::Blue;
    Rml::Element* m_teamBlueButton = nullptr;
    Rml::Element* m_teamRedButton = nullptr;
    Rml::Element* m_teamRow = nullptr;

    Rml::Element* m_minimap = nullptr;

    Rml::Element* m_hudStatus = nullptr;
    std::string m_hudStatusText;

    Rml::Element* m_sidebar = nullptr;
    Rml::Element* m_healthFill = nullptr;
    Rml::Element* m_healthValue = nullptr;
    float m_hullFraction = -1.f;

    bool m_teamChoiceEnabled = true;

    int m_width = 1280;
    int m_height = 720;

    // Attaches `handler` to `element`, keeping the listener alive in
    // m_listeners for as long as this UI exists.
    void Listen(Rml::Element& element, const char* event, std::function<void(Rml::Event&)> handler);

    void SelectIntroTeam(TeamId team);

    // Shared by the minimap's mousedown and drag events: both just report
    // where the cursor currently is on the panel.
    void HandleMinimapPoint(Rml::Event& event);

public:
    UI(IFilesystem& filesystem);

    ~UI();

    void Update();

    void Render();

    bool Init();

    void SetDimensions(int width, int height);

    void SetDensityIndependentPixelRatio(float ratio);

    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int rmlButtonIndex, bool down);

    // Sidebar readout (build identity, ping). The caller formats the text;
    // this layer holds no game or net state. Repeated identical text is
    // ignored, so calling it every frame is fine.
    void SetHudStatusText(const std::string& text);

    // Hull bar fill, 0..1; negative means "no subject" and blanks the bar.
    // Unchanged values are ignored, so calling it every frame is fine.
    void SetHullFraction(float fraction);

    // Fired when the intro dialog's OK is clicked, with the side the player
    // picked. Set before Init(), which is what shows the dialog.
    void SetIntroConfirmCallback(std::function<void(TeamId)> callback);

    // Hides the intro dialog's team picker (multiplayer: the server assigns
    // sides, so there's nothing to choose). Call before Init().
    void SetTeamChoiceEnabled(bool enabled) { m_teamChoiceEnabled = enabled; }

    // Fired while the minimap panel is pressed or dragged, with the cursor as
    // -1..1 across the map in each axis, +Y up. The UI knows the panel's
    // pixels; mapping that to the world is the caller's job.
    void SetMinimapClickCallback(std::function<void(float, float)> callback);

    // Fired by the sidebar's recenter button.
    void SetRecenterCallback(std::function<void()> callback);

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
