#pragma once
#include "StdInclude.hpp"

// The game's cursor while the editor is shown. BO2 shows and hides the system cursor with ShowCursor, always putting
// its count back to exactly 0 or -1 in a loop (0x8D86F0, 0x4D1750: as many calls as it's off by). While the editor is
// up the game's calls don't reach Windows: a hide doesn't hide it, and each call answers the count the game's loop
// waits for (0 for a show, -1 for a hide), so the loop ends at once. With the editor hidden they go through as they
// were. Done by the game's import of ShowCursor (user32), so only the game's own calls are changed.
namespace IWXMVM::T6::Cursor
{
    void Install();
}  // namespace IWXMVM::T6::Cursor
