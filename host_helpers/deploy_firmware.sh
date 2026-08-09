#!/usr/bin/env bash
#
# deploy_firmware.sh — push everything the board needs for ntt, then optionally run.
#
# Transfers from PC:
#   binary_container_1.xclbin  ->  /lib/firmware/xilinx/ntt/ntt.bin  (+ ~/ntt.bin for XRT)
#   shell.json                 ->  /lib/firmware/xilinx/ntt/shell.json
#   ntt_host                   ->  ~/ntt_host
#   runner.sh                  ->  ~/runner.sh
#
# On the board (already there):
#   ~/pl.dtbo                  ->  /lib/firmware/xilinx/ntt/pl.dtbo
#
# Usage:
#   ./deploy_firmware.sh [user@host]
#   ./deploy_firmware.sh [user@host] --run
#
# Optional env: BOARD, APP, XCLBIN, HOST_BIN, PROJECT_REPO
#
# Password prompts: SSH connection sharing means you should only unlock SSH
# once, then sudo once on the board (TTY).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"
PROJECT_REPO="${PROJECT_REPO:-/home/riley/Desktop/COMP4601/project-repo}"

# ---------- defaults ----------
APP="${APP:-ntt}"
BOARD="${BOARD:-petalinux@192.168.8.203}"
XCLBIN_DEFAULT="binary_container_1.xclbin"
HOST_NAME="ntt_host"
REMOTE_FW="/lib/firmware/xilinx/${APP}"
DO_RUN=0

# ---------- colours ----------
if [[ -t 1 ]]; then
    RED=$'\033[0;31m';  GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
    CYAN=$'\033[0;36m';  BOLD=$'\033[1m'; RESET=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi
info() { echo "${CYAN}==>${RESET} ${BOLD}$*${RESET}"; }
ok()   { echo "${GREEN} ✓${RESET} $*"; }
warn() { echo "${YELLOW} !${RESET} $*"; }
err()  { echo "${RED} ✗ $*${RESET}" >&2; }

usage() {
    sed -n '2,22p' "$0" | sed 's/^# \?//'
}

# ---------- args ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --run) DO_RUN=1; shift ;;
        *@*) BOARD="$1"; shift ;;
        *)
            if [[ -f "$1" ]]; then
                XCLBIN="$1"
                shift
            else
                err "Unknown arg: $1"
                usage
                exit 1
            fi
            ;;
    esac
done

# ---------- resolve local artifacts ----------
XCLBIN="${XCLBIN:-}"
if [[ -z "$XCLBIN" ]]; then
    for cand in \
        "${PROJECT_REPO}/ntt_sys/build/hw/hw_link/${XCLBIN_DEFAULT}" \
        "${HERE}/${XCLBIN_DEFAULT}" \
        "./${XCLBIN_DEFAULT}"
    do
        if [[ -f "$cand" ]]; then
            XCLBIN="$cand"
            break
        fi
    done
fi

HOST_BIN="${HOST_BIN:-}"
if [[ -z "$HOST_BIN" ]]; then
    for cand in \
        "${PROJECT_REPO}/ntt_host/build/hw/${HOST_NAME}" \
        "${REPO}/ntt_host/build/hw/${HOST_NAME}" \
        "${HERE}/${HOST_NAME}" \
        "./${HOST_NAME}"
    do
        if [[ -f "$cand" ]]; then
            HOST_BIN="$cand"
            break
        fi
    done
fi

SHELL_JSON="${HERE}/shell.json"
RUNNER="${HERE}/runner.sh"
BIN_NAME="${APP}.bin"

missing=0
for label_path in \
    "xclbin:${XCLBIN:-}" \
    "host:${HOST_BIN:-}" \
    "shell.json:${SHELL_JSON}" \
    "runner.sh:${RUNNER}"
do
    label="${label_path%%:*}"
    path="${label_path#*:}"
    if [[ -z "$path" || ! -f "$path" ]]; then
        err "Missing ${label}: ${path:-<not found>}"
        missing=1
    fi
done
if [[ "$missing" -ne 0 ]]; then
    echo
    echo "Expected defaults:"
    echo "  xclbin: ${PROJECT_REPO}/ntt_sys/build/hw/hw_link/${XCLBIN_DEFAULT}"
    echo "  host:   ${PROJECT_REPO}/ntt_host/build/hw/${HOST_NAME}"
    exit 1
fi

# ---------- SSH multiplexing: one password for all scp/ssh ----------
CTRL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ntt-ssh.XXXXXX")"
CTRL_PATH="${CTRL_DIR}/sock"
cleanup_ssh() {
    ssh -o ControlPath="$CTRL_PATH" -O exit "$BOARD" 2>/dev/null || true
    rm -rf "$CTRL_DIR"
}
trap cleanup_ssh EXIT

SSH_OPTS=(
    -o "ControlMaster=auto"
    -o "ControlPath=${CTRL_PATH}"
    -o "ControlPersist=5m"
)

echo
info "Board:      ${BOARD}"
info "App slot:   ${REMOTE_FW}"
info "Bitstream:  ${XCLBIN}"
info "Host:       ${HOST_BIN}"
info "shell.json: ${SHELL_JSON}"
info "runner:     ${RUNNER}"
echo
info "Opening SSH master connection (enter board login password once)"
ssh "${SSH_OPTS[@]}" -fN "$BOARD"
ok "SSH session shared for the rest of this script"

# ---------- upload straight to the /tmp names we install from ----------
info "Uploading artifacts to /tmp"
scp "${SSH_OPTS[@]}" "$XCLBIN"      "${BOARD}:/tmp/${BIN_NAME}"
scp "${SSH_OPTS[@]}" "$SHELL_JSON"  "${BOARD}:/tmp/shell.json"
scp "${SSH_OPTS[@]}" "$HOST_BIN"    "${BOARD}:/tmp/${HOST_NAME}"
scp "${SSH_OPTS[@]}" "$RUNNER"      "${BOARD}:/tmp/runner.sh"
ok "Upload complete"

# ---------- one TTY session: sudo once, install everything ----------
info "Installing firmware + home files (sudo password once if required)"
ssh "${SSH_OPTS[@]}" -t "$BOARD" "\
  set -euo pipefail; \
  sudo mkdir -p '${REMOTE_FW}'; \
  sudo mv '/tmp/${BIN_NAME}' '${REMOTE_FW}/${BIN_NAME}'; \
  sudo mv /tmp/shell.json '${REMOTE_FW}/shell.json'; \
  if [[ -f \"\$HOME/pl.dtbo\" ]]; then \
    sudo cp \"\$HOME/pl.dtbo\" '${REMOTE_FW}/pl.dtbo'; \
    echo 'Installed pl.dtbo from \$HOME/pl.dtbo'; \
  elif [[ -f '${REMOTE_FW}/pl.dtbo' ]]; then \
    echo 'pl.dtbo already in firmware slot'; \
  else \
    echo 'WARNING: no pl.dtbo in \$HOME or ${REMOTE_FW}' >&2; \
  fi; \
  cp '${REMOTE_FW}/${BIN_NAME}' \"\$HOME/${BIN_NAME}\"; \
  mv /tmp/${HOST_NAME} \"\$HOME/${HOST_NAME}\"; \
  chmod +x \"\$HOME/${HOST_NAME}\"; \
  mv /tmp/runner.sh \"\$HOME/runner.sh\"; \
  chmod +x \"\$HOME/runner.sh\"; \
  echo '--- firmware ---'; \
  ls -la '${REMOTE_FW}'; \
  echo '--- home ---'; \
  ls -la \"\$HOME/${BIN_NAME}\" \"\$HOME/${HOST_NAME}\" \"\$HOME/runner.sh\" \
"
ok "Installed firmware + host + runner"

echo
if [[ "$DO_RUN" -eq 1 ]]; then
    info "Running ~/runner.sh on the board"
    echo
    ssh "${SSH_OPTS[@]}" -t "$BOARD" "cd \$HOME && ./runner.sh"
else
    ok "Deploy done. On the board run:"
    echo "    ./runner.sh"
    echo "Or from this machine:"
    echo "    $0 ${BOARD} --run"
fi
