#pragma once

// Omarchy (https://omarchy.org) integration: on a running Omarchy shell, tt
// lives in the top bar as a widget instead of opening a window.

// True if an Omarchy shell is running in this session.
bool OmarchyShellAvailable();

// Install or update the bar widget, make sure it is in the bar and open its
// dropdown. Returns false if any step failed.
bool RunOmarchyLauncher();
