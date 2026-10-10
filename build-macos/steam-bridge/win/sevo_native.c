/*
 * sevo-native.exe — the command line of the Steam Play tool for macOS builds.
 *
 * The tool's toolmanifest.vdf names this exe as the prefix of a mapped game's
 * launch. Steam's Windows client never runs it: it shell-opens the game's .app
 * instead, and the dock shim takes that process native. The file exists so the
 * tool directory is complete and so a client build that did run the prefix
 * would do nothing and exit cleanly.
 *
 * Build: make native   (x86_64-w64-mingw32-gcc)
 */
#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;
    return 0;
}
