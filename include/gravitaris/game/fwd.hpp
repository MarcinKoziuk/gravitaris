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
struct AIStrategy;
struct Team;
struct Bullet;
struct Damageable;
struct NetId;
struct GravitySource;
struct Orbit;
struct ShipLoadout;
struct ResearchAccess;
struct PilotAccount;
struct PilotRef;
struct FactionState;

// upgrade
struct UpgradeDef;
struct UpgradeLevels;
struct ShipStats;
class UpgradeCatalog;

// event
struct GameEvent;
class GameEventQueue;

// net
class ByteWriter;
class ByteReader;
struct EntityState;
struct SnapshotData;

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
class AIStrategySystem;
class DamageSystem;
class LagCompensation;
class DeathSystem;
class FactionSystem;

// spawner
class EntitySpawner;

} // namespace Gravitaris
