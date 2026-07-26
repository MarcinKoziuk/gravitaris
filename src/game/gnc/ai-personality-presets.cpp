#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>

namespace Gravitaris {

void ApplyAIPersonalityPreset(AIPilot& pilot, AIPersonalityPreset preset)
{
    AIPersonality& p = pilot.personality;
    GuidanceParams& g = pilot.guidance;
    FlightControllerParams& f = pilot.flight;

    switch (preset) {
        case AIPersonalityPreset::Balanced:
            // All fields keep AIPersonality/GuidanceParams/FlightControllerParams'
            // own defaults -- this preset exists so "no preset picked" and
            // "explicitly Balanced" are the same, documented tuning.
            p = AIPersonality{};
            g = GuidanceParams{};
            f = FlightControllerParams{};
            break;

        case AIPersonalityPreset::Aggressive:
            p.engageRange = 8000.0;
            p.standoffDistance = 35.0;
            p.fireRange = 400.0;
            p.fireTolerance = 0.13;
            p.evadeRadius = 140.0;
            p.evadeMargin = 1.3;
            p.dangerLookaheadSteps = 90;
            p.decisionInterval = 10;
            p.fireInterval = 5;
            p.reactionJitter = 0.1;
            p.aimJitter = 0.05;
            g.maxSpeed = 110.0;
            g.transitSpeed = 500.0;
            g.flipTime = 0.9;
            g.orbitRadialKp = 0.6;
            g.maxRadialSpeed = 25.0;
            g.touchdownSpeed = 9.0;
            f.headingKp = 7.5;
            f.headingKd = 1.3;
            f.turnDeadband = 0.2;
            f.aimTolerance = 0.3;
            f.minBurnTicks = 10;
            f.minCoastTicks = 8;
            break;

        case AIPersonalityPreset::Cautious:
            p.engageRange = 4500.0;
            p.standoffDistance = 70.0;
            p.fireRange = 300.0;
            p.fireTolerance = 0.08;
            p.evadeRadius = 260.0;
            p.evadeMargin = 2.0;
            p.dangerLookaheadSteps = 150;
            p.decisionInterval = 15;
            p.fireInterval = 10;
            p.reactionJitter = 0.05;
            p.aimJitter = 0.025;
            g.maxSpeed = 65.0;
            g.transitSpeed = 320.0;
            g.flipTime = 1.5;
            g.orbitRadialKp = 0.4;
            g.maxRadialSpeed = 15.0;
            g.touchdownSpeed = 5.5;
            f.headingKp = 5.0;
            f.headingKd = 1.8;
            f.turnDeadband = 0.3;
            f.aimTolerance = 0.4;
            f.minBurnTicks = 20;
            f.minCoastTicks = 18;
            break;

        case AIPersonalityPreset::Sniper:
            p.engageRange = 6000.0;
            p.standoffDistance = 180.0;
            p.fireRange = 450.0;
            p.fireTolerance = 0.04;
            p.evadeRadius = 200.0;
            p.evadeMargin = 1.6;
            p.dangerLookaheadSteps = 120;
            p.decisionInterval = 15;
            // Three-round bursts, tight together, with a real cooldown after --
            // reads as a sniper's controlled burst rather than a steady drip.
            p.burstCount = 3;
            p.burstShotInterval = 5;
            p.fireInterval = 35;
            p.aimJitter = 0.015;
            g.maxSpeed = 70.0;
            g.transitSpeed = 360.0;
            f.aimTolerance = 0.2;
            // Long, settled burns: a sniper spends its transit lining up, not
            // fidgeting with the throttle.
            f.minBurnTicks = 24;
            f.minCoastTicks = 22;
            break;

        case AIPersonalityPreset::Reckless:
            p.engageRange = 7000.0;
            p.standoffDistance = 30.0;
            p.fireRange = 380.0;
            p.fireTolerance = 0.15;
            p.evadeRadius = 120.0;
            p.evadeMargin = 1.2;
            p.dangerLookaheadSteps = 60;
            p.decisionInterval = 12;
            p.fireInterval = 6;
            p.reactionJitter = 0.15;
            p.aimJitter = 0.08;
            p.dangerIgnoreChance = 0.15;
            g.maxSpeed = 120.0;
            g.transitSpeed = 600.0;
            g.flipTime = 0.8;
            g.touchdownSpeed = 10.0; // as close to SAFE_LANDING_SPEED as contact overshoot allows
            f.headingKp = 8.0;
            f.turnDeadband = 0.15;
            f.minBurnTicks = 7;
            f.minCoastTicks = 5;
            break;
    }
}

void ApplyAIStrategyPreset(AIStrategy& strategy, AIPersonalityPreset preset)
{
    AIStrategyWeights& w = strategy.weights;
    w = AIStrategyWeights{};

    switch (preset) {
        case AIPersonalityPreset::Balanced:
            break;

        case AIPersonalityPreset::Aggressive:
            w.dogfight = 1.3;
            w.claim = 0.8;
            w.attackComplex = 1.5;
            w.interceptFreighter = 1.1;
            w.defend = 0.6;
            break;

        case AIPersonalityPreset::Cautious:
            w.dogfight = 0.7;
            w.claim = 1.3;
            w.attackComplex = 0.6;
            w.interceptFreighter = 0.8;
            w.defend = 1.5;
            break;

        // Picks off the soft targets: raids commerce, avoids the complexes
        // that shoot back (StructureDefenseSystem).
        case AIPersonalityPreset::Sniper:
            w.dogfight = 0.9;
            w.claim = 1.0;
            w.attackComplex = 0.7;
            w.interceptFreighter = 1.5;
            w.defend = 0.9;
            break;

        case AIPersonalityPreset::Reckless:
            w.dogfight = 1.5;
            w.claim = 0.7;
            w.attackComplex = 1.6;
            w.interceptFreighter = 1.2;
            w.defend = 0.3;
            break;
    }
}

} // namespace Gravitaris
