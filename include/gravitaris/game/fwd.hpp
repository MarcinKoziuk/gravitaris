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
struct Bullet;
struct Damageable;
struct NetId;
struct GravitySource;
struct Orbit;

// event
struct GameEvent;
class GameEventQueue;

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
class OrbitSystem;
class InputSystem;
class ShipControlsSystem;
class AIPilotSystem;
class DamageSystem;
class DeathSystem;

// spawner
class EntitySpawner;

} // namespace Gravitaris
