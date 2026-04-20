# C-Legends


Open Terminal in that folder and compile:
bashg++ -std=c++17 -Wall -o SystemLogAnalyzer \
  main.cpp Event.cpp LoginEvent.cpp ErrorEvent.cpp WarningEvent.cpp \
  ActivityEvent.cpp LogManager.cpp FileHandler.cpp ReportGenerator.cpp
  
4. Run with Mac.log:
bash./SystemLogAnalyzer Mac.log

5. Run with default CSV:
bash./SystemLogAnalyzer
That's it — the menu will appear and you can test all features interactively.

Note: You need Xcode Command Line Tools installed (xcode-select --install) for g++ to work on Mac.
