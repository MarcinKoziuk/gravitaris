#pragma once

namespace Gravitaris {

// A Newtonian attractor. Its gravitational mass lives here, deliberately
// decoupled from the Chipmunk body mass: celestial bodies are kinematic, whose
// physical mass is infinite (so collisions and thrust never shift them), yet
// gravity still needs a finite mass to pull ships with. ApplyGravity,
// TrajectoryPredictor and the AI's well-picking all read this rather than
// cpBodyGetMass, which would report infinity for a kinematic body.
//
// Replicated (ADR 0001 constraint 2): a client needs source positions/masses
// to reproduce the same gravity field.
struct GravitySource {
    double mass = 0.0;
    float multiplier = 1.f;
};

} // namespace Gravitaris
