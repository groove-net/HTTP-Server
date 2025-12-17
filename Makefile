# -----------------------------
# Compiler & flags
# -----------------------------
CC := clang
AR := ar
ARFLAGS := rcs
CFLAGS := -Wall -Wextra -Werror -pedantic -std=c99 -g -fsanitize=address -MMD -MP -Iinclude -Isrc -D_XOPEN_SOURCE=700

# -----------------------------
# Directories
# -----------------------------
SRC_DIR := src
BUILD_DIR := build
BIN_DIR := bin
LIB_DIR := lib
TARGET := $(BIN_DIR)/program

# -----------------------------
# Modules
# -----------------------------
# Library modules: directories starting with lib*
LIB_SRC_DIRS := $(shell find $(SRC_DIR) -mindepth 1 -maxdepth 1 -type d -name 'lib*')
LIB_NAMES := $(notdir $(LIB_SRC_DIRS))

# App modules: directories not starting with lib*
APP_SRC_DIRS := $(shell find $(SRC_DIR) -mindepth 1 -maxdepth 1 -type d ! -name 'lib*')
APP_NAMES := $(notdir $(APP_SRC_DIRS))

# -----------------------------
# Unity source files (one per module)
# -----------------------------
LIB_ENTRY_SRCS := $(foreach lib,$(LIB_NAMES),$(SRC_DIR)/$(lib)/$(lib).c)
APP_ENTRY_SRCS := $(foreach app,$(APP_NAMES),$(SRC_DIR)/$(app)/$(app).c)

# Check that entry files exist
$(foreach f,$(LIB_ENTRY_SRCS) $(APP_ENTRY_SRCS),\
  $(if $(wildcard $(f)),,$(error Missing required module entry file: $(f)))\
)

# -----------------------------
# Object files
# -----------------------------
LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_ENTRY_SRCS))
APP_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(APP_ENTRY_SRCS))

# -----------------------------
# Library files
# -----------------------------
STATIC_LIB_FILES := $(wildcard $(LIB_DIR)/*.a)

# -----------------------------
# Dependencies
# -----------------------------
DEPS := $(LIB_OBJS:.o=.d) $(APP_OBJS:.o=.d)

# -----------------------------
# Compile rule for unity files
# -----------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# -----------------------------
# Build static library for each lib module
# -----------------------------
define MAKE_STATIC_LIB
$(LIB_DIR)/$1.a: $(BUILD_DIR)/$1/$1.o
	@mkdir -p $(LIB_DIR)
	$(AR) $(ARFLAGS) $$@ $$^
endef

$(foreach lib,$(LIB_NAMES),$(eval $(call MAKE_STATIC_LIB,$(lib))))

# -----------------------------
# Main program
# -----------------------------
MAIN_OBJ := $(BUILD_DIR)/main.o

# -----------------------------
# Final executable
# -----------------------------
$(TARGET): $(MAIN_OBJ) $(APP_OBJS) $(STATIC_LIB_FILES)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

all: $(TARGET)

run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)

.PHONY: all clean run
