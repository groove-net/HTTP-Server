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
