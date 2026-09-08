# sevo-steamstub — the in-bottle Steamworks proxy for natively-run NW.js games.
#
#   make          both bitnesses
#   make 64       sevo-steamstub.exe
#   make 32       sevo-steamstub32.exe
#
# Needs mingw-w64 (`brew install mingw-w64`).

CC64 := x86_64-w64-mingw32-gcc
CC32 := i686-w64-mingw32-gcc
# GetProcAddress hands back a FARPROC that every caller must cast, and every
# path here is a MAX_PATH buffer the compiler cannot prove short enough.
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter \
          -Wno-cast-function-type -Wno-format-truncation -mwindows
LIBS := -lws2_32

.PHONY: all 64 32 clean

all: 64 32

64: sevo-steamstub.exe
32: sevo-steamstub32.exe

sevo-steamstub.exe: sevo_steam_stub.c
	$(CC64) $(CFLAGS) -o $@ $< $(LIBS)

sevo-steamstub32.exe: sevo_steam_stub.c
	$(CC32) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f sevo-steamstub.exe sevo-steamstub32.exe
