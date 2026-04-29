.PHONY: help configure build test install clean venv-setup vcpkg-bootstrap

VENV_DIR ?= $(CURDIR)/venv
PRESET ?= linux-clang-release
CMAKE_BUILD_DIR := build/$(PRESET)

help:
	@echo "Falcon Qarray device Build System"
	@echo "========================"
	@echo ""
	@echo "Available presets:"
	@cmake --list-presets=all
	@echo ""
	@echo "Usage:"
	@echo "  make configure PRESET=<preset>  - Configure build (default: $(PRESET))"
	@echo "  make build PRESET=<preset>      - Build (default: $(PRESET))"
	@echo "  make test PRESET=<preset>       - Run tests (default: $(PRESET))"
	@echo "  make install PRESET=<preset>    - Install to /opt/falcon"
	@echo "  make clean                      - Clean all build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make build                                      # Build with clang (default)"
	@echo "  make build PRESET=linux-gcc-release             # Build with gcc"
	@echo "  make test PRESET=linux-clang-debug              # Run debug tests"
	@echo "  make install PRESET=linux-gcc-release           # Install gcc build"

venv-setup:
	echo "Creating $(VENV_DIR) with Python 3.12 via uv..."; \
	uv venv --python 3.12 --clear $(VENV_DIR); \
	echo "Installing Python dependencies into $(VENV_DIR)..."; \
	uv pip install --python $(VENV_DIR)/bin/python -r requirements.txt; \
	echo "✓ Python dependencies installed"; \

vcpkg-bootstrap: venv-setup
	@echo "Bootstrapping vcpkg..."
	cmake -P cmake/bootstrap/bootstrap-vcpkg.cmake

configure: vcpkg-bootstrap
	@echo "Configuring $(PRESET)..."
	cmake --preset $(PRESET)

build: configure
	@echo "Building $(PRESET)..."
	cmake --build --preset $(PRESET)

test: build
	@echo "Running tests for $(PRESET)..."
	PLPLOT_LIB=$(CURDIR)/vcpkg_installed/x64-linux-dynamic/share/plplot/data \
	ctest --preset $(PRESET) --output-on-failure

install: build
	@echo "Installing $(PRESET) to /opt/falcon..."
	cmake --install $(CMAKE_BUILD_DIR) --prefix /opt/falcon

clean:
	@echo "Cleaning all build artifacts..."
	rm -rf build vcpkg_installed
	@echo "✓ Clean complete"
