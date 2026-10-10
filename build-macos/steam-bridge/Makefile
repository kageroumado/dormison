# The Steam bridge: steamclient.dylib (macOS, universal), sevo-steambridge.exe (mingw)
# and libsevosteamipc.dylib. Everything generated or compiled lands under
# $DORMISON_BUILD/steam-bridge, never in the repository.
#
#   make sdk                 assemble the SDK headers (fetch-sdk.sh)
#   make generate            run the generator (ONLY=SteamUser021,... narrows it)
#   make -j12 mac win ipc native   build the four artifacts into $(OUT)
#   make test-transport      run the macOS transport against fake helpers (no Steam)
#   make test-gen            run the generator fixtures (tests/gen) into $(BUILD)/test-gen
#
# The generator needs libclang's Python binding; `make venv` makes a venv with it under
# $(BUILD) and PYTHON defaults to that venv's interpreter.
#
# `generate` must finish before `mac`/`win` are evaluated, since their source lists come
# from the generator; run it as its own invocation.

BUILD ?= $(or $(DORMISON_BUILD),$(HOME)/dormison-build)/steam-bridge
GEN := $(BUILD)/generated
OBJ := $(BUILD)/obj
OUT := $(BUILD)/out
SDK := $(BUILD)/sdk
VENV := $(BUILD)/venv
PYTHON ?= $(VENV)/bin/python
LIBCLANG_VERSION := 18.1.1
# mingw-w64's Windows headers, for the generator's Windows parse: under the cross gcc's
# sysroot, else Homebrew's location.
MINGW_SYSROOT := $(shell x86_64-w64-mingw32-gcc -print-sysroot 2>/dev/null)
MINGW_INCLUDE ?= $(if $(wildcard $(MINGW_SYSROOT)/x86_64-w64-mingw32/include/windows.h),$(MINGW_SYSROOT)/x86_64-w64-mingw32/include,/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include)
ONLY ?=
HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all sdk venv generate mac win ipc native test-transport test-gen clean

all: mac win ipc native

sdk:
	$(HERE)fetch-sdk.sh $(SDK)

venv: $(VENV)/bin/python

$(VENV)/bin/python:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet libclang==$(LIBCLANG_VERSION)

generate: $(if $(filter $(VENV)/bin/python,$(PYTHON)),$(VENV)/bin/python)
	@mkdir -p $(GEN)
	MINGW_INCLUDE=$(MINGW_INCLUDE) $(PYTHON) $(HERE)gen/gen_bridge.py --sdk-root $(SDK) --out $(GEN) --cache $(BUILD)/parsed.json \
	    --sdk-include sdk $(if $(ONLY),--only $(ONLY))

test-gen: $(if $(filter $(VENV)/bin/python,$(PYTHON)),$(VENV)/bin/python)
	MINGW_INCLUDE=$(MINGW_INCLUDE) $(PYTHON) $(HERE)tests/gen/test_gen.py --work $(BUILD)/test-gen

# --- macOS: the generated proxies plus the runtime, one universal dylib ---
MAC_GEN := $(addprefix $(GEN)/mac/,$(shell cat $(GEN)/mac/sources.txt 2>/dev/null))
MAC_OBJS := $(patsubst $(GEN)/mac/%.cpp,$(OBJ)/mac/%.o,$(MAC_GEN)) $(OBJ)/mac/bridge.o $(OBJ)/mac/transport.o
MAC_ARCH := -arch arm64 -arch x86_64
MACFLAGS := -std=c++17 -O2 $(MAC_ARCH) -fvisibility=hidden -I$(HERE)mac -I$(HERE)shared -I$(BUILD)
MAC_DEPS := $(HERE)mac/proxy_prelude.h $(HERE)shared/wire.h $(HERE)mac/transport.h

$(OBJ)/mac/%.o: $(GEN)/mac/%.cpp $(MAC_DEPS)
	@mkdir -p $(dir $@)
	clang++ $(MACFLAGS) -fno-rtti -w -c $< -o $@

$(OBJ)/mac/bridge.o: $(HERE)mac/bridge.cpp $(MAC_DEPS)
	@mkdir -p $(dir $@)
	clang++ $(MACFLAGS) -Wall -Wextra -c $< -o $@

$(OBJ)/mac/transport.o: $(HERE)mac/transport.c $(HERE)mac/transport.h
	@mkdir -p $(dir $@)
	clang -O2 $(MAC_ARCH) -fvisibility=hidden -Wall -Wextra -c $< -o $@

$(OUT)/steamclient.dylib: $(MAC_OBJS)
	@mkdir -p $(dir $@)
	clang++ $(MAC_ARCH) -dynamiclib -install_name steamclient.dylib -o $@ $^
	codesign -s - -f $@

mac: $(OUT)/steamclient.dylib

# --- Windows: the generated handlers plus the helper, one static PE ---
WIN_GEN := $(addprefix $(GEN)/win/,$(shell cat $(GEN)/win/sources.txt 2>/dev/null))
WIN_OBJS := $(patsubst $(GEN)/win/%.cpp,$(OBJ)/win/%.o,$(WIN_GEN)) $(OBJ)/win/sevo_steambridge.o
WINCXX := x86_64-w64-mingw32-g++
WINFLAGS := -std=c++17 -O2 -I$(HERE)win -I$(HERE)shared
WIN_DEPS := $(HERE)win/dispatch_prelude.h $(HERE)shared/wire.h

$(OBJ)/win/%.o: $(GEN)/win/%.cpp $(WIN_DEPS)
	@mkdir -p $(dir $@)
	$(WINCXX) $(WINFLAGS) -w -c $< -o $@

$(OBJ)/win/sevo_steambridge.o: $(HERE)win/sevo_steambridge.cpp $(WIN_DEPS)
	@mkdir -p $(dir $@)
	$(WINCXX) $(WINFLAGS) -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -c $< -o $@

$(OUT)/sevo-steambridge.exe: $(WIN_OBJS)
	@mkdir -p $(dir $@)
	$(WINCXX) -static -static-libgcc -static-libstdc++ -mwindows -o $@ $^ -lws2_32

win: $(OUT)/sevo-steambridge.exe

$(OUT)/sevo-native.exe: $(HERE)win/sevo_native.c
	@mkdir -p $(dir $@)
	x86_64-w64-mingw32-gcc -O2 -Wall -mwindows -municode -o $@ $<

native: $(OUT)/sevo-native.exe

# --- the ipcserver stand-in a game's libsteam_api asks for Steam's path ---
$(OUT)/libsevosteamipc.dylib: $(HERE)mac/sevo_steam_ipc.c $(HERE)mac/transport.c $(HERE)mac/transport.h
	@mkdir -p $(dir $@)
	clang $(MAC_ARCH) -O2 -Wall -dynamiclib -o $@ $(HERE)mac/sevo_steam_ipc.c $(HERE)mac/transport.c
	codesign -s - -f $@

ipc: $(OUT)/libsevosteamipc.dylib

# --- the transport against fake helpers that stall, vanish and fragment ---
$(OBJ)/tests/transport_test: $(HERE)tests/transport_test.c $(HERE)mac/transport.c $(HERE)mac/transport.h
	@mkdir -p $(dir $@)
	clang -O2 -Wall -Wextra -I$(HERE)mac -o $@ $(HERE)tests/transport_test.c $(HERE)mac/transport.c

test-transport: $(OBJ)/tests/transport_test
	$<

clean:
	trash $(OBJ) $(OUT) 2>/dev/null || true
