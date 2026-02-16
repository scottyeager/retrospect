.PHONY: build run clean

BUILD_DIR := build
BUILD_TYPE := Debug

build:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build $(BUILD_DIR) -j$$(nproc)

run: build
	@$(BUILD_DIR)/retrospect_artefacts/$(BUILD_TYPE)/retrospect

clean:
	@rm -rf $(BUILD_DIR)
