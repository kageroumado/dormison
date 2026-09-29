# sevo-fpsunlock — Genshin Impact's frame-rate cap raised from beside the game.
#
#   make          sevo-fpsunlock.exe and sevo-fpsunlock-stub.dll
#
# The stub is genshin-fps-unlock's UnlockerStub (MIT, stub/LICENSE), built with
# mingw-w64 and linked static so it depends on nothing but Windows; the
# program beside it is Sevoflurane's own. Needs mingw-w64 (`brew install mingw-w64`).

CC := x86_64-w64-mingw32-gcc
CXX := x86_64-w64-mingw32-g++
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS := -std=c++23 -O2 -w

.PHONY: all clean

all: sevo-fpsunlock.exe sevo-fpsunlock-stub.dll

sevo-fpsunlock.exe: sevo_fpsunlock.c
	$(CC) $(CFLAGS) -mconsole -s -o $@ $<

sevo-fpsunlock-stub.dll: stub/dllmain.o stub/Utils.o stub/Zydis.o
	$(CXX) -shared -static -s -o $@ $^ -lntdll -ldbghelp -luser32

stub/%.o: stub/%.cpp stub/Utils.h stub/Zydis.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

stub/Zydis.o: stub/Zydis.c stub/Zydis.h
	$(CC) -O2 -w -c $< -o $@

clean:
	rm -f sevo-fpsunlock.exe sevo-fpsunlock-stub.dll stub/*.o
