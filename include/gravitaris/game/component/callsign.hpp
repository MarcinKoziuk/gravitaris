#pragma once

#include <string>

namespace Gravitaris {

// A name somebody can type at a ship: what a human joined under
// (ClientHelloPacket::name, or the intro dialog's field in single-player), or
// an AI leader's handle. Carried by the ship rather than looked up per peer,
// so anything holding an entity can name its pilot -- which is what lets the
// chat cheats address one another (/heal @bob, /tp bob).
//
// Only on ships worth addressing: the humans, and the one leader per AI
// faction. A dogfighter out of a /spawn wave has no name and needs none.
// Stable membership: set at spawn, and a respawn re-attaches it to the fresh
// hull; nothing adds or removes it in flight.
//
// Replication class: not replicated. Cheats resolve names server-side, which
// is where they run; a client that wants nameplates will need this on the
// wire, and that's the change to make then rather than now.
struct Callsign {
    std::string name;
};

} // namespace Gravitaris
