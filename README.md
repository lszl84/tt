# tt — Time Tracker

A minimal time-tracking app, built on wxWidgets.

## Features

- Add tasks, start/stop a timer with one click
- Daily / weekly / two-week summary panel with previous-period navigation
- State persisted as JSON in `$XDG_DATA_HOME/tt/state.json` (override via `$TT_DATA_DIR`)
- If the data dir is a git repo, every start/stop is committed and pushed
- Lives in the top bar on [Omarchy](https://omarchy.org) (see below)
- Shows the running task's time in the Xfce panel (see below)

## Dependencies

- A C++20 compiler and CMake ≥ 3.28
- wxWidgets ≥ 3.2 (system package, or auto-fetched and built from source)
- nlohmann/json (auto-fetched)

### Fedora

```bash
sudo dnf install cmake gcc-c++ wxGTK-devel
```

### Arch Linux

```bash
sudo pacman -S cmake gcc wxwidgets-gtk3
```

### Debian / Ubuntu

```bash
sudo apt install cmake g++ libwxgtk3.2-dev
```

If wxWidgets isn't installed, CMake will fetch and build it from source via
`FetchContent` — no extra step required, just a longer first build.

The Xfce panel plugin is built only if the panel and libwnck headers are
installed:

```bash
sudo dnf install xfce4-panel-devel libwnck3-devel              # Fedora
sudo pacman -S xfce4-panel libwnck3                            # Arch Linux
sudo apt install libxfce4panel-2.0-dev libwnck-3-dev           # Debian / Ubuntu
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./tt
```

## Usage

- Type a task name and press **Enter** or click **+ Add**
- Click a task to select it
- Click **Start** to begin tracking; click **Stop** (or **Space**) to stop
- The summary panel shows totals for the chosen range; **<** / **>** cycle ranges

## Omarchy

On a running Omarchy shell, `tt` doesn't open a window. The first launch
installs a bar widget (`com.devmindscape.tt`) into
`~/.config/omarchy/plugins/` and adds it to the right side of the bar; later
launches open its dropdown. `tt --window` (or the launcher's **Open Window**
action) opens the regular window instead. Everywhere else `tt` behaves as
before.

`tt --popup` shows the dropdown, or hides it if it is already showing, and
adds the widget to the bar first if needed. Bind it to a key in
`~/.config/hypr/bindings.lua`:

```lua
o.bind("SUPER + BACKSLASH", "Time tracker", "tt --popup")
```

The dropdown does everything the window does:

- Add a task in the text field (**n** focuses it from the keyboard)
- Click a task, or move to it with **↑/↓** and press **Enter**, to start or
  stop it; **s** stops the running task
- Page through the summary ranges with **<** / **>** (or **←/→**)

While a task runs, the bar shows its elapsed time next to the icon. Turn that
off with `omarchy bar set com.devmindscape.tt showElapsed false --json`.

The widget is a view only: it runs the `tt` commands below and never touches
`state.json` itself. It refreshes whenever `state.json` changes, so the window,
the widget on another monitor and the commands stay in sync. Starts and stops
are pushed with `tt sync`, the same as in the window.

## Xfce panel

The panel plugin shows the Omarchy widget's timer icon, with the running
task's time next to it (`1:02`). Hover over it for the running task and
today's times. A click shows the tt window, hides it if it's the window in
use, or starts tt if it isn't running. In a vertical panel it shows only the
icon, as the Omarchy bar does.

`make install` installs the plugin with tt. The panel finds plugins under
each prefix in the XDG data dirs, so install to one of those, such as
`~/.local`, which needs no root:

```bash
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make install
xfce4-panel -r               # the panel only looks for plugins at startup
xfce4-panel --add=tt-panel   # or Panel Preferences → Items → + → Time Tracker
```

`cmake --install . --component xfce-panel` installs just the plugin.

The plugin runs the `tt` installed with it, or else the first `tt` in `PATH`.
Like the Omarchy widget, it only runs `tt status` and watches `state.json` for
changes. To try it without the panel, run
`make tt-panel-window && ./tt-panel-window`.

## Command line

Each command prints one JSON object:

```
tt status          tasks, the running task and totals per summary range
tt toggle <task>   start <task> (stopping any other), or stop it
tt start <task>    start <task>, stopping any other running task
tt stop            stop the running task
tt add <name>      add a task
tt sync            commit and push the data dir if it is a git repo
```

`<task>` is a task id or its exact name.

## Architecture

```
src/
  main.cpp        — entry: commands, Omarchy bar widget or window
  tt_frame.h/.cpp — main window: input, task list, toggle, summary panel
  data.h/.cpp     — Task / TimeSession model and JSON persistence
  cli.h/.cpp      — JSON commands used by the bar widget
  git_sync.h/.cpp — commit and push of the data dir
  omarchy.h/.cpp  — installs and opens the Omarchy bar widget
omarchy/          — the bar widget (QML), installed to share/tt-omarchy-plugin
xfce/             — the Xfce panel plugin and its icons
```

## License

GPLv3
