// sevo-native-supervisor: owns one native macOS game that Steam started in the bottle.
//
// The dock shim execs this in the child it forks from the process Steam created, and that
// process goes on as wine running sevo-native.exe, the waiter Steam tracks as the game.
// The supervisor:
//   - holds the write end of the waiter's pipe, so the waiter (and Steam's view of the game)
//     lives exactly as long as the session;
//   - spawns the game, native-arch, as the leader of a new process group, so a launcher that
//     hands off to a child or a game that execs itself stays one session;
//   - ends the whole group when the waiter dies (Steam's Stop, the wineserver going down with
//     an app quit or an engine switch) or when it is sent SIGTERM (the app's stop);
//   - publishes the session in <prefix>/.sevo/native-sessions/<own pid>.json for the app.
// None of this depends on the game loading anything, so a hardened game that strips
// DYLD_INSERT_LIBRARIES is supervised the same way.
//
// sevo-native-supervisor --status-fd N --waiter PID --prefix DIR --appid ID --bundle PATH
//                        --cwd DIR -- EXECUTABLE [ARGUMENT...]
// The game gets this process's environment unchanged.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/machine.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum {
    kStopGraceMilliseconds = 10000,
    kKillGraceMilliseconds = 2000,
    kPollMilliseconds = 100,
    kGroupPollSeconds = 1,
};

static char session_path[PATH_MAX];

static void say(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs("sevo-native: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

static int mkdir_p(const char *path) {
    char buffer[PATH_MAX];
    if (snprintf(buffer, sizeof buffer, "%s", path) >= (int)sizeof buffer) return -1;
    for (char *at = buffer + 1; *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return -1;
        *at = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void put_json_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *at = (const unsigned char *)text; *at; at++) {
        if (*at == '"' || *at == '\\') fprintf(out, "\\%c", *at);
        else if (*at < 0x20) fprintf(out, "\\u%04x", *at);
        else fputc(*at, out);
    }
    fputc('"', out);
}

// Written to a temporary name and renamed, so a reader sees a whole file or none.
static void write_session(const char *prefix, const char *appid, pid_t game, const char *executable,
                          const char *bundle) {
    char directory[PATH_MAX], temporary[PATH_MAX];
    snprintf(directory, sizeof directory, "%s/.sevo/native-sessions", prefix);
    if (mkdir_p(directory) != 0) {
        say("cannot create %s: %s", directory, strerror(errno));
        return;
    }
    snprintf(session_path, sizeof session_path, "%s/%d.json", directory, (int)getpid());
    snprintf(temporary, sizeof temporary, "%s/.%d.json.tmp", directory, (int)getpid());
    FILE *out = fopen(temporary, "w");
    if (!out) {
        say("cannot write %s: %s", temporary, strerror(errno));
        session_path[0] = '\0';
        return;
    }
    fputs("{\"version\":1,\"appid\":", out);
    put_json_string(out, appid);
    fprintf(out, ",\"supervisor\":%d,\"game\":%d,\"pgid\":%d,\"executable\":", (int)getpid(), (int)game, (int)game);
    put_json_string(out, executable);
    fputs(",\"bundle\":", out);
    put_json_string(out, bundle);
    fprintf(out, ",\"started\":%ld}\n", (long)time(NULL));
    if (fclose(out) != 0 || rename(temporary, session_path) != 0) {
        say("cannot publish %s: %s", session_path, strerror(errno));
        unlink(temporary);
        session_path[0] = '\0';
    }
}

static void remove_session(void) {
    if (session_path[0]) unlink(session_path);
    session_path[0] = '\0';
}

static int spawn_game(const char *executable, char *const *arguments, pid_t *pid) {
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t defaults, empty;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setsigmask(&attributes, &empty);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK |
                                              POSIX_SPAWN_CLOEXEC_DEFAULT);
    int arm64 = 0;
    size_t size = sizeof arm64;
    if (sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64) {
        cpu_type_t preferred[] = { CPU_TYPE_ARM64, CPU_TYPE_X86_64 };
        size_t set = 0;
        posix_spawnattr_setbinpref_np(&attributes, 2, preferred, &set);
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd <= 2; fd++) posix_spawn_file_actions_addinherit_np(&actions, fd);
    int error = posix_spawn(pid, executable, &actions, &attributes, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return error;
}

static int group_alive(pid_t group) {
    return killpg(group, 0) == 0 || errno == EPERM;
}

static void reap(pid_t game, int *status, int *reaped) {
    if (*reaped) return;
    int result;
    if (waitpid(game, &result, WNOHANG) == game) {
        *status = result;
        *reaped = 1;
    }
}

// Waits up to `milliseconds` for the group to empty, reaping the leader on the way.
static int wait_for_group(pid_t group, pid_t game, int *status, int *reaped, int milliseconds) {
    for (int waited = 0; waited <= milliseconds; waited += kPollMilliseconds) {
        reap(game, status, reaped);
        if (!group_alive(group)) return 1;
        usleep(kPollMilliseconds * 1000);
    }
    return 0;
}

static void stop_group(pid_t group, pid_t game, int *status, int *reaped, const char *why) {
    say("%s: ending the game's process group %d", why, (int)group);
    killpg(group, SIGTERM);
    if (wait_for_group(group, game, status, reaped, kStopGraceMilliseconds)) return;
    say("process group %d outlived SIGTERM by %d s: SIGKILL", (int)group, kStopGraceMilliseconds / 1000);
    killpg(group, SIGKILL);
    wait_for_group(group, game, status, reaped, kKillGraceMilliseconds);
}

static int exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int usage(void) {
    say("usage: sevo-native-supervisor --status-fd N --waiter PID --prefix DIR --appid ID --bundle PATH "
        "--cwd DIR -- EXECUTABLE [ARGUMENT...]");
    return 64;
}

int main(int argc, char **argv) {
    int status_fd = -1;
    pid_t waiter = 0;
    const char *prefix = NULL, *appid = NULL, *bundle = NULL, *cwd = NULL;
    int at = 1;
    for (; at < argc; at++) {
        if (strcmp(argv[at], "--") == 0) {
            at++;
            break;
        }
        if (at + 1 >= argc) return usage();
        const char *option = argv[at], *value = argv[++at];
        if (strcmp(option, "--status-fd") == 0) status_fd = atoi(value);
        else if (strcmp(option, "--waiter") == 0) waiter = (pid_t)atoi(value);
        else if (strcmp(option, "--prefix") == 0) prefix = value;
        else if (strcmp(option, "--appid") == 0) appid = value;
        else if (strcmp(option, "--bundle") == 0) bundle = value;
        else if (strcmp(option, "--cwd") == 0) cwd = value;
        else return usage();
    }
    if (at >= argc || waiter <= 0 || !prefix || !appid || !bundle) return usage();
    const char *executable = argv[at];
    char *const *arguments = argv + at;

    // The pipe stays open in this process alone: the game and everything it starts never
    // hold the waiter.
    if (status_fd > 2) fcntl(status_fd, F_SETFD, FD_CLOEXEC);

    int queue = kqueue();
    if (queue < 0) {
        say("kqueue: %s", strerror(errno));
        return 70;
    }
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    struct kevent changes[4];
    EV_SET(&changes[0], waiter, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    EV_SET(&changes[1], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&changes[2], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&changes[3], SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(queue, changes, 4, NULL, 0, NULL) != 0) {
        say("Steam's waiter %d is already gone: the game is not started", (int)waiter);
        return 0;
    }

    if (cwd && chdir(cwd) != 0) say("cannot enter %s: %s", cwd, strerror(errno));
    pid_t game = 0;
    int error = spawn_game(executable, arguments, &game);
    if (error != 0) {
        say("cannot start %s: %s", executable, strerror(error));
        return 127;
    }
    struct kevent watch;
    EV_SET(&watch, game, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    int game_watched = kevent(queue, &watch, 1, NULL, 0, NULL) == 0;
    write_session(prefix, appid, game, executable, bundle);
    say("app %s: %s runs as pid %d in its own process group", appid, executable, (int)game);

    int status = 0, reaped = 0, leader_gone = !game_watched;
    for (;;) {
        struct timespec poll = { kGroupPollSeconds, 0 };
        struct kevent event;
        int count = kevent(queue, NULL, 0, &event, 1, leader_gone ? &poll : NULL);
        if (count < 0 && errno != EINTR) {
            say("kevent: %s", strerror(errno));
            stop_group(game, game, &status, &reaped, "lost the event queue");
            break;
        }
        if (count > 0 && event.filter == EVFILT_PROC && (pid_t)event.ident == waiter) {
            stop_group(game, game, &status, &reaped, "Steam's waiter ended");
            break;
        }
        if (count > 0 && event.filter == EVFILT_SIGNAL) {
            char why[48];
            snprintf(why, sizeof why, "asked to stop (signal %d)", (int)event.ident);
            stop_group(game, game, &status, &reaped, why);
            break;
        }
        if (count > 0 && event.filter == EVFILT_PROC && (pid_t)event.ident == game) {
            reap(game, &status, &reaped);
            if (!leader_gone && group_alive(game)) {
                say("pid %d exited and its process group carries on: the session lasts until the group is empty",
                    (int)game);
            }
            leader_gone = 1;
        }
        reap(game, &status, &reaped);
        if (leader_gone && !group_alive(game)) break;
    }
    remove_session();
    say("app %s: session over (status %d)", appid, exit_code(status));
    return exit_code(status);
}
