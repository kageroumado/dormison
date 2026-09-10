# sevo-discord-bridge — the in-bottle relay between Discord's named pipe and
# the Discord client's unix socket on macOS.
#
#   make          sevo-discord-bridge.exe
#
# One 64-bit build serves 32-bit games too: the pipe lives in wineserver, and
# every process in the prefix reaches it whatever its bitness.
#
# Needs mingw-w64 (`brew install mingw-w64`).

CC64 := x86_64-w64-mingw32-gcc
# -mwindows keeps the console away; stderr still reaches the wine log.
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -mwindows
LIBS := -lws2_32 -lntdll

.PHONY: all clean

all: sevo-discord-bridge.exe

sevo-discord-bridge.exe: sevo_discord_bridge.c
	$(CC64) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f sevo-discord-bridge.exe
