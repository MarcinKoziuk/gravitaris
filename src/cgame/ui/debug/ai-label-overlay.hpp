#pragma once

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

class CGame;

// World-anchored "what is this ship doing" labels above every AI ship and
// freighter, in team color: the AI debug tab's table read in situ, so a bad
// decision can be watched instead of correlated with a row. Toggled with F2,
// independently of the dev overlay.
void DrawAiLabelOverlay(CGame& game, const Magnum::Vector2& uiSize);

} // namespace Gravitaris
