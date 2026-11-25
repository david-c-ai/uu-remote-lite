CXX := /usr/bin/clang++
CXXFLAGS := -std=c++20 -O3 -flto -Wall -Wextra -Wno-unused-parameter -Icore/include -Iserver-lite/include

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    APPLE_LDFLAGS := -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework CoreVideo -framework CoreMedia -framework ImageIO -framework VideoToolbox -framework AudioToolbox -framework ApplicationServices -framework ScreenCaptureKit -lcompression
    OBJCXX_FLAGS := -ObjC++ -fobjc-arc
    SHARED_EXT := dylib
    SHARED_FLAGS := -dynamiclib -install_name @rpath/libdircore.dylib
else
    APPLE_LDFLAGS := -lpthread
    OBJCXX_FLAGS :=
    SHARED_EXT := so
    SHARED_FLAGS := -shared -fPIC
endif

BUILD_DIR := build

CORE_SRCS := \
    server-lite/src/server_runtime.cpp \
    core/src/crypto_e2ee.cpp \
    core/src/fec_qos.cpp \
    core/src/capture_hal.cpp \
    core/src/codec_hal.cpp \
    core/src/keymap_hal.cpp \
    core/src/audio_hal.cpp \
    core/src/hid_clipboard_file.cpp \
    core/src/session_engine.cpp \
    core/src/dircore_ffi.cpp

.PHONY: all clean test

all: $(BUILD_DIR)/dir-server $(BUILD_DIR)/libdircore.$(SHARED_EXT) $(BUILD_DIR)/dir-e2e-bench

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/dir-server: server-lite/src/server_runtime.cpp server-lite/src/main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@
	strip -x $@

$(BUILD_DIR)/libdircore.$(SHARED_EXT): $(CORE_SRCS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJCXX_FLAGS) -fPIC $(SHARED_FLAGS) $^ $(APPLE_LDFLAGS) -o $@
	strip -x $@

$(BUILD_DIR)/dir-e2e-bench: core/tools/uu_e2e_bench.cpp $(BUILD_DIR)/libdircore.$(SHARED_EXT) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wl,-rpath,@executable_path core/tools/uu_e2e_bench.cpp -L$(BUILD_DIR) -ldircore $(APPLE_LDFLAGS) -o $@

test: all
	./$(BUILD_DIR)/dir-e2e-bench

clean:
	rm -rf $(BUILD_DIR)
