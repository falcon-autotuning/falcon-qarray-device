# Falcon qarrayDevice Makefile
# Wraps cmake + vcpkg, following the same pattern as database/ and comms/

.PHONY: all configure build-debug build-release test test-debug clean install uninstall \
        help check-vcpkg check-python install-python-deps clangd-helpers rebuild-debug rebuild-release

PLATFORM := linux
CMAKE_GENERATOR := Ninja
VCPKG_TRIPLET ?= x64-linux-dynamic
NPROC := $(shell nproc 2>/dev/null || echo 4)
export CC=clang
export CXX=clang++

ENV_FILE := .nuget-credentials
ifeq ($(wildcard $(ENV_FILE)),)
  $(info [Makefile] $(ENV_FILE) not found, skipping environment sourcing)
else
  include $(ENV_FILE)
  export $(shell sed 's/=.*//' $(ENV_FILE) | xargs)
  $(info [Makefile] Loaded environment from $(ENV_FILE))
endif
# ── Paths ─────────────────────────────────────────────────────────────────────
VCPKG_ROOT ?= $(CURDIR)/vcpkg
VCPKG_TOOLCHAIN ?= $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
VCPKG_INSTALLED_DIR ?= $(CURDIR)/vcpkg_installed
FEED_URL ?= 
NUGET_API_KEY ?=
FEED_NAME ?= 
USERNAME ?=
VCPKG_BINARY_SOURCES ?= ""
ifeq ($(strip $(FEED_URL)),)
  CMAKE_VCPKG_BINARY_SOURCES :=
else
	VCPKG_BINARY_SOURCES := "clear;nuget,$(FEED_URL),readwrite"
  CMAKE_VCPKG_BINARY_SOURCES := -DVCPKG_BINARY_SOURCES=$(VCPKG_BINARY_SOURCES)
endif
LINKER_FLAGS ?=
ifeq ($(PLATFORM),linux)
	LINKER_FLAGS := -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld"
endif

BUILD_DIR_DEBUG   := build/debug
BUILD_DIR_RELEASE := build/release

INSTALL_PREFIX    ?= /opt/falcon
INSTALL_LIBDIR    := $(INSTALL_PREFIX)/lib
INSTALL_INCLUDEDIR := $(INSTALL_PREFIX)/include

# ── Python: use the uv venv, not the system or vcpkg Python ──────────────────
# We detect the Python executable inside the .venv created by uv.
# This Python has the full stdlib AND qarray/numpy installed.
# The cmake build must link against THIS libpython — not vcpkg's stub.
#
# Override by setting PYTHON_EXECUTABLE explicitly:
#   make configure PYTHON_EXECUTABLE=/path/to/my/python
VENV_DIR     := $(CURDIR)/.venv
PYTHON_EXECUTABLE ?= $(VENV_DIR)/bin/python

# ── Default target ────────────────────────────────────────────────────────────
all: build-release

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo "Falcon qarrayDevice Build System"
	@echo "=================================="
	@echo ""
	@echo "First-time setup (run in order):"
	@echo "  make install-python-deps  - Create .venv with Python 3.12 + qarray"
	@echo "  make build-release        - Build the C++ library (links against venv Python)"
	@echo "  make install              - Install to INSTALL_PREFIX"
	@echo ""
	@echo "Build targets:"
	@echo "  make build-debug          - Build debug version"
	@echo "  make build-release        - Build release version"
	@echo "  make configure            - Configure both debug and release"
	@echo "  make clean                - Clean build artifacts"
	@echo ""
	@echo "Test targets:"
	@echo "  make test                 - Run release tests"
	@echo "  make test-debug           - Run debug tests"
	@echo ""
	@echo "Environment variables:"
	@echo "  VCPKG_ROOT          - Path to vcpkg root         (default: ../.vcpkg)"
	@echo "  VCPKG_TRIPLET       - vcpkg triplet              (default: x64-linux-dynamic)"
	@echo "  INSTALL_PREFIX      - Install prefix             (default: /opt/falcon)"
	@echo "  PYTHON_EXECUTABLE   - Full path to Python 3.12   (default: .venv/bin/python)"
	@echo "  FALCON_QARRAY_PYTHON_PATH       - Runtime: path to quantum_dot_device.py"
	@echo "  FALCON_QARRAY_VENV_SITE_PACKAGES - Runtime: override venv site-packages path"
	@echo ""
	@echo "Current configuration:"
	@echo "  Platform:          $(PLATFORM)"
	@echo "  Generator:         $(CMAKE_GENERATOR)"
	@echo "  Triplet:           $(VCPKG_TRIPLET)"
	@echo "  Python executable: $(PYTHON_EXECUTABLE)"

# ── vcpkg check ───────────────────────────────────────────────────────────────
.PHONY: vcpkg-bootstrap
vcpkg-bootstrap:
	@if [ ! -d "$(VCPKG_ROOT)" ]; then \
		echo "Cloning vcpkg..."; \
		git clone https://github.com/microsoft/vcpkg.git $(VCPKG_ROOT); \
	fi
	@if [ ! -f "$(VCPKG_ROOT)/vcpkg" ]; then \
		echo "Bootstrapping vcpkg..."; \
		cd $(VCPKG_ROOT) && ./bootstrap-vcpkg.sh; \
	fi

setup-nuget-auth:
	@if [ -z "$$NUGET_API_KEY" ]; then \
		echo "No NUGET_API_KEY found, skipping NuGet setup (local-only build, no binary cache)."; \
		exit 0; \
	fi
	@echo "Setting up NuGet authentication for vcpkg binary caching..."
	@if ! command -v mono >/dev/null 2>&1; then \
		echo "Error: mono is not installed. Please install mono (e.g., 'sudo pacman -S mono' on Arch, 'sudo apt install mono-complete' on Ubuntu)."; \
		exit 1; \
	fi
	@NUGET_EXE=$$(vcpkg fetch nuget | tail -n1); \
	mono "$$NUGET_EXE" sources remove -Name "$(FEED_NAME)" || true; \
	mono "$$NUGET_EXE" sources add -Name "$(FEED_NAME)" -Source "$(FEED_URL)" -Username "$(USERNAME)" -Password "$(NUGET_API_KEY)"

.PHONY: vcpkg-install-deps
vcpkg-install-deps: setup-nuget-auth 
	@echo "Installing vcpkg dependencies" 
	VCPKG_FEATURE_FLAGS=binarycaching \
		$(VCPKG_ROOT)/vcpkg install \
		--binarysource=$(VCPKG_BINARY_SOURCES) \
		--triplet="$(VCPKG_TRIPLET)"

check-vcpkg: vcpkg-bootstrap  vcpkg-install-deps
	@echo "Checking vcpkg configuration..."
	@if [ ! -d "$(VCPKG_ROOT)" ]; then \
		echo "Error: vcpkg not found at $(VCPKG_ROOT)"; \
		echo "Run 'make deps' in the parent directory first"; \
		exit 1; \
	fi
	@if [ ! -f "$(VCPKG_TOOLCHAIN)" ]; then \
		echo "Error: vcpkg toolchain not found at $(VCPKG_TOOLCHAIN)"; \
		exit 1; \
	fi
	@echo "✓ vcpkg configuration OK"

# ── Python venv check ─────────────────────────────────────────────────────────
check-python:
	@if [ ! -f "$(PYTHON_EXECUTABLE)" ]; then \
		echo "Error: Python not found at $(PYTHON_EXECUTABLE)"; \
		echo "Run 'make install-python-deps' first to create the .venv"; \
		exit 1; \
	fi
	@$(PYTHON_EXECUTABLE) -c "import qarray" 2>/dev/null || { \
		echo "Error: qarray not importable from $(PYTHON_EXECUTABLE)"; \
		echo "Run 'make install-python-deps' first"; \
		exit 1; \
	}
	@echo "✓ Python OK: $(PYTHON_EXECUTABLE)"
	@echo "✓ qarray importable"

# ── Python dependencies ───────────────────────────────────────────────────────
# Creates a local .venv using uv with Python 3.12 and installs qarray into it.
# Python 3.12 is required because qarray-rust-core has no wheels for 3.13+.
install-python-deps:
	@echo "Creating .venv with Python 3.12 via uv..."
	uv venv --python 3.12 $(VENV_DIR)
	@echo "Installing Python dependencies into $(VENV_DIR)..."
	uv pip install --python $(VENV_DIR)/bin/python -r requirements.txt
	@echo ""
	@echo "✓ Python dependencies installed"
	@echo "  Venv:   $(VENV_DIR)"
	@echo "  Python: $(PYTHON_EXECUTABLE)"
	@echo ""
	@echo "Next step: run 'make build-release' to build the C++ library."
	@echo "The build will automatically link against this venv's Python."

# ── CMake configure ───────────────────────────────────────────────────────────
# We pass Python3_EXECUTABLE explicitly so cmake uses the venv Python
# (which has the stdlib and qarray) rather than vcpkg's stripped Python.
configure-debug: check-vcpkg check-python
	@echo "Configuring debug build..."
	@echo "  Using Python: $(PYTHON_EXECUTABLE)"
	@mkdir -p $(BUILD_DIR_DEBUG)
	cd $(BUILD_DIR_DEBUG) && cmake ../.. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) \
		-DVCPKG_INSTALLED_DIR=$(VCPKG_INSTALLED_DIR) \
		-DVCPKG_TARGET_TRIPLET=$(VCPKG_TRIPLET) \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX) \
		-DPython3_EXECUTABLE=$(PYTHON_EXECUTABLE) \
		-DPython3_ROOT_DIR=$(VENV_DIR) \
		-DPython3_FIND_STRATEGY=LOCATION \
		-DBUILD_TESTS=ON \
		-DUSE_CCACHE=ON \
		-DENABLE_PCH=ON \
		-DCMAKE_C_COMPILER=clang \
		-DCMAKE_CXX_COMPILER=clang++ \
		$(CMAKE_VCPKG_BINARY_SOURCES) \
		-G $(CMAKE_GENERATOR)
	@echo "✓ Debug build configured"

configure-release: check-vcpkg check-python
	@echo "Configuring release build..."
	@echo "  Using Python: $(PYTHON_EXECUTABLE)"
	@mkdir -p $(BUILD_DIR_RELEASE)
	cd $(BUILD_DIR_RELEASE) && cmake ../.. \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) \
		-DVCPKG_INSTALLED_DIR=$(VCPKG_INSTALLED_DIR) \
		-DVCPKG_TARGET_TRIPLET=$(VCPKG_TRIPLET) \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX) \
		-DPython3_EXECUTABLE=$(PYTHON_EXECUTABLE) \
		-DPython3_ROOT_DIR=$(VENV_DIR) \
		-DPython3_FIND_STRATEGY=LOCATION \
		-DBUILD_TESTS=ON \
		-DUSE_CCACHE=ON \
		-DENABLE_PCH=ON \
		-DCMAKE_C_COMPILER=clang \
		-DCMAKE_CXX_COMPILER=clang++ \
		$(CMAKE_VCPKG_BINARY_SOURCES) \
		-G $(CMAKE_GENERATOR)
	@echo "✓ Release build configured"

configure: configure-debug configure-release

# ── Build ─────────────────────────────────────────────────────────────────────
build-debug: configure-debug
	@echo "Building debug..."
	ninja -C $(BUILD_DIR_DEBUG) -j$(NPROC)
	@echo "✓ Debug build complete"
	@$(MAKE) clangd-helpers

build-release: configure-release
	@echo "Building release..."
	ninja -C $(BUILD_DIR_RELEASE) -j$(NPROC)
	@echo "✓ Release build complete"

# ── Test ──────────────────────────────────────────────────────────────────────
test: build-release
	@echo "Running release tests..."
	cd $(BUILD_DIR_RELEASE) && ctest --output-on-failure -C Release -V
	@echo "✓ Tests passed"

test-debug: build-debug
	@echo "Running debug tests..."
	cd $(BUILD_DIR_DEBUG) && ctest --output-on-failure -C Debug
	@echo "✓ Tests passed"

# ── Install ───────────────────────────────────────────────────────────────────
install: build-release
	@echo "Installing falcon-qarrayDevice to $(INSTALL_PREFIX)..."
	$(SUDO) cmake --install $(BUILD_DIR_RELEASE) --prefix $(INSTALL_PREFIX)
	@echo "Copying vcpkg headers and libraries..."
	$(SUDO) cp -r $(VCPKG_INSTALLED_DIR)/$(VCPKG_TRIPLET)/include/* $(INSTALL_INCLUDEDIR)/
	$(SUDO) cp -r $(VCPKG_INSTALLED_DIR)/$(VCPKG_TRIPLET)/lib/*     $(INSTALL_LIBDIR)/
	$(SUDO) cp -r $(VCPKG_INSTALLED_DIR)/$(VCPKG_TRIPLET)/bin/*     $(INSTALL_LIBDIR)/ || true
ifeq ($(UNAME_S),Linux)
	$(SUDO) ldconfig
endif
	@echo "✓ Installation complete"

# ── Uninstall ─────────────────────────────────────────────────────────────────
uninstall:
	@echo "Uninstalling falcon-qarrayDevice from $(INSTALL_PREFIX)..."
	$(SUDO) rm -rf \
		$(INSTALL_LIBDIR)/libfalcon-qarrayDevice* \
		$(INSTALL_INCLUDEDIR)/qarrayDevice \
		$(INSTALL_PREFIX)/share/qarrayDevice \
		$(INSTALL_PREFIX)/lib/cmake/falcon-qarrayDevice || true
	@echo "✓ Uninstall complete"

# ── Clean ──────────────────────────────────────────────────────────���──────────
clean: uninstall
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR_DEBUG) $(BUILD_DIR_RELEASE) build/ compile_commands.json
	@echo "✓ Clean complete"

# ── Quick rebuild (no reconfigure) ───────────────────────────────────────────
rebuild-debug:
	cmake --build $(BUILD_DIR_DEBUG)   --config Debug   -j$(NPROC)

rebuild-release:
	cmake --build $(BUILD_DIR_RELEASE) --config Release -j$(NPROC)

# ── clangd helpers ────────────────────────────────────────────────────────────
.PHONY: clangd-helpers
clangd-helpers:
	@if [ -f $(BUILD_DIR_DEBUG)/compile_commands.json ]; then \
		ln -sf $(BUILD_DIR_DEBUG)/compile_commands.json compile_commands.json; \
		echo "✓ clangd compile_commands.json symlinked to qarrayDevice/ root"; \
	else \
		echo "No compile_commands.json found in debug build directory."; \
	fi
