/*
 * sevo-native.exe — the Windows face of a native macOS game.
 *
 * The tool's toolmanifest.vdf names this exe as the prefix of a mapped game's
 * launch; Steam's Windows client never runs that prefix (it shell-opens the
 * game's .app instead), so run bare it only exits. The dock shim runs it as
 * `sevo-native.exe --wait <fd>` in the process Steam created for the game,
 * after forking the game natively (ntdll swaps it in for the program Steam
 * asked for): it holds the read end of a pipe the game inherited, the unix
 * descriptor after --wait, and exits at end-of-file, when the game has ended. Steam's launch completes against this process and its tracking of
 * the game is this process's lifetime.
 *
 * Build: make native   (x86_64-w64-mingw32-gcc)
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef long (__cdecl *fd_to_handle_fn)(int fd, unsigned int access, unsigned int attributes, HANDLE *handle);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    (void)instance;
    (void)previous;
    (void)show;
    const wchar_t *wait = command_line ? wcsstr(command_line, L"--wait ") : NULL;
    if (!wait) return 0;
    int fd = _wtoi(wait + 7);
    if (fd <= 2) return 0;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    fd_to_handle_fn to_handle = ntdll ? (fd_to_handle_fn)GetProcAddress(ntdll, "wine_server_fd_to_handle") : NULL;
    if (!to_handle) return 0;
    HANDLE pipe_end = NULL;
    if (to_handle(fd, GENERIC_READ, 0, &pipe_end) != 0 || !pipe_end) return 0;
    char byte;
    DWORD read = 0;
    while (ReadFile(pipe_end, &byte, 1, &read, NULL) && read) {}
    CloseHandle(pipe_end);
    return 0;
}
