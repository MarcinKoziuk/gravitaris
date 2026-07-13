#pragma once

namespace Gravitaris {

// The per-tick control state of a ship: which actions are being requested this
// tick. Shared by the Controls component (the resolved state the sim acts on)
// and by InputCommand (a tick-stamped command sitting in an InputQueue). Fits
// in one byte; see input-command.hpp for the packed replay/wire serialization.
struct ControlFlags {
    bool thrustForward : 1 = false;
    bool rotateLeft : 1 = false;
    bool rotateRight : 1 = false;
    bool firePrimary : 1 = false;
    bool fireSecondary : 1 = false;
};

// Resolved control state for an entity, written each tick by InputSystem from
// the entity's InputQueue and consumed by ShipControlsSystem.
struct Controls {
    ControlFlags actionFlags;
};

} // namespace Gravitaris
