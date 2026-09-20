# The driver's Swift half, built into a static archive that winemac.so links.
#
# Wine's makefiles do not know Swift, so this is a plain makefile run on its
# own, before wine's `make`: `libwinemacswift.a` must exist at the winemac.so
# link, and nothing in wine's makefiles orders it (build-macos/README.md).
# build-macos/package-engine.sh refuses to package when the archive is missing
# or newer than the staged winemac.so.
#
# The target triple matches the engine's: x86_64, deployment target 14. The
# Swift runtime is in the dyld shared cache in both slices, so nothing ships
# alongside the archive.

SWIFTC ?= swiftc
TARGET ?= x86_64-apple-macos14
SOURCES = Presenter.swift SurfacePresenter.swift GLPresenter.swift FinalPass.swift Scalers.swift MPVHook.swift

libwinemacswift.a: $(SOURCES)
	$(SWIFTC) -emit-library -static -target $(TARGET) -O -parse-as-library \
	    -module-name WineMacSwift -o $@ $(SOURCES)

clean:
	rm -f libwinemacswift.a WineMacSwift.swiftmodule WineMacSwift.swiftdoc

.PHONY: clean
