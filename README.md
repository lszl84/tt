# tt — Time Tracker

A minimal time-tracking app, built on wxWidgets.

## Features

- Add tasks, start/stop a timer with one click
- Daily / weekly / two-week summary panel with previous-period navigation
- State persisted as JSON in `$XDG_DATA_HOME/tt/state.json` (override via `$TT_DATA_DIR`)
- If the data dir is a git repo, every start/stop is committed and pushed
- Lives in the top bar on [Omarchy](https://omarchy.org) (see below)

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
```

## License

GPLv3
