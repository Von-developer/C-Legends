# C-Legends

`C-Legends` is a C++ system log analyzer with a terminal menu interface.

## Prerequisites

- macOS with Xcode Command Line Tools installed:
  - `xcode-select --install`

## Build

From the project root, compile with:

```bash
g++ -std=c++17 -Wall -o SystemLogAnalyzer \
  main.cpp Event.cpp LoginEvent.cpp ErrorEvent.cpp WarningEvent.cpp \
  ActivityEvent.cpp LogManager.cpp FileHandler.cpp ReportGenerator.cpp
```

## Run

- Analyze `Mac.log`:
  - `./SystemLogAnalyzer Mac.log`
- Run with the default sample file (`logs.csv`):
  - `./SystemLogAnalyzer`

## UI Dashboard (Grafana)

The UI dashboard is powered by Grafana, backed by the C++ engine’s Prometheus metrics.

```bash
docker compose up --build
```

Then open http://localhost:3000 (admin / clegends). The dashboard is auto-provisioned.
