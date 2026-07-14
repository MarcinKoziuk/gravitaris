#pragma once

namespace Gravitaris {

class Game;

// fs
class IFilesystem;

// resource-common
class ResourceLoader;
template <typename T> class ResourcePtr;

// resource
class Body;

// component
struct RigidBodyDesc;
struct PhysicsRef;
struct Transform;
struct ControlFlags;
struct Controls;
struct InputCommand;
struct InputQueue;
struct AIPilot;
struct Team;

// input
class InputLog;

// nav
class TrajectoryPredictor;

// control
struct FlightControllerParams;

// guidance
struct GuidanceParams;

// system
class PhysicsSystem;
class InputSystem;
class ShipControlsSystem;
class AIPilotSystem;

// spawner
class EntitySpawner;

} // namespace Gravitaris
