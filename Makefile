CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
CXX ?= c++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

BUILD_DIR := build
SRC_DIR := src
UNAME_S := $(shell uname -s 2>/dev/null)

COMMON_OBJS := $(BUILD_DIR)/cpu6809.o $(BUILD_DIR)/util.o
APEXDIS_CORE_OBJS := $(BUILD_DIR)/apexdis.o $(BUILD_DIR)/apexdmd.o $(BUILD_DIR)/apex_project.o $(BUILD_DIR)/apex_render.o $(BUILD_DIR)/apex_analysis.o $(BUILD_DIR)/apex_config.o $(COMMON_OBJS)
APEXDMD_OBJS := $(COMMON_OBJS) $(BUILD_DIR)/apex_analysis.o $(BUILD_DIR)/apex_config.o
APEXINI_OBJS := $(BUILD_DIR)/apexini.o $(BUILD_DIR)/apex_config.o $(COMMON_OBJS)

APEXIMGUI_SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
APEXIMGUI_SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)

ifeq ($(OS),Windows_NT)
APEXIMGUI_GL_LIBS := -lopengl32
else ifeq ($(UNAME_S),Darwin)
APEXIMGUI_GL_LIBS := -framework OpenGL
else
APEXIMGUI_GL_LIBS := $(shell $(PKG_CONFIG) --libs gl 2>/dev/null)
endif

APEXIMGUI_CPPFLAGS := $(CPPFLAGS) -I$(SRC_DIR) -Ithird_party/imgui -Ithird_party/imgui/backends $(APEXIMGUI_SDL_CFLAGS)
APEXIMGUI_LDLIBS := $(APEXIMGUI_SDL_LIBS) $(APEXIMGUI_GL_LIBS)
APEXIMGUI_OBJS := $(BUILD_DIR)/apeximgui.o \
	$(BUILD_DIR)/apeximgui_data.o \
	$(BUILD_DIR)/apeximgui_analysis.o \
	$(BUILD_DIR)/apeximgui_views.o \
	$(BUILD_DIR)/imgui.o \
	$(BUILD_DIR)/imgui_draw.o \
	$(BUILD_DIR)/imgui_tables.o \
	$(BUILD_DIR)/imgui_widgets.o \
	$(BUILD_DIR)/imgui_demo.o \
	$(BUILD_DIR)/imgui_impl_sdl2.o \
	$(BUILD_DIR)/imgui_impl_opengl3.o \
	$(APEXDIS_CORE_OBJS)

.PHONY: all clean test apexcli

all: $(BUILD_DIR)/apexdis $(BUILD_DIR)/apexasm $(BUILD_DIR)/apextab $(BUILD_DIR)/apeximgui $(BUILD_DIR)/apexdmd $(BUILD_DIR)/apexini $(BUILD_DIR)/project_api_test $(BUILD_DIR)/apexdmd_test

apexcli: $(BUILD_DIR)/apexdis $(BUILD_DIR)/apexasm $(BUILD_DIR)/apextab $(BUILD_DIR)/apexdmd $(BUILD_DIR)/apexini

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/apex.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/apeximgui.o $(BUILD_DIR)/apeximgui_data.o $(BUILD_DIR)/apeximgui_analysis.o $(BUILD_DIR)/apeximgui_views.o: $(SRC_DIR)/apeximgui_core.h

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui.o: third_party/imgui/imgui.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui_draw.o: third_party/imgui/imgui_draw.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui_tables.o: third_party/imgui/imgui_tables.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui_widgets.o: third_party/imgui/imgui_widgets.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui_demo.o: third_party/imgui/imgui_demo.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/imgui_impl_%.o: third_party/imgui/backends/imgui_impl_%.cpp | $(BUILD_DIR)
	$(CXX) $(APEXIMGUI_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/project_api_test.o: tests/project_api_test.c $(SRC_DIR)/apex_project.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

$(BUILD_DIR)/apexdmd_test.o: tests/apexdmd_test.c $(SRC_DIR)/apexdmd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

$(BUILD_DIR)/apexdis: $(BUILD_DIR)/apexdis_main.o $(APEXDIS_CORE_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/apeximgui: $(APEXIMGUI_OBJS)
	$(CXX) $(LDFLAGS) $^ $(APEXIMGUI_LDLIBS) -o $@

$(BUILD_DIR)/apexdmd: $(BUILD_DIR)/apexdmd_main.o $(BUILD_DIR)/apexdmd.o $(APEXDMD_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/project_api_test: $(BUILD_DIR)/project_api_test.o $(APEXDIS_CORE_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/apexdmd_test: $(BUILD_DIR)/apexdmd_test.o $(BUILD_DIR)/apexdmd.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/apexini: $(APEXINI_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/apexasm: $(BUILD_DIR)/apexasm.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

test: all
	./tests/roundtrip.sh

clean:
	rm -rf $(BUILD_DIR) out
