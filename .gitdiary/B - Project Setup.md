### Setting up the repository

Inside your workspace directory, create a file called `init.sh` and copy the following contents into that file. This script automates sets up a basic C project.

`init.sh`:

```bash
#!/bin/bash

set -e # Exit immediately if any command returns a non-zero (error) status.

program_name="$1"
program_name=$(echo "$program_name" | sed 's/  */ /g')
program_name_formatted=$(echo "$program_name" | tr '[:upper:]' '[:lower:]' | sed 's/ \+/-/g')
license="$2"

# Check if program_name is empty
if [ -z "$program_name" ]; then
  echo "Error: Please provide a project name."
  exit 1
fi

# Default license to MIT if not provided
if [ -z "$license" ]; then
  license="MIT"
fi

# Begin project setup
echo "💡 Setting up Project: $program_name"
mkdir "$program_name_formatted"
cd "$program_name_formatted"

echo "📁 Creating directory structure..."
mkdir -p src include lib

echo "📄 Creating starter source file..."
cat > src/main.c << EOF
/*
 ============================================================================
 Name        : $program_name
 Author      : _______________
 Version     : 1.0
 Description : A simple C application demonstrating project structure with
               src, include, Makefile-based build system, and modular design.
               Uses static libraries and includes dependency management.
 License     : $license
 ============================================================================
*/

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	puts("Running $program_name...");
  return EXIT_SUCCESS;
}
EOF

echo "📝 Creating README.md..."
cat > README.md << EOF
# $program_name

A simple C application demonstrating project structure with
src, include, Makefile-based build system, and modular design.
Uses static libraries and includes dependency management.
EOF

echo "🚫 Creating .gitignore..."
cat > .gitignore << 'EOF'
bin/
build/
lib/
EOF

echo "📜 Downloading LICENSE.md..."
gh repo license view "$license" > LICENSE.md

echo "🛠  Creating Makefile..."
cat > Makefile << 'EOF'
# -----------------------------
# Compiler & flags
# -----------------------------
CC := clang
AR := ar
ARFLAGS := rcs
CFLAGS := -Wall -Wextra -Werror -pedantic -std=c99 -g -MMD -MP -Iinclude -Isrc -D_XOPEN_SOURCE=700

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
EOF

echo "✅ Project initialized!"
```

Make it executable using the `chmod` command:

```bash
❯ ****chmod +x init.sh
```

Then we can use the `bash` command to run the script and pass the language name as an argument like so:

```bash
❯ ./init.sh httplite
```

### Installing dependencies with excalibur

Excalibur is a toy package manager for C application, I wrote in a single bash script. Inside your project directory, create a file called `excalibur.sh` and copy the following contents into that file. This script setting up dependencies in your project.

`excalibur.sh`:

```bash
#!/usr/bin/env bash
set -e

ACTION="$1"
TARGET="$2"

PROJECT_LIB_DIR="./lib"
PROJECT_INCLUDE_DIR="./include"
HIDDEN_WORKDIR=".excalibur_tmp"

# Colors
RED="\033[31m"
GREEN="\033[32m"
YELLOW="\033[33m"
BLUE="\033[34m"
BOLD="\033[1m"
RESET="\033[0m"

# Spinner status file
SPINNER_STATUS_FILE=".spinner_status"
SPINNER_PID=""

########################################
# Spinner control
########################################
spinner_start() {
    echo "Starting..." > "$SPINNER_STATUS_FILE"

    (
        local frames='|/-\'
        local i=0
        tput civis  # hide cursor
        while true; do
            local status
            status=$(cat "$SPINNER_STATUS_FILE" 2>/dev/null)
            printf "\r[%c] %s" "${frames:i++%4:1}" "$status"
            sleep 0.1
        done
    ) &
    SPINNER_PID=$!
}

spinner_set_status() {
    echo "$1" > "$SPINNER_STATUS_FILE"
}

spinner_stop() {
    if [[ -n "$SPINNER_PID" ]]; then
        kill "$SPINNER_PID" 2>/dev/null || true
        wait "$SPINNER_PID" 2>/dev/null || true
    fi
    rm -f "$SPINNER_STATUS_FILE"
    tput cnorm   # restore cursor
    printf "\r\033[K"  # clear line
}
########################################

# Check arguments
if [[ -z "$ACTION" || -z "$TARGET" ]]; then
    echo -e "${BOLD}${YELLOW}Usage:${RESET} ./excalibur.sh <install|uninstall> <library>"
    exit 1
fi

mkdir -p "$HIDDEN_WORKDIR" "$PROJECT_LIB_DIR" "$PROJECT_INCLUDE_DIR"

########################################
# Library: liblog
########################################
install_liblog() {
    local REPO="https://github.com/groove-net/liblog.c.git"
    local WORK=".liblog_$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' ')"
    local WD="$HIDDEN_WORKDIR/$WORK"

    spinner_start

    # Step 1 — Clone
    spinner_set_status "Cloning repository..."
    git clone --quiet "$REPO" "$WD"

    # Step 2 — Compile
    spinner_set_status "Compiling..."
    (
        cd "$WD/src"
        gcc -c -DLOG_USE_COLOR liblog.c -o liblog.o
    )

    # Step 3 — Archiving
    spinner_set_status "Archiving..."
    (
        cd "$WD/src"
        ar rcs liblog.a liblog.o
    )

    # Step 4 — Installing (and Cleanup)
    spinner_set_status "Installing..."
    cp "$WD/src/liblog.a" "$PROJECT_LIB_DIR/"
    cp "$WD/src/liblog.h" "$PROJECT_INCLUDE_DIR/"
    rm -rf "$WD"

    spinner_stop
    echo -e "${GREEN}✔ Installed liblog${RESET}"
}

uninstall_liblog() {
    local removed=false

    if [[ -f "$PROJECT_LIB_DIR/liblog.a" ]]; then
        rm "$PROJECT_LIB_DIR/liblog.a"
        removed=true
    fi

    if [[ -f "$PROJECT_INCLUDE_DIR/liblog.h" ]]; then
        rm "$PROJECT_INCLUDE_DIR/liblog.h"
        removed=true
    fi

    if [[ "$removed" = true ]]; then
        echo -e "${GREEN}✔ Uninstalled liblog${RESET}"
    else
        echo -e "${YELLOW}Nothing to uninstall for liblog${RESET}"
    fi
}
########################################

########################################
# Dispatcher
########################################
case "$ACTION" in
    install)
        # Check if library is already Installed
        if [[ -f "$PROJECT_LIB_DIR/$TARGET.a" && -f "$PROJECT_INCLUDE_DIR/$TARGET.h" ]]; then
            echo -e "${GREEN}✔ Already installed${RESET}"
            exit 0
        fi

        case "$TARGET" in
            liblog) install_liblog ;;
            *) echo -e "${RED}Unknown library: $TARGET${RESET}"; exit 1 ;;
        esac
        ;;
    uninstall)
        case "$TARGET" in
            liblog) uninstall_liblog ;;
            *) echo -e "${RED}Unknown library: $TARGET${RESET}"; exit 1 ;;
        esac
        ;;
    *)
        echo -e "${RED}Unknown action: $ACTION${RESET}"
        exit 1
        ;;
esac

```

Make sure you:

```bash
❯ chmod +x excalibur.sh
```

Now you can use it to install `liblog`, a simple logging library implemented in C99

```bash
❯ ./excalibur.sh install liblog
```
