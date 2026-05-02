# tt — Time Tracker

A minimal time-tracking app, built on wxWidgets.

## Features

- Add tasks, start/stop a timer with one click
- Daily / weekly / two-week summary panel with previous-period navigation
- State persisted as JSON in `$XDG_DATA_HOME/tt/state.json` (override via `$TT_DATA_DIR`)

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

## Architecture

```
src/
  main.cpp       — wxApp entry
  tt_frame.h/.cpp — main window: input, task list, toggle, summary panel
  data.h/.cpp     — Task / TimeSession model and JSON persistence
```

## License

GPLv3
