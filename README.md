# C-Legends

`C-Legends` is a C++ system log analyzer with a terminal menu interface.

## Prerequisites

- macOS with Xcode Command Line Tools installed:
  - `xcode-select --install`
- Linux: install libcurl development headers:
  - `sudo apt-get install libcurl4-openssl-dev`

## Build

Recommended (full feature set) from the project root:

```bash
cmake -S . -B build
cmake --build build
```

Legacy single-file build (CLI only):

```bash
g++ -std=c++17 -Wall -o SystemLogAnalyzer \
  main.cpp Event.cpp LoginEvent.cpp ErrorEvent.cpp WarningEvent.cpp \
  ActivityEvent.cpp LogManager.cpp FileHandler.cpp ReportGenerator.cpp
```

## Run

- Analyze `Mac.log`:
  - `./build/log_analyzer Mac.log`
- Run with the default sample file (`logs.csv`):
  - `./build/log_analyzer`
- Serve the live dashboard (HTTP + WebSocket):
  - `./build/log_analyzer --serve Mac.log`

## UI Dashboard

Open `dashboard/index.html` in a browser for demo data, or run `--serve` and visit
`http://localhost:8080` for live data and WebSocket updates.
Legacy demo assets live under `dashboard/js/` and are not used by the main UI.
