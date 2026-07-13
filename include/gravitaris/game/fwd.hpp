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
struct Controls;

// system
class PhysicsSystem;
class ShipControlsSystem;

// spawner
class EntitySpawner;

} // namespace Gravitaris
