.PHONY: build run clean cross-arm64 cross-arm64-extract

BUILD_DIR := build
BUILD_TYPE := Debug
CONTAINER_RT := $(shell command -v podman 2>/dev/null || echo docker)
CROSS_IMAGE := retrospect-cross-arm64

build:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build $(BUILD_DIR) -j$$(nproc)

run: build
	@$(BUILD_DIR)/retrospect_artefacts/$(BUILD_TYPE)/retrospect

clean:
	@rm -rf $(BUILD_DIR)

cross-arm64:
	$(CONTAINER_RT) build -f Dockerfile.cross-arm64 -t $(CROSS_IMAGE) .

cross-arm64-extract: cross-arm64
	@mkdir -p $(BUILD_DIR)/arm64
	$(CONTAINER_RT) create --name retrospect-tmp $(CROSS_IMAGE) true
	$(CONTAINER_RT) cp retrospect-tmp:/src/build/retrospect_artefacts/Release/retrospect $(BUILD_DIR)/arm64/retrospect
	$(CONTAINER_RT) rm retrospect-tmp
	@echo "ARM64 binary: $(BUILD_DIR)/arm64/retrospect"
