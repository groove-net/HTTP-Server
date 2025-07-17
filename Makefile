CC = gcc
AR = ar
ARFLAGS = rcs
CFLAGS = -Wall -Wextra -Werror -pedantic -MMD -MP -Iinclude -Isrc

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
LIB_DIR = lib
TARGET = $(BIN_DIR)/program

# Find only src/lib* subdirectories
LIB_SRC_DIRS = $(shell find $(SRC_DIR) -mindepth 1 -maxdepth 1 -type d -name 'lib*')
LIB_NAMES = $(notdir $(LIB_SRC_DIRS))

# Static library targets
STATIC_LIB_FILES = $(addprefix $(LIB_DIR)/, $(addsuffix .a, $(LIB_NAMES)))

# Collect .c files from each lib folder
LIB_SRCS = $(foreach dir,$(LIB_SRC_DIRS),$(wildcard $(dir)/*.c))
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(LIB_SRCS))

# App sources (excluding library sources)
APP_SRCS = $(filter-out $(LIB_SRCS), $(shell find $(SRC_DIR) -type f -name '*.c' ! -path "$(SRC_DIR)/lib*" ))
APP_OBJS = $(patsubst $(SRC_DIR)/%, $(BUILD_DIR)/%, $(APP_SRCS:.c=.o))

# Dependency files
DEPS = $(LIB_OBJS:.o=.d) $(APP_OBJS:.o=.d)

# Compile rule for object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rules to build .a for each lib
define MAKE_STATIC_LIB
$(LIB_DIR)/$1.a: $(filter $(BUILD_DIR)/$1/%.o, $(LIB_OBJS))
	@mkdir -p $(LIB_DIR)
	$(AR) $(ARFLAGS) $$@ $$^
endef
$(foreach lib,$(LIB_NAMES),$(eval $(call MAKE_STATIC_LIB,$(lib))))

# Final executable
$(TARGET): $(APP_OBJS) $(STATIC_LIB_FILES)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@

all: $(TARGET)

run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR)

-include $(DEPS)

.PHONY: all clean run
