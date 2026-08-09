#!/usr/bin/env bash
#
# runner.sh — ON THE BOARD: load the ntt firmware slot and run ntt_host.
#
# Expects in \$HOME (put there by deploy_firmware.sh):
#   ./ntt_host
#   ./ntt.bin
# And firmware already installed at:
#   /lib/firmware/xilinx/ntt/{pl.dtbo,ntt.bin,shell.json}
#
# Usage (on the board):
#   ./runner.sh
#   ./runner.sh ntt          # same (app name default)
#
# From your PC after deploy:
#   ./deploy_firmware.sh petalinux@<ip> --run

set -uo pipefail

# ---------- colours ----------
if [[ -t 1 ]]; then
    RED=$'\033[0;31m';  GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
    BLUE=$'\033[0;34m'; CYAN=$'\033[0;36m';  BOLD=$'\033[1m'; RESET=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; CYAN=''; BOLD=''; RESET=''
fi

info() { echo "${CYAN}==>${RESET} ${BOLD}$*${RESET}"; }
ok()   { echo "${GREEN} ✓${RESET} $*"; }
warn() { echo "${YELLOW} !${RESET} $*"; }
err()  { echo "${RED} ✗ $*${RESET}" >&2; }

APP="${1:-ntt}"
BIN="${APP}.bin"
HOST="./ntt_host"
FW_DIR="/lib/firmware/xilinx/${APP}"

cd "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null || cd "$HOME"
# Prefer running from home when copied there by deploy
if [[ ! -f "$HOST" && -f "${HOME}/ntt_host" ]]; then
    cd "$HOME"
fi

echo
echo "${BOLD}${BLUE}==== runner: ${APP} ====${RESET}"
echo

# ---------- sanity: firmware slot ----------
info "Checking firmware slot ${FW_DIR}"
for f in pl.dtbo "$BIN" shell.json; do
    if [[ ! -f "${FW_DIR}/${f}" ]]; then
        err "Missing ${FW_DIR}/${f}"
        echo "  Deploy from the PC with host_helpers/deploy_firmware.sh"
        echo "  (pl.dtbo must already be in the slot from a previous lab)"
        exit 1
    fi
done
ok "Firmware files present"

# Prefer ~/ntt.bin for XRT; fall back to firmware copy
if [[ ! -f "$BIN" ]]; then
    if [[ -f "${FW_DIR}/${BIN}" ]]; then
        info "Copying ${FW_DIR}/${BIN} -> ./${BIN} for XRT"
        cp "${FW_DIR}/${BIN}" "./${BIN}"
    else
        err "Bitstream ${BIN} not found in \$PWD or ${FW_DIR}"
        exit 1
    fi
fi

if [[ ! -f "$HOST" ]]; then
    err "Host executable ${HOST} not found."
    exit 1
fi
chmod +x "$HOST"

# ---------- unload / load ----------
info "Unloading current application"
if sudo xmutil unloadapp; then
    ok "Unloaded"
else
    warn "Nothing to unload (or slot already empty) — continuing anyway."
fi
echo

info "Loading application '${APP}'"
if ! sudo xmutil loadapp "$APP"; then
    err "Failed to load '${APP}'. Check: sudo xmutil listapps"
    exit 1
fi
ok "Loaded '${APP}'"
echo

# ---------- run host ----------
# Default -b 128 matches POLY_MUL_BATCH in hls/src/ntt_top.h
BATCH="${BATCH:-128}"
info "Running ${HOST} -x ${BIN} -b ${BATCH}"
echo "${BLUE}------------------------------------------------------------${RESET}"
"$HOST" -x "$BIN" -b "$BATCH"
status=$?
echo "${BLUE}------------------------------------------------------------${RESET}"

if [[ $status -eq 0 ]]; then
    ok "Done — ${APP} exited cleanly."
else
    err "${APP} exited with status ${status}."
fi
exit $status
