/* The panel's module entry points. XFCE_PANEL_PLUGIN_REGISTER only compiles
 * as C, so it lives here and calls into panel-plugin.cpp. */

#include <libxfce4panel/libxfce4panel.h>

void tt_panel_construct (XfcePanelPlugin *plugin);

XFCE_PANEL_PLUGIN_REGISTER (tt_panel_construct);
