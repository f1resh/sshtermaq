// Windows entry point: the UTF-16 command line as UTF-8 arguments for the portable composition root (app/App.h).
#include "app/App.h"
#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <cstdlib>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // DLLs loaded by name from now on (by Windows components too) come from the program's folder, System32 or folders
    // added on purpose, never from the current folder or PATH (1.2.0 review item 11.6). The program's own imports:
    // /DEPENDENTLOADFLAG in CMakeLists.txt.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::vector<std::string> args;
    for (int i = 1; i < __argc; ++i) args.push_back(st::narrow(__wargv[i]));
    return st::runApp(args);
}
