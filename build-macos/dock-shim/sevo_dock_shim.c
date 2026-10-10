// The bottle's face is the app; wine's infrastructure must not reach the
// screen. Four levers, each per process by the Windows exe name:
//
// "Infrastructure" is Steam's own processes by exe name, plus any process
// started with SEVO_QUIET=1: a helper the app runs beside a game.
//
// 1. TransformProcessType is interposed away for Steam's own processes:
//    winemac.drv promotes any wine process that shows a window into the
//    Dock, and there is no demotion API. Games keep the real call, since
//    fullscreen and input capture depend on a promoted process.
//
// 2. With SEVO_SUPPRESS_WINDOWS=1 in the environment (the app sets it for
//    every bottle spawn), NSWindow ordering is swizzled: infrastructure
//    processes cannot put a window on screen at all, since the app renders
//    Steam's UI itself. A game's windows reach the original implementation
//    and order normally.
//
//    Every window is chronicled to ~/Library/Logs/Sevoflurane-windows.log
//    with who/what/when, `suppressed` or `passed`: the audit trail for
//    "what tried to appear, and what actually did". A bottle window nobody
//    can account for is named there by its exe.
//
// 3. A game that becomes a Dock app is named after its Steam title, read from
//    the app manifest beside its install directory, and its Dock tile is the
//    exe's icon shaped the way macOS shapes every app's: through a throwaway
//    bundle that carries the icon, so LaunchServices renders it.
//
// 4. A game the app marks as an NW.js title is exec'd into native NW.js from
//    the constructor, before wine runs (see "The native runner").
//
// 5. A game installed as its macOS build is exec'd from its bundle, with the
//    Steam bridge beside it, when Steam shell-opens the `.app` (see "Native
//    macOS games").
//
// The levers ask "is this Steam's own infrastructure?" at call time, never
// at load time: wine rewrites argv to the Windows command line long after
// this dylib's constructor runs, so a name read in the constructor is the
// unix loader's path and matches nothing.
//
// The universal dylib reaches native infrastructure and translated games
// through DYLD_INSERT_LIBRARIES.
#include <ApplicationServices/ApplicationServices.h>
#include <crt_externs.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <pwd.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <fts.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

// Wine rewrites argv[0] to the program's Windows path for a program named
// the Windows way and leaves the unix path alone for one named that way, so
// the file name is whatever follows the last separator of either kind.
static const char *exe_name(void) {
    char **argv = *_NSGetArgv();
    if (*_NSGetArgc() < 1 || argv == NULL || argv[0] == NULL) return "?";
    const char *exe = argv[0];
    for (const char *at = argv[0]; *at; at++) {
        if (*at == '\\' || *at == '/') exe = at + 1;
    }
    return exe;
}

static int is_steam_infrastructure(void) {
    // A helper the app starts beside a game, whatever its name: SEVO_QUIET=1
    // in its own environment, never a bottle-wide one. The first is the
    // frame-rate unlocker Genshin Impact runs with. Its window, shown the
    // moment it starts, took the foreground from the game, and the game
    // minimized itself into the Dock at its next display-mode change, as you
    // entered the world (Sevoflurane 1.17, Dormison r19, 2026-09-26). It
    // needs no window: the app writes its settings.
    const char *helper = getenv("SEVO_QUIET");
    if (helper && strcmp(helper, "1") == 0) return 1;
    static const char *quiet[] = {
        "steamwebhelper.exe", "steam.exe", "steamservice.exe",
        "steamerrorreporter.exe", "steamerrorreporter64.exe",
        "explorer.exe", "SteamSetup.exe",
        // Steam's boot-time probes. Each orders a window it needs only as a
        // GL/Vulkan context or a PyInstaller stub; the hardware updater's
        // covers the whole screen.
        "gldriverquery.exe", "gldriverquery64.exe",
        "vulkandriverquery.exe", "vulkandriverquery64.exe",
        // The rest of Steam's bin: the Xbox utility at boot, shader
        // pre-caching at a launch, the minidump writer at a crash, the
        // monitor, the streaming client and the launch stubs.
        "steamxboxutil.exe", "steamxboxutil64.exe",
        "fossilize-replay.exe", "fossilize-replay64.exe",
        "x64launcher.exe", "x86launcher.exe", "WriteMiniDump.exe",
        "steam_monitor.exe", "secure_desktop_capture.exe",
        "streaming_client.exe", "drivers.exe",
        "hardwareupdater.exe", "steamsysinfo.exe",
        // Wine's own prefix update, run once when an engine is new to the
        // bottle: its wait dialog has no controls and nothing to answer.
        "wineboot.exe",
    };
    const char *exe = exe_name();
    for (unsigned i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        if (strcasecmp(exe, quiet[i]) == 0) return 1;
    }
    return 0;
}

// MARK: - The chronicle

// The app spawns bottle processes with an environment holding only what
// wine needs, and wine needs no HOME, so the account database supplies the
// home directory when the variable is absent.
static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && *home) return home;
    struct passwd *account = getpwuid(getuid());
    return account ? account->pw_dir : NULL;
}

static void chronicle(const char *verb, id window) {
    const char *home = home_directory();
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/Library/Logs/Sevoflurane-windows.log", home);
    FILE *log = fopen(path, "a");
    if (!log) return;
    const char *title = "";
    const char *class_name = "";
    CGRect frame = CGRectZero;
    if (window) {
        class_name = object_getClassName(window);
        id ns_title = ((id (*)(id, SEL))objc_msgSend)(window, sel_registerName("title"));
        if (ns_title) {
            title = ((const char *(*)(id, SEL))objc_msgSend)(
                ns_title, sel_registerName("UTF8String"));
            if (!title) title = "";
        }
#if defined(__x86_64__)
        // The x86_64 ABI returns CGRect through a hidden pointer.
        ((void (*)(CGRect *, id, SEL))objc_msgSend_stret)(
            &frame, window, sel_registerName("frame"));
#else
        frame = ((CGRect (*)(id, SEL))objc_msgSend)(window, sel_registerName("frame"));
#endif
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm parts;
    localtime_r(&now.tv_sec, &parts);
    fprintf(log, "%02d:%02d:%02d.%03d %s pid=%d %s %s \"%s\" %.0fx%.0f\n",
            parts.tm_hour, parts.tm_min, parts.tm_sec, (int)(now.tv_usec / 1000),
            verb, getpid(), exe_name(), class_name, title,
            frame.size.width, frame.size.height);
    fclose(log);
}

// MARK: - Naming

// The Dock, Activity Monitor and Force Quit read a foreground app's name from
// LaunchServices. winemac.drv never sets one, so every game shows as the wine
// loader, "wine". These SPI set a running app's display name;
// `kLSDefaultSessionID` is -2.
extern CFTypeRef _LSGetCurrentApplicationASN(void);
extern OSStatus _LSSetApplicationInformationItem(int, CFTypeRef, CFStringRef,
                                                 CFTypeRef, CFDictionaryRef *);
extern const CFStringRef _kLSDisplayNameKey;

// The Windows exe name without its extension.
static int exe_stem(char *out, size_t len) {
    const char *exe = exe_name();
    size_t n = strlen(exe);
    if (strcmp(exe, "?") == 0) return 0;
    if (n > 4 && strcasecmp(exe + n - 4, ".exe") == 0) n -= 4;
    if (n == 0 || n >= len) return 0;
    memcpy(out, exe, n);
    out[n] = '\0';
    return 1;
}

static int is_separator(char c) { return c == '\\' || c == '/'; }

// Locates `steamapps<sep>common<sep>` in a Windows path, case-insensitively.
// Returns the start of the install directory segment, or NULL.
static const char *steam_common_segment(const char *path) {
    for (const char *at = path; *at; at++) {
        if (strncasecmp(at, "steamapps", 9) != 0 || !is_separator(at[9])) continue;
        if (strncasecmp(at + 10, "common", 6) != 0 || !is_separator(at[16])) continue;
        return at + 17;
    }
    return NULL;
}

// The value of a quoted `"key"  "value"` line in a Valve KeyValues file;
// the first match wins, which is the app-level key in an app manifest.
static int acf_value(const char *text, const char *key, char *out, size_t len) {
    size_t key_len = strlen(key);
    for (const char *line = text; line;) {
        const char *at = line, *next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
        while (*at == ' ' || *at == '\t') at++;
        if (*at != '"' || strncmp(at + 1, key, key_len) != 0 || at[1 + key_len] != '"') continue;
        at += 2 + key_len;
        while (*at == ' ' || *at == '\t') at++;
        if (*at != '"') return 0;
        const char *end = strchr(++at, '"');
        if (!end || (size_t)(end - at) >= len) return 0;
        memcpy(out, at, end - at);
        out[end - at] = '\0';
        return 1;
    }
    return 0;
}

// Reads a small file whole; `size` receives its length.
static char *read_file(const char *path, size_t limit, size_t *size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *bytes = malloc(limit + 1);
    size_t n = bytes ? fread(bytes, 1, limit, f) : 0;
    fclose(f);
    if (!bytes) return NULL;
    bytes[n] = '\0';
    if (size) *size = n;
    return bytes;
}

// A top-level value of the app manifest whose install directory a Windows
// path runs through. Steam keeps one manifest per app beside the `common`
// directory: `steamapps/appmanifest_<appid>.acf`, with `appid`, `installdir`
// and the display `name` at the top level.
static int steam_manifest_value(const char *path, const char *key, char *out, size_t len) {
    const char *prefix = getenv("WINEPREFIX");
    if (!path || !prefix) return 0;
    const char *segment = steam_common_segment(path);
    if (!segment || !isalpha((unsigned char)path[0]) || path[1] != ':') return 0;

    char installdir[256];
    size_t n = 0;
    while (segment[n] && !is_separator(segment[n]) && n < sizeof(installdir) - 1) {
        installdir[n] = segment[n];
        n++;
    }
    installdir[n] = '\0';
    if (n == 0) return 0;

    // `C:\...\steamapps\` as a unix path under the bottle's drive link.
    char steamapps[1024];
    int written = snprintf(steamapps, sizeof(steamapps), "%s/dosdevices/%c:", prefix,
                           tolower((unsigned char)path[0]));
    if (written < 0 || (size_t)written >= sizeof(steamapps)) return 0;
    size_t pos = (size_t)written;
    for (const char *at = path + 2; at < segment - 7 && pos < sizeof(steamapps) - 1; at++) {
        steamapps[pos++] = is_separator(*at) ? '/' : *at;
    }
    steamapps[pos] = '\0';

    DIR *dir = opendir(steamapps);
    if (!dir) return 0;
    int found = 0;
    struct dirent *entry;
    while (!found && (entry = readdir(dir))) {
        if (strncmp(entry->d_name, "appmanifest_", 12) != 0) continue;
        char manifest[1280];
        snprintf(manifest, sizeof(manifest), "%s/%s", steamapps, entry->d_name);
        char *text = read_file(manifest, 64 * 1024, NULL);
        if (!text) continue;
        char value[256];
        if (acf_value(text, "installdir", value, sizeof(value)) && strcmp(value, installdir) == 0) {
            found = acf_value(text, key, out, len);
        }
        free(text);
    }
    closedir(dir);
    return found;
}

// The Steam title of the program, from the manifest its exe's path names.
static int steam_title(char *out, size_t len) {
    char **argv = *_NSGetArgv();
    if (*_NSGetArgc() < 1 || !argv) return 0;
    return steam_manifest_value(argv[0], "name", out, len);
}

// The name the process shows under: the Steam title when the exe lives in a
// Steam install directory, otherwise the exe stem. Resolved once.
static const char *process_title(void) {
    static char title[256];
    static int resolved = 0;
    if (resolved) return title[0] ? title : NULL;
    resolved = 1;
    if (!steam_title(title, sizeof(title)) && !exe_stem(title, sizeof(title))) title[0] = '\0';
    return title[0] ? title : NULL;
}

// Names the process before it becomes a Dock app. Activity Monitor, Force
// Quit and the window title follow. The Dock tile's label does not: the
// Dock names a tile after the executable the process was launched from,
// whatever the LaunchServices record says, so a tile named after the game
// needs the process launched from a bundle of that name. Runs once; the
// name never changes after.
static void name_process(void) {
    static int named = 0;
    if (named) return;
    named = 1;
    const char *title = process_title();
    if (!title) return;
    CFStringRef name = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
    if (!name) return;
    _LSSetApplicationInformationItem(-2, _LSGetCurrentApplicationASN(),
                                     _kLSDisplayNameKey, name, NULL);
    // The app menu's bold title is the process name as AppKit builds the
    // menu bar; NSProcessInfo's setter for it is private.
    id info = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSProcessInfo"),
                                              sel_registerName("processInfo"));
    SEL set_name = sel_registerName("setProcessName:");
    if (info && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(info, sel_registerName("respondsToSelector:"), set_name)) {
        ((void (*)(id, SEL, id))objc_msgSend)(info, set_name, (id)name);
    }
    CFRelease(name);
}

// MARK: - Dock promotion

static OSStatus sevo_transform(ProcessSerialNumber *psn,
                               ProcessApplicationTransformState state) {
    if (state == kProcessTransformToForegroundApplication) {
        if (is_steam_infrastructure()) return noErr;
        // A game, becoming a Dock app: give it the game's name instead of
        // the wine loader's, then let the promotion through.
        name_process();
        return TransformProcessType(psn, state);
    }
    return TransformProcessType(psn, state);
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)sevo_transform, (const void *)TransformProcessType },
};

// MARK: - Window ordering

// The verdict is settled once and reused: the first window a process orders
// comes long after wine has rewritten argv, and the name never changes
// again. A game pays one name comparison for its whole run.
static int verdict = -1;

static int suppressing(void) {
    if (verdict < 0) verdict = is_steam_infrastructure();
    return verdict;
}

// The overrides go on `WineWindow`, winemac.drv's own NSWindow subclass.
// Never override NSWindow itself: replacing an AppKit implementation
// process-wide leaves a wine process unable to put up any window at all,
// even with every ordering entry point chained to its original.
//
// A suppressed call returns without reaching the superclass. An allowed one
// is forwarded with `objc_msgSendSuper`, which is what `[super orderFront:]`
// compiles to, so AppKit sees exactly the call winemac.drv made.
static Class wine_window_class;

static void forward_to_super(id self, SEL selector, void *first, void *second) {
    struct objc_super target = { self, class_getSuperclass(wine_window_class) };
    ((void (*)(struct objc_super *, SEL, void *, void *))objc_msgSendSuper)(
        &target, selector, first, second);
}

// A window that reaches the screen is chronicled too: `passed` names its
// process in the log when a window turns up that nobody expected. The line
// costs an fopen per ordering call, and a window orders when it is shown
// rather than every frame.
static void allow(id self, SEL selector, void *first, void *second) {
    chronicle("passed", self);
    forward_to_super(self, selector, first, second);
}

static void sevo_order_window(id self, SEL _cmd, long place, long other) {
    if (suppressing()) {
        chronicle("suppressed", self);
        return;
    }
    allow(self, _cmd, (void *)place, (void *)other);
}

static void sevo_order_front(id self, SEL _cmd, id sender) {
    if (suppressing()) {
        chronicle("suppressed", self);
        return;
    }
    allow(self, _cmd, sender, NULL);
}

static void sevo_order_regardless(id self, SEL _cmd) {
    if (suppressing()) {
        chronicle("suppressed", self);
        return;
    }
    allow(self, _cmd, NULL, NULL);
}

// `-[WineWindow makeKeyAndOrderFront:]` is winemac.drv's own override, so
// there is no superclass call to make: it routes through
// `-orderBelow:orAbove:activate:`, which orders by `-orderFront:` and
// `-orderWindow:relativeTo:`. Suppressing those covers it; this one is
// only chronicled so the audit trail shows the ask.
static IMP original_key_and_order_front;

static void sevo_key_and_order_front(id self, SEL _cmd, id sender) {
    chronicle(suppressing() ? "asked" : "passed", self);
    ((void (*)(id, SEL, id))original_key_and_order_front)(self, _cmd, sender);
}

static void override(const char *name, IMP implementation, const char *types) {
    class_addMethod(wine_window_class, sel_registerName(name), implementation, types);
}

// MARK: - Window title

// The title bar shows the Steam title whatever the game titles its window
// (a build number, a codename). Only the NSWindow title is swapped; the
// Win32 window text the game set is untouched.
static IMP original_set_title;

static void sevo_set_title(id self, SEL _cmd, id title) {
    const char *steam = suppressing() ? NULL : process_title();
    if (steam && title) {
        CFStringRef replacement = CFStringCreateWithCString(NULL, steam, kCFStringEncodingUTF8);
        if (replacement) {
            ((void (*)(id, SEL, id))original_set_title)(self, _cmd, (id)replacement);
            CFRelease(replacement);
            return;
        }
    }
    ((void (*)(id, SEL, id))original_set_title)(self, _cmd, title);
}

// MARK: - Icon

// winemac.drv hands the Dock the exe's icon as it is: a square raster next
// to tiles macOS has shaped. LaunchServices applies the shape to app
// bundles, so the icon is written into a throwaway bundle in the user's
// caches and the bundle's icon is asked for. Every size the Dock uses comes
// back shaped, with the platter behind transparent content.
static const char *safe_component(const char *title, char *out, size_t len) {
    size_t n = 0;
    for (const char *at = title; *at && n < len - 1; at++) {
        out[n++] = (*at == '/' || *at == ':') ? '-' : *at;
    }
    out[n] = '\0';
    return out;
}

static int mkdir_p(const char *path) {
    char buffer[1280];
    size_t n = strlen(path);
    if (n >= sizeof(buffer)) return -1;
    memcpy(buffer, path, n + 1);
    for (char *at = buffer + 1; *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return -1;
        *at = '/';
    }
    return (mkdir(buffer, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

// Without an executable LaunchServices badges the icon as unlaunchable, so
// the bundle carries a copy of one that does nothing.
static int copy_executable(const char *from, const char *to) {
    size_t size = 0;
    char *bytes = read_file(from, 1 << 20, &size);
    if (!bytes) return 0;
    FILE *f = fopen(to, "w");
    int ok = f && fwrite(bytes, 1, size, f) == size;
    if (f) fclose(f);
    free(bytes);
    return ok && chmod(to, 0755) == 0;
}

static int write_icns(CGImageRef image, const char *path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    if (!url) return 0;
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("com.apple.icns"), 1, NULL);
    CFRelease(url);
    if (!dst) return 0;
    CGImageDestinationAddImage(dst, image, NULL);
    int ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    return ok;
}

// The throwaway bundle for this program, written or refreshed: the plist and
// the executable always, the icon when there is a raster to write. Answers
// the bundle path, kept for the process's lifetime.
static const char *icon_bundle_path(CGImageRef image) {
    static char base[1024];
    const char *home = home_directory();
    const char *title = process_title();
    char stem[256], component[256], path[1280];
    if (!home || !title || !exe_stem(stem, sizeof(stem))) return NULL;
    safe_component(title, component, sizeof(component));
    snprintf(base, sizeof(base), "%s/Library/Caches/Sevoflurane/DockIcons/%s.app", home, component);
    snprintf(path, sizeof(path), "%s/Contents/Resources", base);
    if (mkdir_p(path) != 0) return NULL;
    snprintf(path, sizeof(path), "%s/Contents/MacOS", base);
    if (mkdir_p(path) != 0) return NULL;

    if (image) {
        snprintf(path, sizeof(path), "%s/Contents/Resources/icon.icns", base);
        write_icns(image, path);
    }
    snprintf(path, sizeof(path), "%s/Contents/MacOS/%s", base, stem);
    if (access(path, X_OK) != 0 && !copy_executable("/usr/bin/true", path)) return NULL;
    char plist[2048];
    snprintf(plist, sizeof(plist),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "<key>CFBundleExecutable</key><string>%s</string>\n"
             "<key>CFBundleIconFile</key><string>icon</string>\n"
             "<key>CFBundleIdentifier</key><string>glass.kagerou.sevoflurane.dock-icon</string>\n"
             "<key>CFBundleName</key><string>%s</string>\n"
             "<key>CFBundleDisplayName</key><string>%s</string>\n"
             "<key>CFBundlePackageType</key><string>APPL</string>\n"
             "</dict></plist>\n", stem, title, title);
    snprintf(path, sizeof(path), "%s/Contents/Info.plist", base);
    if (!write_file(path, plist)) return NULL;
    return base;
}

// The system-shaped NSImage for a raster, or nil.
static id shaped_icon(CGImageRef image) {
    const char *base = icon_bundle_path(image);
    if (!base) return nil;
    id workspace = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSWorkspace"),
                                                   sel_registerName("sharedWorkspace"));
    CFStringRef bundle_path = CFStringCreateWithCString(NULL, base, kCFStringEncodingUTF8);
    if (!bundle_path) return nil;
    id icon = ((id (*)(id, SEL, id))objc_msgSend)(workspace, sel_registerName("iconForFile:"),
                                                  (id)bundle_path);
    CFRelease(bundle_path);
    return icon;
}

static CGImageRef largest_image(id images) {
    CGImageRef best = NULL;
    size_t count = ((size_t (*)(id, SEL))objc_msgSend)(images, sel_registerName("count"));
    for (size_t i = 0; i < count; i++) {
        CGImageRef image = ((CGImageRef (*)(id, SEL, size_t))objc_msgSend)(
            images, sel_registerName("objectAtIndex:"), i);
        if (!best || CGImageGetWidth(image) > CGImageGetWidth(best)) best = image;
    }
    return best;
}

static IMP original_set_icon;

// `-[WineApplicationController setApplicationIconFromCGImageArray:]`: the
// driver's own icon goes in first, then the shaped one replaces it, both
// where the driver reads it back when the process becomes a Dock app and on
// NSApp itself for the case where that has already happened.
static void sevo_set_application_icon(id self, SEL _cmd, id images) {
    ((void (*)(id, SEL, id))original_set_icon)(self, _cmd, images);
    if (suppressing() || !images) return;
    CGImageRef largest = largest_image(images);
    id shaped = largest ? shaped_icon(largest) : nil;
    if (!shaped) return;
    ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("setApplicationIcon:"), shaped);
    id app = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"),
                                             sel_registerName("sharedApplication"));
    ((void (*)(id, SEL, id))objc_msgSend)(app, sel_registerName("setApplicationIconImage:"), shaped);
    chronicle("shaped", NULL);
}

static int swizzled;

// This dylib loads before AppKit (insert libraries come first), so the class
// does not exist at constructor time; each image load is another try.
// `WineWindow` arrives with winemac.drv, which is loaded on demand, long
// after AppKit is up.
static void sevo_try_swizzle(const struct mach_header *header, intptr_t slide) {
    (void)header; (void)slide;
    if (swizzled) return;
    wine_window_class = objc_getClass("WineWindow");
    if (!wine_window_class) return;
    swizzled = 1;
    Class controller = objc_getClass("WineApplicationController");
    Method set_icon = controller ? class_getInstanceMethod(
        controller, sel_registerName("setApplicationIconFromCGImageArray:")) : NULL;
    if (set_icon) {
        original_set_icon = method_setImplementation(set_icon, (IMP)sevo_set_application_icon);
    }
    override("orderWindow:relativeTo:", (IMP)sevo_order_window, "v@:qq");
    override("orderFront:", (IMP)sevo_order_front, "v@:@");
    override("orderFrontRegardless", (IMP)sevo_order_regardless, "v@:");
    Method set_title = class_getInstanceMethod(wine_window_class, sel_registerName("setTitle:"));
    if (set_title) original_set_title = method_setImplementation(set_title, (IMP)sevo_set_title);
    Method key_and_order_front = class_getInstanceMethod(
        wine_window_class, sel_registerName("makeKeyAndOrderFront:"));
    if (key_and_order_front) {
        original_key_and_order_front
            = method_setImplementation(key_and_order_front, (IMP)sevo_key_and_order_front);
    }
    // One line per bottle process that can show a window, before the verdict
    // is asked for: proof that the shim is live without a window to point at.
    chronicle("armed", NULL);
}

// MARK: - The native runner

// Some games are Chromium applications wearing a Windows exe: NW.js titles,
// RPG Maker MV and MZ above all. macOS runs the same application natively, so
// the wine process Steam started is replaced by NW.js right here. The pid is
// the same, so Steam's running state, playtime and overlay all follow the
// native process, and the game ending ends Steam's session.
//
// Which games, and with what, the app decides: it writes the answer into the
// per-program env file ntdll reads at process start
// (`<WINEPREFIX>/.sevo/apps/<exe>.env`, `load_sevo_env`). This runs in the
// constructor, before any of wine's own code, so the file is read here too.
//
// The program is argv[1]: wine rewrites argv[0] to the Windows command line
// much later, which is why every other reader in this file waits for a call
// and this one cannot.

// The value of a `KEY=VALUE` line, or NULL. Blank lines and the file's
// `# written by …` header have no `=` at the key's length and fall through.
static char *env_file_value(const char *text, const char *key) {
    size_t key_len = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *next = end ? end + 1 : NULL;
        if (len && line[len - 1] == '\r') len--;
        if (len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            size_t value_len = len - key_len - 1;
            char *value = malloc(value_len + 1);
            if (!value) return NULL;
            memcpy(value, line + key_len + 1, value_len);
            value[value_len] = '\0';
            return value;
        }
        line = next;
    }
    return NULL;
}

// Wine's own internals travel in the environment: the pre-opened wineserver
// socket and the loader's reserved-range description belong to *this*
// process's place in the bottle, and a program started fresh must not adopt
// them.
static int is_wine_internal(const char *entry) {
    static const char *internal[] = {
        "WINESERVERSOCKET=", "WINEPRELOADRESERVE=", "WINELOADERNOEXEC=",
    };
    for (unsigned i = 0; i < sizeof(internal) / sizeof(internal[0]); i++) {
        if (strncmp(entry, internal[i], strlen(internal[i])) == 0) return 1;
    }
    return 0;
}

// A copy of this process's environment without `drop`ped prefixes, with room
// for `extra` additions. The strings are the originals; only the vector is
// new, and the caller frees just that.
static char **filtered_environment(const char **drop, unsigned drop_count, unsigned extra) {
    char **environment = *_NSGetEnviron();
    size_t count = 0;
    while (environment[count]) count++;
    char **out = calloc(count + extra + 1, sizeof(char *));
    if (!out) return NULL;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        int dropped = 0;
        for (unsigned d = 0; d < drop_count && !dropped; d++) {
            if (strncmp(environment[i], drop[d], strlen(drop[d])) == 0) dropped = 1;
        }
        if (!dropped) out[kept++] = environment[i];
    }
    out[kept] = NULL;
    return out;
}

// The environment for a Windows process started beside this one: this
// process's, less `drop`, less wine's own internals (the pre-opened server
// socket and the loader's reserved range belong to this process alone),
// with room for `extra` additions. `count` receives the entries kept.
static char **bottle_environment(const char **drop, unsigned drop_count, unsigned extra, size_t *count) {
    char **environment = filtered_environment(drop, drop_count, extra);
    if (!environment) return NULL;
    size_t n = 0;
    while (environment[n]) n++;
    for (size_t i = 0; i < n;) {
        if (is_wine_internal(environment[i])) {
            environment[i] = environment[--n];
            environment[n] = NULL;
        } else {
            i++;
        }
    }
    *count = n;
    return environment;
}

// The engine directory: this dylib's own, since the app injects it from
// there and the stub is packaged beside it.
static int engine_directory(char *out, size_t len) {
    Dl_info info;
    if (!dladdr((const void *)env_file_value, &info) || !info.dli_fname) return 0;
    size_t n = strlen(info.dli_fname);
    if (n >= len) return 0;
    memcpy(out, info.dli_fname, n + 1);
    char *slash = strrchr(out, '/');
    if (!slash) return 0;
    *slash = '\0';
    return 1;
}

// The native process cannot reach the bottle's steam_api.dll, so a Windows
// stub holds the Steam connection open for it and answers achievement calls
// over loopback. Started before the exec, in the bottle this process is still
// part of; the app decides whether a game needs one.
//
// `directory` is the game's own directory as Windows names it: the stub
// searches it and four levels below for the game's steam_api dll, which it
// loads into itself. Always the 64-bit stub: it hands off to the 32-bit one
// beside it when the dll is 32-bit, as RPG Maker MV's is. The stub exits by
// itself when its client goes away, so nothing reaps it.
static void spawn_steam_stub(const char *appid, const char *directory, const char *port) {
    char engine[1024], stub[1280], loader[1024];
    if (!engine_directory(engine, sizeof(engine))) return;
    snprintf(stub, sizeof(stub), "%s/sevo-steamstub.exe", engine);
    if (access(stub, R_OK) != 0) {
        fprintf(stderr, "sevo-shim: no %s — achievements stay quiet this run\n", stub);
        return;
    }
    uint32_t size = sizeof(loader);
    if (_NSGetExecutablePath(loader, &size) != 0) return;

    static const char *drop[] = {
        "DYLD_INSERT_LIBRARIES=", "SteamAppId=", "SEVO_ENV_FILES=", "SEVO_STEAM_STUB_PORT=",
    };
    size_t count = 0;
    char **environment = bottle_environment(drop, 4, 3, &count);
    if (!environment) return;
    char app_variable[64], port_variable[64];
    snprintf(app_variable, sizeof(app_variable), "SteamAppId=%s", appid);
    environment[count++] = app_variable;
    if (port) {
        snprintf(port_variable, sizeof(port_variable), "SEVO_STEAM_STUB_PORT=%s", port);
        environment[count++] = port_variable;
    }
    // The per-program env file that sent this process native would send the
    // stub native too, so the stub reads none.
    environment[count++] = (char *)"SEVO_ENV_FILES=0";
    environment[count] = NULL;

    char *arguments[] = { loader, stub, (char *)directory, NULL };
    if (!directory) arguments[2] = NULL;
    pid_t child = 0;
    if (posix_spawn(&child, loader, NULL, NULL, arguments, environment) == 0) {
        fprintf(stderr, "sevo-shim: steam stub for app %s started (pid %d)\n", appid, child);
    } else {
        fprintf(stderr, "sevo-shim: could not start the steam stub: %s\n", strerror(errno));
    }
    free(environment);
}

// Copies the file's `SEVO_…` lines into the environment being built. ntdll
// applies them to a wine process and never runs in this one; the native side
// reads `SEVO_STEAM_STUB_PORT` and `SEVO_NWJS_DIR` from its environment.
static void add_sevo_variables(char **environment, size_t *count, const char *text) {
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *next = end ? end + 1 : NULL;
        if (len && line[len - 1] == '\r') len--;
        if (len > 5 && strncmp(line, "SEVO_", 5) == 0 && memchr(line, '=', len)) {
            char *entry = malloc(len + 1);
            if (entry) {
                memcpy(entry, line, len);
                entry[len] = '\0';
                environment[(*count)++] = entry;
            }
        }
        line = next;
    }
    environment[*count] = NULL;
}

// How many `SEVO_…` lines the file has, so the environment vector is sized
// before it is filled.
static unsigned count_sevo_variables(const char *text) {
    unsigned found = 0;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        if (strncmp(line, "SEVO_", 5) == 0) found++;
        line = end ? end + 1 : NULL;
    }
    return found;
}

static void sevo_run_natively(void) {
    char **argv = *_NSGetArgv();
    int argc = *_NSGetArgc();
    const char *prefix = getenv("WINEPREFIX");
    if (argc < 2 || !argv || !argv[1] || !prefix || !*prefix) return;

    const char *exe = argv[1];
    for (const char *at = argv[1]; *at; at++) {
        if (*at == '\\' || *at == '/') exe = at + 1;
    }
    size_t exe_len = strlen(exe);
    char name[256];
    if (exe_len == 0 || exe_len >= sizeof(name)) return;
    for (size_t i = 0; i <= exe_len; i++) name[i] = tolower((unsigned char)exe[i]);

    char path[1400];
    snprintf(path, sizeof(path), "%s/.sevo/apps/%s.env", prefix, name);
    char *text = read_file(path, 64 * 1024, NULL);
    if (!text) return;
    char *runner = env_file_value(text, "SEVO_RUNNER");
    if (!runner || strcmp(runner, "nwjs") != 0) {
        free(runner);
        free(text);
        return;
    }
    free(runner);
    char *nwjs = env_file_value(text, "SEVO_NWJS");
    char *directory = env_file_value(text, "SEVO_NWJS_DIR");
    char *stub = env_file_value(text, "SEVO_STEAM_STUB");
    char *appid = env_file_value(text, "SEVO_STEAM_APPID");
    char *game_dir = env_file_value(text, "SEVO_STEAM_STUB_DIR");
    char *port = env_file_value(text, "SEVO_STEAM_STUB_PORT");

    if (!nwjs || !directory || access(nwjs, X_OK) != 0 || access(directory, X_OK) != 0) {
        // Falling through to wine keeps the game running; the line says why
        // it ran the long way.
        fprintf(stderr, "sevo-shim: %s asks for NW.js, but %s is not runnable — staying in wine\n",
                name, nwjs ? nwjs : "(no runtime)");
        goto done;
    }
    if (stub && strcmp(stub, "1") == 0 && appid) spawn_steam_stub(appid, game_dir, port);
    if (chdir(directory) != 0) {
        fprintf(stderr, "sevo-shim: cannot enter %s: %s — staying in wine\n",
                directory, strerror(errno));
        goto done;
    }

    // NW.js takes the directory holding the wrapper package as its first
    // argument; whatever Steam put on the game's own command line follows.
    char **arguments = calloc((size_t)argc + 2, sizeof(char *));
    if (!arguments) goto done;
    size_t written = 0;
    arguments[written++] = nwjs;
    arguments[written++] = directory;
    for (int i = 2; i < argc; i++) arguments[written++] = argv[i];
    arguments[written] = NULL;

    static const char *drop[] = { "DYLD_INSERT_LIBRARIES=", "WINE", "SEVO_" };
    char **environment = filtered_environment(drop, 3, count_sevo_variables(text));
    if (!environment) {
        free(arguments);
        goto done;
    }
    size_t count = 0;
    while (environment[count]) count++;
    add_sevo_variables(environment, &count, text);
    fprintf(stderr, "sevo-shim: running %s natively with %s\n", name, nwjs);
    fflush(stderr);
    execve(nwjs, arguments, environment);
    fprintf(stderr, "sevo-shim: exec of %s failed: %s — staying in wine\n",
            nwjs, strerror(errno));
    free(environment);
    free(arguments);
done:
    free(text);
    free(nwjs);
    free(directory);
    free(stub);
    free(appid);
    free(game_dir);
    free(port);
}

// MARK: - Native macOS games

// A game installed as its macOS build through Steam Play is a `.app` in a
// Steam library, and Steam launches it by shell-opening the bundle, which
// reaches wine as `explorer.exe "<…>\Game.app"`. That process forks the
// bundle's executable as a native child and goes on as wine running
// `sevo-native.exe --wait`, which lives exactly as long as the game, so
// Steam's launch completes and its tracking holds. Beside the game runs the
// Steam bridge (steam-bridge/DESIGN.md): sevo-steambridge.exe in the bottle,
// started here, and the bridge's steamclient.dylib in the game, reached
// through libsevosteamipc.dylib.

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

// `C:\…` as a unix path under the bottle's drive link; a unix path is kept.
static int unix_path_of(const char *path, char *out, size_t len) {
    const char *prefix = getenv("WINEPREFIX");
    if (path[0] == '/') return snprintf(out, len, "%s", path) < (int)len;
    if (!prefix || !isalpha((unsigned char)path[0]) || path[1] != ':') return 0;
    int written = snprintf(out, len, "%s/dosdevices/%c:", prefix, tolower((unsigned char)path[0]));
    if (written < 0 || (size_t)written >= len) return 0;
    size_t pos = (size_t)written;
    for (const char *at = path + 2; *at; at++) {
        if (pos >= len - 1) return 0;
        out[pos++] = is_separator(*at) ? '/' : *at;
    }
    out[pos] = '\0';
    return 1;
}

// The Windows client writes every depot file as 0644. Anything that starts
// like a Mach-O or a script gets its executable bit, which is what Steam for
// Mac does from the manifest's flags.
static int looks_executable(const char *path) {
    unsigned char magic[4];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(magic, 1, sizeof magic, f);
    fclose(f);
    if (n < 2) return 0;
    if (magic[0] == '#' && magic[1] == '!') return 1;
    if (n < 4) return 0;
    uint32_t word;
    memcpy(&word, magic, 4);
    return word == 0xfeedface || word == 0xfeedfacf || word == 0xcefaedfe || word == 0xcffaedfe ||
           word == 0xcafebabe || word == 0xbebafeca;
}

static void fix_executable_bits(const char *bundle) {
    char *const roots[] = { (char *)bundle, NULL };
    FTS *walk = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!walk) return;
    unsigned fixed = 0;
    FTSENT *entry;
    while ((entry = fts_read(walk))) {
        if (entry->fts_info != FTS_F || (entry->fts_statp->st_mode & S_IXUSR)) continue;
        if (!looks_executable(entry->fts_accpath)) continue;
        if (chmod(entry->fts_accpath, (entry->fts_statp->st_mode & 07777) | 0755) == 0) fixed++;
    }
    fts_close(walk);
    if (fixed) fprintf(stderr, "sevo-shim: made %u files in the bundle executable\n", fixed);
}

// The bundle's main executable, as its Info.plist names it.
static int bundle_executable(const char *bundle, char *out, size_t len) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)bundle, (CFIndex)strlen(bundle), true);
    if (!url) return 0;
    CFBundleRef b = CFBundleCreate(NULL, url);
    CFRelease(url);
    if (!b) return 0;
    CFURLRef exe = CFBundleCopyExecutableURL(b);
    CFRelease(b);
    if (!exe) return 0;
    Boolean ok = CFURLGetFileSystemRepresentation(exe, true, (UInt8 *)out, (CFIndex)len);
    CFRelease(exe);
    return ok;
}

// The hardened runtime strips DYLD_INSERT_LIBRARIES unless the bundle allows
// it, and that variable is how the game finds the bridge.
static void warn_if_hardened(const char *bundle) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)bundle, (CFIndex)strlen(bundle), true);
    SecStaticCodeRef code = NULL;
    CFDictionaryRef info = NULL;
    if (!url) return;
    if (SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess &&
        SecCodeCopySigningInformation(code, kSecCSSigningInformation | kSecCSRequirementInformation, &info) == errSecSuccess) {
        long flags = 0;
        CFNumberRef number = CFDictionaryGetValue(info, kSecCodeInfoFlags);
        if (number) CFNumberGetValue(number, kCFNumberLongType, &flags);
        CFDictionaryRef entitlements = CFDictionaryGetValue(info, kSecCodeInfoEntitlementsDict);
        CFTypeRef allowed = entitlements
            ? CFDictionaryGetValue(entitlements, CFSTR("com.apple.security.cs.allow-dyld-environment-variables"))
            : NULL;
        if ((flags & kSecCodeSignatureRuntime) && !(allowed && CFEqual(allowed, kCFBooleanTrue))) {
            fprintf(stderr, "sevo-shim: %s has the hardened runtime without allow-dyld-environment-variables: "
                    "the game will not find Steam\n", bundle);
        }
    }
    if (info) CFRelease(info);
    if (code) CFRelease(code);
    CFRelease(url);
}

// Replaces the calling process with `executable`, running native: a
// translated process execs a universal binary translated too, and wine is
// x86_64 under Rosetta, so the game would otherwise take its x86_64 slice.
// Returns only on failure.
static int exec_native(const char *executable, char *const *arguments, char *const *environment) {
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC);
    int arm64 = 0;
    size_t size = sizeof arm64;
    if (sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64) {
        cpu_type_t preferred[] = { CPU_TYPE_ARM64, CPU_TYPE_X86_64 };
        size_t set = 0;
        posix_spawnattr_setbinpref_np(&attributes, 2, preferred, &set);
    }
    pid_t pid;
    int error = posix_spawn(&pid, executable, NULL, &attributes, arguments, environment);
    posix_spawnattr_destroy(&attributes);
    return error;
}

// Where the helper publishes the port it bound: %LOCALAPPDATA%\Sevoflurane\
// steambridge-<appid>.port, seen from macOS. The helper picks the port itself
// and owns it from the first moment, so nothing can take it in between.
static int bridge_port_file(const char *prefix, const char *appid, char *out, size_t len) {
    struct passwd *user = getpwuid(getuid());
    if (!user || !user->pw_name) return 0;
    int n = snprintf(out, len, "%s/drive_c/users/%s/AppData/Local/Sevoflurane/steambridge-%s.port",
                     prefix, user->pw_name, appid);
    return n > 0 && (size_t)n < len;
}

// The bridge's helper, started in the bottle this process is still part of.
// It loads the running client's steamclient64.dll and serves the game's
// Steamworks calls on a port of its own choosing; it exits when the game's
// connections close.
static int spawn_steam_bridge(const char *appid, const char *token) {
    char engine[1024], helper[1280], loader[1024];
    if (!engine_directory(engine, sizeof(engine))) return 0;
    snprintf(helper, sizeof(helper), "%s/sevo-steambridge.exe", engine);
    if (access(helper, R_OK) != 0) {
        fprintf(stderr, "sevo-shim: no %s — the game runs without Steam\n", helper);
        return 0;
    }
    uint32_t size = sizeof(loader);
    if (_NSGetExecutablePath(loader, &size) != 0) return 0;

    static const char *drop[] = {
        "DYLD_INSERT_LIBRARIES=", "SteamAppId=", "SteamGameId=", "SEVO_ENV_FILES=", "SEVO_STEAM_BRIDGE_",
    };
    size_t count = 0;
    char **environment = bottle_environment(drop, 5, 5, &count);
    if (!environment) return 0;
    char app_variable[64], game_variable[64], token_variable[96];
    snprintf(app_variable, sizeof(app_variable), "SteamAppId=%s", appid);
    snprintf(game_variable, sizeof(game_variable), "SteamGameId=%s", appid);
    snprintf(token_variable, sizeof(token_variable), "SEVO_STEAM_BRIDGE_TOKEN=%s", token);
    environment[count++] = app_variable;
    environment[count++] = game_variable;
    environment[count++] = token_variable;
    environment[count++] = (char *)"SEVO_ENV_FILES=0";
    environment[count++] = (char *)"SEVO_QUIET=1";
    environment[count] = NULL;

    char *arguments[] = { loader, helper, NULL };
    pid_t child = 0;
    int started = posix_spawn(&child, loader, NULL, NULL, arguments, environment) == 0;
    if (started) fprintf(stderr, "sevo-shim: steam bridge for app %s started (pid %d)\n", appid, child);
    else fprintf(stderr, "sevo-shim: could not start the steam bridge: %s\n", strerror(errno));
    free(environment);
    return started;
}

// The environment a native game gets: what a Finder launch would give it, the
// Steam variables the client set for the launch, and the bridge's own. Nothing
// of wine's or the app's: the client runs with the engine's PATH, the renderer's
// variables and no HOME, and a native app must not inherit any of that.
static char **native_environment(const char *appid, int bridged, const char *ipc, const char *bridge,
                                 const char *port_file, const char *token, const char *prefix, pid_t waiter) {
    char **inherited = *_NSGetEnviron();
    size_t inherited_count = 0;
    while (inherited[inherited_count]) inherited_count++;
    char **out = calloc(inherited_count + 24, sizeof(char *));
    if (!out) return NULL;
    size_t n = 0;
    struct passwd *user = getpwuid(getuid());
    char buffer[1400];
    if (user) {
        snprintf(buffer, sizeof buffer, "HOME=%s", user->pw_dir);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "USER=%s", user->pw_name);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "LOGNAME=%s", user->pw_name);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "SHELL=%s", user->pw_shell);
        out[n++] = strdup(buffer);
    }
    char tmp[PATH_MAX];
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp) > 0) {
        snprintf(buffer, sizeof buffer, "TMPDIR=%s", tmp);
        out[n++] = strdup(buffer);
    }
    snprintf(buffer, sizeof buffer, "__CF_USER_TEXT_ENCODING=0x%X:0:0", (unsigned)getuid());
    out[n++] = strdup(buffer);
    out[n++] = strdup("PATH=/usr/bin:/bin:/usr/sbin:/sbin");
    for (size_t i = 0; i < inherited_count; i++) {
        char *entry = inherited[i];
        if (strncmp(entry, "SteamAppId=", 11) == 0 || strncmp(entry, "SteamGameId=", 12) == 0) continue;
        if (strncmp(entry, "Steam", 5) == 0 || strncmp(entry, "STEAM_", 6) == 0 ||
            strncmp(entry, "ENABLE_VK_LAYER_VALVE", 21) == 0)
            out[n++] = entry;
    }
    snprintf(buffer, sizeof buffer, "SteamAppId=%s", appid);
    out[n++] = strdup(buffer);
    snprintf(buffer, sizeof buffer, "SteamGameId=%s", appid);
    out[n++] = strdup(buffer);
    if (bridged) {
        snprintf(buffer, sizeof buffer, "DYLD_INSERT_LIBRARIES=%s", ipc);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "SEVO_STEAM_BRIDGE_DIR=%s", bridge);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "SEVO_STEAM_BRIDGE_PORT_FILE=%s", port_file);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "SEVO_STEAM_BRIDGE_TOKEN=%s", token);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "SEVO_STEAM_BRIDGE_PREFIX=%s", prefix);
        out[n++] = strdup(buffer);
        snprintf(buffer, sizeof buffer, "SEVO_STEAM_BRIDGE_WAITER_PID=%d", (int)waiter);
        out[n++] = strdup(buffer);
        const char *trace = getenv("SEVO_STEAM_BRIDGE_LOG");
        if (trace && *trace == '1') out[n++] = strdup("SEVO_STEAM_BRIDGE_LOG=1");
    }
    out[n] = NULL;
    return out;
}

static void sevo_run_native_app(void) {
    char **argv = *_NSGetArgv();
    int argc = *_NSGetArgc();
    const char *prefix = getenv("WINEPREFIX");
    if (argc < 3 || !argv || !argv[1] || !argv[2] || !prefix || !*prefix) return;

    const char *exe = argv[1];
    for (const char *at = argv[1]; *at; at++) {
        if (is_separator(*at)) exe = at + 1;
    }
    if (strcasecmp(exe, "explorer.exe") != 0) return;
    if (!ends_with_ci(argv[2], ".app") || !steam_common_segment(argv[2])) return;

    char bundle[1400], executable[1400], appid[32];
    struct stat info;
    if (!unix_path_of(argv[2], bundle, sizeof(bundle)) || stat(bundle, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "sevo-shim: %s is not a bundle on disk — staying in wine\n", argv[2]);
        return;
    }
    if (!steam_manifest_value(argv[2], "appid", appid, sizeof(appid))) {
        const char *inherited = getenv("SteamAppId");
        snprintf(appid, sizeof(appid), "%s", inherited && *inherited ? inherited : "0");
    }
    fix_executable_bits(bundle);
    if (!bundle_executable(bundle, executable, sizeof(executable)) || access(executable, X_OK) != 0) {
        fprintf(stderr, "sevo-shim: %s names no runnable executable — staying in wine\n", bundle);
        return;
    }

    char engine[1024], bridge[1100], ipc[1200], client[1200], port_file[1400], waiter[1300];
    int bridged = 0;
    char token[20] = "";
    if (!engine_directory(engine, sizeof(engine))) return;
    snprintf(waiter, sizeof(waiter), "%s/sevo-native.exe", engine);
    if (access(waiter, R_OK) != 0) {
        fprintf(stderr, "sevo-shim: no %s — staying in wine\n", waiter);
        return;
    }
    snprintf(bridge, sizeof(bridge), "%s/steam-bridge", engine);
    snprintf(ipc, sizeof(ipc), "%s/libsevosteamipc.dylib", bridge);
    snprintf(client, sizeof(client), "%s/steamclient.dylib", bridge);
    if (access(ipc, R_OK) == 0 && access(client, R_OK) == 0) {
        uint8_t random[8];
        arc4random_buf(random, sizeof random);
        for (unsigned i = 0; i < sizeof random; i++) snprintf(token + 2 * i, 3, "%02x", random[i]);
        bridged = bridge_port_file(prefix, appid, port_file, sizeof(port_file)) &&
                  (unlink(port_file), spawn_steam_bridge(appid, token));
    } else {
        fprintf(stderr, "sevo-shim: no steam bridge under %s — the game runs without Steam\n", engine);
    }
    if (bridged) warn_if_hardened(bundle);

    // The game is a child, so the process Steam created lives on: wine runs
    // sevo-native.exe --wait in it, holding the read end of a pipe whose write
    // end the game inherits, and Steam's launch completes and tracks a process
    // that ends when the game does. The game's end of the bargain is in
    // libsevosteamipc.dylib: it exits when this process goes.
    int status_pipe[2];
    if (pipe(status_pipe) != 0) {
        fprintf(stderr, "sevo-shim: pipe: %s — staying in wine\n", strerror(errno));
        return;
    }
    char **arguments = calloc((size_t)argc, sizeof(char *));
    char **environment = native_environment(appid, bridged, ipc, bridge, port_file, token, prefix, getpid());
    if (!arguments || !environment) return;
    size_t written = 0;
    arguments[written++] = executable;
    for (int i = 3; i < argc; i++) arguments[written++] = argv[i];
    arguments[written] = NULL;

    // The bundle's parent is the game's install directory, where its files are.
    char parent[1400];
    snprintf(parent, sizeof(parent), "%s", bundle);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0';

    fflush(stderr);
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "sevo-shim: fork: %s — staying in wine\n", strerror(errno));
        return;
    }
    if (child == 0) {
        close(status_pipe[0]);
        fcntl(status_pipe[1], F_SETFD, 0);
        if (chdir(parent) != 0) fprintf(stderr, "sevo-shim: cannot enter %s: %s\n", parent, strerror(errno));
        int error = exec_native(executable, arguments, environment);
        fprintf(stderr, "sevo-shim: exec of %s failed: %s\n", executable, strerror(error));
        _exit(127);
    }
    close(status_pipe[1]);
    fcntl(status_pipe[0], F_SETFD, 0);
    fprintf(stderr, "sevo-shim: %s runs natively as pid %d (app %s%s); this process waits for it\n",
            executable, (int)child, appid, bridged ? ", with the steam bridge" : "");
    char fd_text[16];
    snprintf(fd_text, sizeof fd_text, "%d", status_pipe[0]);
    setenv("SEVO_NATIVE_WAIT_FD", fd_text, 1);
    setenv("SEVO_QUIET", "1", 1);
    argv[1] = strdup(waiter);
    argv[2] = (char *)"--wait";
    for (int i = 3; i < argc; i++) argv[i] = (char *)"";
}

// MARK: - The owner watch

// SEVO_OWNER_PID names the process that owns this bottle. Nothing inside the
// prefix notices when it dies: the wineserver keeps the registry, the drives
// and every game process alive, and a later launch silently adopts them. So
// each bottle process watches the owner and takes the prefix down with it.

// The engine's wineserver: beside the loader that started this process,
// three levels up from the ntdll a game bundle was pointed at, or under the
// engine directory this dylib itself sits in, which is the only one of the
// three that is always there.
static int wineserver_path(char *out, size_t len) {
    const char *loader = getenv("WINELOADER");
    const char *tree = getenv("SEVO_LOADER_TREE");
    Dl_info self;
    if (loader && *loader) {
        const char *slash = strrchr(loader, '/');
        if (slash) {
            snprintf(out, len, "%.*s/wineserver", (int)(slash - loader), loader);
            if (access(out, X_OK) == 0) return 1;
        }
    }
    if (tree && *tree) {
        char root[1024];
        snprintf(root, sizeof(root), "%s", tree);
        for (int up = 0; up < 3; up++) {
            char *slash = strrchr(root, '/');
            if (!slash) return 0;
            *slash = '\0';
        }
        snprintf(out, len, "%s/bin/wineserver", root);
        if (access(out, X_OK) == 0) return 1;
    }
    if (dladdr((void *)wineserver_path, &self) && self.dli_fname) {
        const char *slash = strrchr(self.dli_fname, '/');
        if (slash) {
            snprintf(out, len, "%.*s/wine/bin/wineserver",
                     (int)(slash - self.dli_fname), self.dli_fname);
            if (access(out, X_OK) == 0) return 1;
        }
    }
    return 0;
}

static void *sevo_owner_watch(void *argument) {
    pid_t owner = (pid_t)(intptr_t)argument;
    int queue = kqueue();
    if (queue < 0) return NULL;
    struct kevent watch;
    EV_SET(&watch, (uintptr_t)owner, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
    if (kevent(queue, &watch, 1, NULL, 0, NULL) < 0) {
        int failure = errno;
        close(queue);
        // ESRCH is the owner already gone, which is the event itself.
        if (failure != ESRCH) return NULL;
    }
    else {
        struct kevent fired;
        int seen = kevent(queue, NULL, 0, &fired, 1, NULL);
        close(queue);
        if (seen != 1) return NULL;
    }

    fprintf(stderr, "sevo:shim owner %d exited — bringing the prefix down\n", (int)owner);
    fflush(stderr);
    char server[1024];
    if (!wineserver_path(server, sizeof(server))) return NULL;
    char *arguments[] = { server, "-k", NULL };
    // The kill runs outside the bottle: with this dylib and the owner still
    // in its environment it would arm a watch of its own on an owner that is
    // already gone, and spawn another kill from that, without end.
    static const char *drop[] = { "DYLD_INSERT_LIBRARIES=", "SEVO_OWNER_PID=" };
    char **environment = filtered_environment(drop, 2, 0);
    if (!environment) return NULL;
    pid_t child;
    if (posix_spawn(&child, server, NULL, NULL, arguments, environment) == 0) {
        waitpid(child, NULL, 0);
    }
    free(environment);
    return NULL;
}

static void sevo_watch_owner(void) {
    const char *owner = getenv("SEVO_OWNER_PID");
    const char *prefix = getenv("WINEPREFIX");
    if (!owner || !*owner || !prefix || !*prefix) return;
    char *end = NULL;
    long pid = strtol(owner, &end, 10);
    if (end == owner || pid <= 0 || pid > INT32_MAX) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, sevo_owner_watch, (void *)(intptr_t)pid) == 0) {
        pthread_detach(thread);
    }
}

__attribute__((constructor)) static void sevo_shim_init(void) {
    // First: a game that runs natively reaches neither the rest of this file
    // nor wine.
    sevo_run_native_app();
    sevo_run_natively();
    sevo_watch_owner();
    const char *mode = getenv("SEVO_SUPPRESS_WINDOWS");
    if (!mode || strcmp(mode, "1") != 0) return;
    _dyld_register_func_for_add_image(sevo_try_swizzle);
}
