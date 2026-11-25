#pragma once

namespace lazysf2::hooks {

/// <summary>What decides whether the system cursor is confined to the game window.</summary>
struct ClipInputs {
    bool fullscreen;   // Display Mode is Fullscreen (not Borderless)
    bool cursor_mode;  // Mouse Pointer is "Match Windows cursor" and the mouse read hook is in place
    bool foreground;   // the game is the active app
    bool iconic;       // the window is minimized
    bool have_rect;    // the client rectangle is known and not empty
};

/// <summary>True when the cursor must be confined to the client area. Borderless and Stock never clip.</summary>
constexpr bool want_clip(const ClipInputs& in) {
    return in.fullscreen && in.cursor_mode && in.foreground && !in.iconic && in.have_rect;
}

}  // namespace lazysf2::hooks
