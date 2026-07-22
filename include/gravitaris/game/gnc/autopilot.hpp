#pragma once

#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>

namespace Gravitaris {

// Player pilot assist; produces commands like a human would, the sim only
// sees the InputQueue.
enum class AutopilotMode {
    Off,
    KillVelocity, // retro-burn toward zero velocity
    HoldPosition, // hover at the position where engaged
    GotoPoint,    // fly to the goto target and stop
    Orbit,        // circle the heaviest gravity source at the engage radius
};

// A client-side command producer, the same seam keyboard input uses: its
// output feeds FeedInput's InputCommand, nothing here touches the sim
// directly. Reads Transform/PhysicsRef/GravitySource off the registry to
// compute its target velocity, but never mutates them.
class Autopilot {
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;

    AutopilotMode m_mode = AutopilotMode::Off;
    Magnum::Math::Vector2<double> m_anchor;
    FlightControllerParams m_flightParams;

    GuidanceParams m_guidanceParams;
    Magnum::Math::Vector2<double> m_gotoTarget;
    Magnum::Math::Vector2<double> m_orbitCenter;
    double m_orbitMass = 0.0;
    double m_orbitRadius = 0.0;
    double m_orbitDirection = 1.0;

    struct GravitySource {
        Magnum::Math::Vector2<double> pos;
        double mass;
    };
    std::optional<GravitySource> FindHeaviestGravitySource();

public:
    Autopilot(flecs::world& registry, PhysicsSystem& physicsSystem);

    [[nodiscard]] AutopilotMode GetMode() const { return m_mode; }

    // Engaging HoldPosition captures the player's current position as anchor;
    // engaging Orbit captures the heaviest gravity source and the player's
    // current radius/direction around it. `player` may be a dead/invalid
    // entity, in which case engaging anything but Off is a no-op.
    void SetMode(AutopilotMode mode, std::optional<flecs::entity> player);

    void ToggleMode(AutopilotMode mode, std::optional<flecs::entity> player)
    {
        SetMode(m_mode == mode ? AutopilotMode::Off : mode, player);
    }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetAnchor() const { return m_anchor; }

    FlightControllerParams& GetFlightParams() { return m_flightParams; }

    GuidanceParams& GetGuidanceParams() { return m_guidanceParams; }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetGotoTarget() const { return m_gotoTarget; }

    void SetGotoTarget(const Magnum::Math::Vector2<double>& target) { m_gotoTarget = target; }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetOrbitCenter() const { return m_orbitCenter; }

    [[nodiscard]] double GetOrbitRadius() const { return m_orbitRadius; }

    // This tick's autopilot command, or nullopt when off / no player. Fire
    // bits are false; the caller merges keyboard fire.
    std::optional<ControlFlags> ComputeControls(std::optional<flecs::entity> player);
};

} // namespace Gravitaris
