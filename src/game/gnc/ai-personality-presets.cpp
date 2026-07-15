#include <gravitaris/game/component/ai-pilot.hpp>
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
            p.engageRange = 650.0;
            p.standoffDistance = 35.0;
            p.fireRange = 400.0;
            p.fireTolerance = 0.16;
            p.evadeRadius = 70.0;
            p.evadeMargin = 1.3;
            p.dangerLookaheadSteps = 90;
            p.decisionInterval = 10;
            p.fireInterval = 9;
            p.reactionJitter = 0.1;
            p.aimJitter = 0.06;
            g.maxSpeed = 110.0;
            g.flipTime = 0.9;
            g.orbitRadialKp = 0.6;
            g.maxRadialSpeed = 25.0;
            f.headingKp = 7.5;
            f.headingKd = 1.3;
            f.turnDeadband = 0.2;
            f.aimTolerance = 0.3;
            break;

        case AIPersonalityPreset::Cautious:
            p.engageRange = 350.0;
            p.standoffDistance = 70.0;
            p.fireRange = 300.0;
            p.fireTolerance = 0.10;
            p.evadeRadius = 130.0;
            p.evadeMargin = 2.0;
            p.dangerLookaheadSteps = 150;
            p.decisionInterval = 15;
            p.fireInterval = 17;
            p.reactionJitter = 0.05;
            p.aimJitter = 0.03;
            g.maxSpeed = 65.0;
            g.flipTime = 1.5;
            g.orbitRadialKp = 0.4;
            g.maxRadialSpeed = 15.0;
            f.headingKp = 5.0;
            f.headingKd = 1.8;
            f.turnDeadband = 0.3;
            f.aimTolerance = 0.4;
            break;

        case AIPersonalityPreset::Sniper:
            p.engageRange = 550.0;
            p.standoffDistance = 180.0;
            p.fireRange = 450.0;
            p.fireTolerance = 0.05;
            p.evadeRadius = 100.0;
            p.evadeMargin = 1.6;
            p.dangerLookaheadSteps = 120;
            p.decisionInterval = 15;
            p.fireInterval = 20;
            p.aimJitter = 0.02;
            g.maxSpeed = 70.0;
            f.aimTolerance = 0.2;
            break;

        case AIPersonalityPreset::Reckless:
            p.engageRange = 600.0;
            p.standoffDistance = 30.0;
            p.fireRange = 380.0;
            p.fireTolerance = 0.18;
            p.evadeRadius = 60.0;
            p.evadeMargin = 1.2;
            p.dangerLookaheadSteps = 60;
            p.decisionInterval = 12;
            p.fireInterval = 10;
            p.reactionJitter = 0.15;
            p.aimJitter = 0.10;
            p.dangerIgnoreChance = 0.15;
            g.maxSpeed = 120.0;
            g.flipTime = 0.8;
            f.headingKp = 8.0;
            f.turnDeadband = 0.15;
            break;
    }
}

} // namespace Gravitaris
