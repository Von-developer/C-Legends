# C-Legends


Open Terminal in that folder and compile:

bashg++ -std=c++17 -Wall -o SystemLogAnalyzer \
  main.cpp Event.cpp LoginEvent.cpp ErrorEvent.cpp WarningEvent.cpp \
  ActivityEvent.cpp LogManager.cpp FileHandler.cpp ReportGenerator.cpp


Run with Mac.log:

bash./SystemLogAnalyzer Mac.log

Run with default CSV:

./SystemLogAnalyzer


Note: You need Xcode Command Line Tools installed (xcode-select --install) for g++ to work on Mac.
