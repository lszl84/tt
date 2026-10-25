#pragma once

// Omarchy (https://omarchy.org) integration: on a running Omarchy shell, tt
// lives in the top bar as a widget instead of opening a window.

// True if an Omarchy shell is running in this session.
bool OmarchyShellAvailable();

// Install or update the bar widget and make sure it is in the bar. A widget
// that just joined the bar stays shut; one already there opens its dropdown.
// With `togglePopup` the dropdown is shown if hidden (even for a new widget)
// and hidden if shown. Returns false if any step failed.
bool RunOmarchyLauncher(bool togglePopup);
