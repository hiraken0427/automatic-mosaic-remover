CXX ?= g++
PKG_CONFIG ?= pkg-config
BUILD_DIR ?= bin
IMGUI_DIR ?= vendor/imgui

CPPFLAGS += -Isrc
CXXFLAGS ?= -O2 -g
PROJECT_FLAGS := -std=c++20 -Wall -Wextra -Wpedantic
WINDOWS_BUILD := $(filter Windows_NT,$(OS))$(findstring mingw,$(MAKE_HOST))

ifneq ($(WINDOWS_BUILD),)
EXE := .exe
DEP_PREFIX ?= vendor/windows-x64
WINDOWS_DIST := dist/windows-x64
GUI_LINK_FLAGS := -mwindows
ifeq ($(DEP_PREFIX),vendor/windows-x64)
RUNTIME_DIR := $(WINDOWS_DIST)
RUNTIME_DLLS := $(patsubst $(RUNTIME_DIR)/%,$(BUILD_DIR)/%,$(wildcard $(RUNTIME_DIR)/*.dll))
# Link compiler runtimes statically and keep SDL2 and FFmpeg as DLL imports.
RUNTIME_LINK_FLAGS := -static
SDL_LIBS ?= -L"$(DEP_PREFIX)/lib" -l:libSDL2.dll.a
FFMPEG_LIBS ?= -L"$(DEP_PREFIX)/lib" -l:libavformat.dll.a -l:libavcodec.dll.a -l:libavutil.dll.a -l:libswscale.dll.a
endif
else
THREAD_FLAGS := -pthread
endif

ifeq ($(DEP_PREFIX),)
SDL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS ?= $(shell $(PKG_CONFIG) --libs sdl2)
FFMPEG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil libswscale)
FFMPEG_LIBS ?= $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil libswscale)
else
SDL_CFLAGS ?= -I"$(DEP_PREFIX)/include/SDL2"
SDL_LIBS ?= -L"$(DEP_PREFIX)/lib" -lSDL2
FFMPEG_CFLAGS ?= -I"$(DEP_PREFIX)/include"
FFMPEG_LIBS ?= -L"$(DEP_PREFIX)/lib" -lavformat -lavcodec -lavutil -lswscale
endif

DOMAIN := $(BUILD_DIR)/domain/restoration.o $(BUILD_DIR)/domain/detection.o $(BUILD_DIR)/domain/tracking.o
APPLICATION := $(BUILD_DIR)/application/video.o
MEDIA := $(BUILD_DIR)/infrastructure/ffmpeg_media.o
GUI := $(BUILD_DIR)/presentation/main.o
IMGUI_NAMES := imgui imgui_draw imgui_tables imgui_widgets backends/imgui_impl_sdl2 backends/imgui_impl_sdlrenderer2
IMGUI_OBJECTS := $(addprefix $(BUILD_DIR)/imgui/,$(addsuffix .o,$(IMGUI_NAMES)))
APP := $(BUILD_DIR)/mosaic-remover$(EXE)
DOMAIN_TEST := $(BUILD_DIR)/domain-test$(EXE)
MEDIA_TEST := $(BUILD_DIR)/media-test$(EXE)

.PHONY: all run test test-media clean check-imgui
all: $(APP)

run: all
	"./$(APP)"

test: $(DOMAIN_TEST)
	"./$(DOMAIN_TEST)"

test-media: $(MEDIA_TEST)
	python3 tests/media_test.py "$(MEDIA_TEST)"

check-imgui:
	@test -f "$(IMGUI_DIR)/backends/imgui_impl_sdlrenderer2.cpp" || { echo "Set IMGUI_DIR to a Dear ImGui 1.90.1 or compatible source tree."; exit 1; }

$(APP): $(DOMAIN) $(APPLICATION) $(MEDIA) $(GUI) $(IMGUI_OBJECTS) | $(RUNTIME_DLLS)
	$(CXX) $(LDFLAGS) $(THREAD_FLAGS) $(GUI_LINK_FLAGS) $(RUNTIME_LINK_FLAGS) $^ $(SDL_LIBS) $(FFMPEG_LIBS) $(LDLIBS) -o "$@"

$(DOMAIN_TEST): tests/domain_test.cpp $(DOMAIN)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PROJECT_FLAGS) $(THREAD_FLAGS) $(LDFLAGS) $(RUNTIME_LINK_FLAGS) $^ -o "$@"

$(MEDIA_TEST): tests/media_test.cpp $(DOMAIN) $(APPLICATION) $(MEDIA) | $(RUNTIME_DLLS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PROJECT_FLAGS) $(THREAD_FLAGS) $(LDFLAGS) $(RUNTIME_LINK_FLAGS) $^ $(FFMPEG_LIBS) -o "$@"

ifeq ($(DEP_PREFIX),vendor/windows-x64)
ifneq ($(WINDOWS_BUILD),)
.PHONY: package-windows
package-windows: $(APP)
	cp "$<" "$(WINDOWS_DIST)/mosaic-remover.exe"

ifneq ($(RUNTIME_DLLS),)
$(RUNTIME_DLLS): $(BUILD_DIR)/%.dll: $(RUNTIME_DIR)/%.dll
	@mkdir -p "$(@D)"
	cp "$<" "$@"
endif
endif
endif

$(MEDIA): CPPFLAGS += $(FFMPEG_CFLAGS)
$(GUI): CPPFLAGS += $(SDL_CFLAGS) -I"$(IMGUI_DIR)"
$(GUI): | check-imgui

$(BUILD_DIR)/imgui/%.o: $(IMGUI_DIR)/%.cpp | check-imgui
	@mkdir -p "$(@D)"
	$(CXX) $(SDL_CFLAGS) $(CXXFLAGS) -std=c++20 -I"$(IMGUI_DIR)" -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PROJECT_FLAGS) $(THREAD_FLAGS) -MMD -MP -c "$<" -o "$@"

clean:
	rm -rf -- "$(BUILD_DIR)"

-include $(DOMAIN:.o=.d) $(APPLICATION:.o=.d) $(MEDIA:.o=.d) $(GUI:.o=.d) $(IMGUI_OBJECTS:.o=.d)
