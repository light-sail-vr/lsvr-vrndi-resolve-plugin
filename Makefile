# NDI Output Plugin Makefile

# Version management
VERSION_FILE = VERSION
VERSION = $(shell cat $(VERSION_FILE))

# Directories
# Standard (royalty-free) NDI SDK — never the Advanced SDK, which is a
# commercial license. Override for a non-default install, keeping the quotes:
#   make dev NDI_SDK_PATH='"/path/to/NDI SDK for Apple"'
NDI_SDK_PATH ?= "/Library/NDI SDK for Apple"
NDI_INCLUDE = $(NDI_SDK_PATH)/include
NDI_LIB = $(NDI_SDK_PATH)/lib/macOS/libndi.dylib

# Deployment target / architectures.
# macOS 13 is our supported floor (libndi.dylib itself needs only 11.2). Without an
# explicit -mmacosx-version-min the binary is stamped with the build host's OS
# version and refuses to load on anything older (bit us in v1.12: minos 26.0).
# ARCHFLAGS is empty for dev builds (host arch); release packaging passes
# ARCHFLAGS="-arch arm64 -arch x86_64" for a universal binary.
DEPLOYMENT_TARGET ?= 13.0
ARCHFLAGS ?=

# Compiler settings
# third_party/braw holds the vendored (Boost-licensed) Blackmagic RAW API
# header + dispatch shim; the framework itself is resolved at runtime from the
# host app (Resolve ships it), so nothing Blackmagic is linked or bundled.
CXX = c++
CXXFLAGS = -c -fvisibility=hidden -mmacosx-version-min=$(DEPLOYMENT_TARGET) $(ARCHFLAGS) -Iopenfx/include -I$(NDI_INCLUDE) -Ithird_party/braw
OBJCXXFLAGS = -c -fvisibility=hidden -mmacosx-version-min=$(DEPLOYMENT_TARGET) $(ARCHFLAGS) -Iopenfx/include -I$(NDI_INCLUDE) -x objective-c++
# -headerpad_max_install_names: `make install`/`make bench` rewrite the NDI
# dylib reference to its absolute SDK path with install_name_tool, which fails
# ("larger updated load commands do not fit") when that path is long.
LDFLAGS = -headerpad_max_install_names -bundle -fvisibility=hidden -mmacosx-version-min=$(DEPLOYMENT_TARGET) $(ARCHFLAGS) -exported_symbols_list openfx/Support/include/osxSymbols $(NDI_LIB) -framework Metal -framework MetalKit -framework Foundation -framework AppKit -framework UniformTypeIdentifiers -lz

# Source files
SOURCES = src/NDIOutputPlugin.cpp src/BRAWImmersiveReader.cpp src/TimelineClipWatcher.cpp
OBJCXX_SOURCES = src/MetalGPUAcceleration.mm src/MacFileDialog.mm
OBJECTS = $(SOURCES:.cpp=.o) $(OBJCXX_SOURCES:.mm=.o)

# Bundle structure
BUNDLE_NAME = NDIOutput.ofx.bundle
BUNDLE_EXECUTABLE = $(BUNDLE_NAME)/Contents/MacOS/NDIOutput.ofx

# Build targets
.PHONY: all clean dev install test test-metal ndi-verify

all: $(BUNDLE_EXECUTABLE)

dev: $(BUNDLE_EXECUTABLE)

# Reference NDI receiver for the signal-flow checks (BUILD.md "Receiving the
# stream"): reports a source's on-wire geometry, cadence and content, with
# --expect assertions. Needs the NDI SDK; no Resolve. Builds only.
ndi-verify:
	mkdir -p build
	$(CXX) -std=c++17 -O2 -I$(NDI_INCLUDE) tools/ndi_verify.cpp \
		-o build/ndi_verify -headerpad_max_install_names $(NDI_LIB)
	install_name_tool -change "@rpath/libndi.dylib" $(NDI_LIB) build/ndi_verify
	@echo "built build/ndi_verify — run ./build/ndi_verify --help"

# Host-independent unit tests (no Resolve or NDI SDK needed)
test:
	mkdir -p build
	$(CXX) -Isrc tests/test_render_probe.cpp -o build/test_render_probe
	./build/test_render_probe
	$(CXX) -Isrc tests/test_stream_resolution.cpp -o build/test_stream_resolution
	./build/test_stream_resolution
	$(CXX) -Isrc tests/test_stereo_pair.cpp -o build/test_stereo_pair
	./build/test_stereo_pair
	$(CXX) -Isrc tests/test_platform_paths.cpp -o build/test_platform_paths
	./build/test_platform_paths
	$(CXX) -Isrc tests/test_ndi_loader.cpp -o build/test_ndi_loader
	./build/test_ndi_loader
	$(CXX) -Isrc tests/test_stmap.cpp -o build/test_stmap -lz
	./build/test_stmap
	$(CXX) -Isrc tests/test_brawmap.cpp -o build/test_brawmap -lz
	./build/test_brawmap
	$(CXX) -Isrc tests/test_mac_timeline_watch.cpp -o build/test_mac_timeline_watch
	./build/test_mac_timeline_watch

# GPU kernel correctness tests (needs a Metal device, but no Resolve or NDI SDK)
test-metal: src/MetalGPUAcceleration.o
	mkdir -p build
	$(CXX) -Isrc tests/test_metal_downscale.mm src/MetalGPUAcceleration.o \
		-o build/test_metal_downscale -framework Metal -framework Foundation -lz
	./build/test_metal_downscale

# Pipeline timing harness at production 8K dims (needs Metal + NDI SDK, no
# Resolve). Reproduces the plugin's per-pair send pattern; see the file header.
bench: src/MetalGPUAcceleration.o
	mkdir -p build
	$(CXX) -Isrc -I$(NDI_INCLUDE) tests/bench_pipeline.mm src/MetalGPUAcceleration.o \
		-o build/bench_pipeline -headerpad_max_install_names -framework Metal -framework Foundation $(NDI_LIB)
	install_name_tool -change "@rpath/libndi.dylib" $(NDI_LIB) build/bench_pipeline
	./build/bench_pipeline

$(BUNDLE_EXECUTABLE): $(OBJECTS) | bundle_structure
	@echo "Building NDI Output Plugin v$(VERSION)"
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Built NDI Output Plugin v$(VERSION) successfully!"

# Object file compilation
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

%.o: %.mm
	$(CXX) $(OBJCXXFLAGS) $< -o $@

# Bundle structure
bundle_structure:
	mkdir -p $(BUNDLE_NAME)/Contents/MacOS
	mkdir -p $(BUNDLE_NAME)/Contents/Resources
	cp BaldavengerOFX.NDIOutput.png $(BUNDLE_NAME)/Contents/Resources/
	cp Info.plist $(BUNDLE_NAME)/Contents/
	cp src/ndi_timeline_watch.py $(BUNDLE_NAME)/Contents/Resources/
	cp src/ndi_timeline_watch.lua $(BUNDLE_NAME)/Contents/Resources/

# Installation
install: $(BUNDLE_EXECUTABLE)
	sudo rm -rf "/Library/OFX/Plugins/$(BUNDLE_NAME)"
	sudo cp -R $(BUNDLE_NAME) "/Library/OFX/Plugins/"
	sudo install_name_tool -change "@rpath/libndi.dylib" $(NDI_LIB) "/Library/OFX/Plugins/$(BUNDLE_EXECUTABLE)"

# Clean
clean:
	rm -rf $(BUNDLE_NAME)
	rm -f *.o src/*.o
	rm -rf build

# Version increment (for development)
bump-patch:
	@echo "$(shell echo $(VERSION) | awk -F. '{print $$1"."$$2"."$$3+1}')" > $(VERSION_FILE)
	@echo "Version bumped to $(shell cat $(VERSION_FILE))"

bump-minor:
	@echo "$(shell echo $(VERSION) | awk -F. '{print $$1"."$$2+1".0"}')" > $(VERSION_FILE)
	@echo "Version bumped to $(shell cat $(VERSION_FILE))"

bump-major:
	@echo "$(shell echo $(VERSION) | awk -F. '{print $$1+1".0.0"}')" > $(VERSION_FILE)
	@echo "Version bumped to $(shell cat $(VERSION_FILE))" 