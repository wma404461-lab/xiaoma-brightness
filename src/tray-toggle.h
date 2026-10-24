#pragma once

// Keep one tray press intact when focus loss arrives before its select callback.
class TrayToggle {
    bool pressStarted = false;
    bool closeOnSelect = false;
public:
    void Reset() { pressStarted = false; closeOnSelect = false; }
    void MouseDown(bool visible) {
        if (pressStarted) closeOnSelect = false; // A new press replaces a cancelled one.
        pressStarted = true;
        closeOnSelect = closeOnSelect || visible;
    }
    void Deactivated(bool visible, bool pressedOwnIcon) {
        if (visible) closeOnSelect = pressedOwnIcon || pressStarted;
    }
    bool Select(bool visible) {
        const bool open = !visible && !closeOnSelect;
        Reset();
        return open;
    }
};
