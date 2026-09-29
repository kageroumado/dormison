/* sevo-fpsunlock — raises Genshin Impact's frame-rate cap from beside the game.
 *
 *   sevo-fpsunlock.exe <fps> [seconds to wait for the game]
 *
 * The cap is a value inside the game: the stub DLL beside this program
 * (sevo-fpsunlock-stub.dll, genshin-fps-unlock's UnlockerStub, MIT) finds it
 * in the il2cpp code and writes the target into it every few milliseconds.
 * This program is how the stub gets into the game and how it learns the
 * target: a shared memory block the stub opens by name, and a WH_GETMESSAGE
 * hook on the game window's thread, which makes Windows (and Wine) load the
 * stub into the game's process. It then keeps the target written and leaves
 * when the game does.
 *
 * Run it in the game's own prefix, on the game's engine, once the game has
 * drawn: an unlocker alive while the game initialises makes it quit.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The stub's name for the block, and the block's layout (its IPCData). */
#define MAPPING_NAME "Global\\2DE95FDC-6AB7-4593-BFE6-760DD4AB422B"
#define MAPPING_SIZE 4096

enum { STATUS_NONE = 0, STATUS_ERROR = 1, STATUS_READY = 2 };

struct __attribute__((aligned(8))) ipc_data {
    signed char status;
    int framerate;
    unsigned char power_save;
    unsigned char use_mobile_ui;
};

static const wchar_t *game_names[] = { L"GenshinImpact.exe", L"YuanShen.exe" };

static void say(const char *what)
{
    fprintf(stderr, "sevo-fpsunlock: %s\n", what);
}

static void say_error(const char *what)
{
    fprintf(stderr, "sevo-fpsunlock: %s failed (%lu)\n", what, GetLastError());
}

/* The game's process id, or 0. */
static DWORD find_game(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry = { .dwSize = sizeof entry };
    DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            for (size_t i = 0; i < sizeof game_names / sizeof *game_names; i++)
                if (!_wcsicmp(entry.szExeFile, game_names[i])) pid = entry.th32ProcessID;
        } while (!pid && Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

struct window_search { DWORD pid; HWND found; };

static BOOL CALLBACK match_window(HWND hwnd, LPARAM lparam)
{
    struct window_search *search = (struct window_search *)lparam;
    DWORD pid = 0;
    char class_name[64] = "";
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    GetClassNameA(hwnd, class_name, sizeof class_name);
    if (strcmp(class_name, "UnityWndClass") != 0) return TRUE;
    search->found = hwnd;
    return FALSE;
}

/* The game's Unity window, once it exists: the game has drawn by then. */
static HWND game_window(DWORD pid)
{
    struct window_search search = { pid, NULL };
    EnumWindows(match_window, (LPARAM)&search);
    return search.found;
}

static BOOL game_alive(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    DWORD code = 0;
    BOOL alive;
    if (!process) return FALSE;
    alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

/* The stub DLL beside this executable. */
static BOOL stub_path(wchar_t *path, size_t count)
{
    wchar_t *slash;
    if (!GetModuleFileNameW(NULL, path, (DWORD)count)) return FALSE;
    slash = wcsrchr(path, L'\\');
    if (!slash) return FALSE;
    slash[1] = 0;
    return wcscat_s(path, count, L"sevo-fpsunlock-stub.dll") == 0;
}

int main(int argc, char **argv)
{
    int target = argc > 1 ? atoi(argv[1]) : 120;
    int wait_seconds = argc > 2 ? atoi(argv[2]) : 120;
    wchar_t stub[MAX_PATH];
    HANDLE mapping;
    volatile struct ipc_data *ipc;
    HMODULE module;
    HOOKPROC hook_proc;
    HHOOK hook;
    DWORD pid = 0, thread;
    HWND window = NULL;
    int waited;

    if (target < 10 || target > 1000) {
        say("the frame rate must be between 10 and 1000");
        return 2;
    }
    if (!stub_path(stub, MAX_PATH)) {
        say("the stub DLL's path is too long");
        return 2;
    }

    for (waited = 0; waited < wait_seconds; waited++) {
        if ((pid = find_game()) && (window = game_window(pid))) break;
        Sleep(1000);
    }
    if (!window) {
        say("no game window appeared");
        return 3;
    }

    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, MAPPING_SIZE, MAPPING_NAME);
    if (!mapping) { say_error("CreateFileMapping"); return 4; }
    ipc = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!ipc) { say_error("MapViewOfFile"); return 4; }
    ipc->status = STATUS_NONE;
    ipc->framerate = target;
    ipc->power_save = 0;
    ipc->use_mobile_ui = 0;

    module = LoadLibraryW(stub);
    if (!module) { say_error("LoadLibrary of the stub"); return 5; }
    hook_proc = (HOOKPROC)(void *)GetProcAddress(module, "WndProc");
    if (!hook_proc) { say_error("GetProcAddress WndProc"); return 5; }

    thread = GetWindowThreadProcessId(window, NULL);
    hook = SetWindowsHookExW(WH_GETMESSAGE, hook_proc, module, thread);
    if (!hook) { say_error("SetWindowsHookEx"); return 6; }
    /* The hook loads the stub when the thread next reads a message. */
    PostThreadMessageW(thread, WM_NULL, 0, 0);

    for (waited = 0; waited < 20; waited++) {
        if (ipc->status == STATUS_READY) break;
        if (ipc->status == STATUS_ERROR) { say("the stub found no frame-rate cap in this game build"); UnhookWindowsHookEx(hook); return 7; }
        Sleep(1000);
        PostThreadMessageW(thread, WM_NULL, 0, 0);
    }
    if (ipc->status != STATUS_READY) { say("the stub did not answer within 20 s"); UnhookWindowsHookEx(hook); return 7; }
    fprintf(stderr, "sevo-fpsunlock: aiming for %d fps in process %lu\n", target, pid);

    /* The stub reads the block every 62 ms; the value is kept written in case
       something else opened the same block and cleared it. */
    while (game_alive(pid)) {
        ipc->framerate = target;
        Sleep(1000);
    }
    ipc->status = STATUS_NONE;
    UnhookWindowsHookEx(hook);
    UnmapViewOfFile((void *)ipc);
    CloseHandle(mapping);
    return 0;
}
