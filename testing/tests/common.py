# Shared helpers for the step modules under testing/tests/.

import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
LOGS = HERE / "logs"
OUTS = HERE / "outputs"
KEEP_NAMES = {".gitkeep"}

# Step status (PASSED / done)
GREEN = "\033[32m"
# Testbench / Vitis lines that start with PASS
PASS_GREEN = "\033[92m"
RED = "\033[31m"
BLUE = "\033[34m"
YELLOW = "\033[33m"
GRAY = "\033[90m"
RESET = "\033[0m"


def now():
    return datetime.now().isoformat(timespec="seconds")


def colour(text, code):
    if not sys.stdout.isatty():
        return text
    return f"{code}{text}{RESET}"


# format_tool_line
# Colour WARNING / ERROR / PASS tags for console display (log file stays plain).
def format_tool_line(line):
    stripped = line.lstrip()
    if stripped.startswith("PASS"):
        # colour only the leading PASS token (PASS / PASSED / ...)
        m = re.match(r"^(\s*)(PASS\w*)(.*)$", line)
        if m:
            return m.group(1) + colour(m.group(2), PASS_GREEN) + m.group(3)
    if "WARNING" in line:
        line = line.replace("WARNING", colour("WARNING", YELLOW), 1)
    if "ERROR" in line:
        line = line.replace("ERROR", colour("ERROR", RED), 1)
    if "CRITICAL" in line:
        line = line.replace("CRITICAL", colour("CRITICAL", RED), 1)
    return line


# should_show_line
# Skip INFO: lines if suppress_info is True
def should_show_line(line, suppress_info):
    if suppress_info and line.lstrip().startswith("INFO:"):
        return False
    return True


# print_running
# Print a running banner for the given step
def print_running(what):
    # e.g. RUNNING csim ========================================
    bar = "=" * 40
    print(f"\n{colour('RUNNING', BLUE)} {what} {bar}")


# print_passed
# Print a passed banner for the given step
def print_passed(prefix=""):
    bar = "=" * 40
    label = colour("PASSED", GREEN)
    msg = f"{prefix} {label} {bar}".strip() if prefix else f"{label} {bar}"
    print(msg)


# print_failed
# Print a failed banner for the given step
def print_failed(prefix=""):
    bar = "=" * 40
    label = colour("FAILED", RED)
    msg = f"{prefix} {label} {bar}".strip() if prefix else f"{label} {bar}"
    print(msg)


# print_skip
# Print a skipped banner for the given step
def print_skip(what):
    print(f"\n{colour('SKIP', BLUE)} {what}")


# print_done
# Print a done banner for the given step
def print_done(elapsed_s=None):
    msg = colour("done", GREEN)
    if elapsed_s is not None:
        total = max(0, int(round(elapsed_s)))
        mins, secs = divmod(total, 60)
        msg = f"{msg}. took {mins} min, {secs} seconds"
    print(f"\n{msg}")


# purge_folders
# Delete files under logs/ and outputs/, keeping .gitkeep placeholders.
def purge_folders():
    removed = 0
    for folder in (LOGS, OUTS):
        folder.mkdir(parents=True, exist_ok=True)
        for path in folder.iterdir():
            if path.name in KEEP_NAMES:
                continue
            if path.is_file():
                path.unlink()
                removed += 1
            elif path.is_dir():
                shutil.rmtree(path)
                removed += 1
    return removed


# resource_threshold_notes
# Compare each csynth resource % against config thresholds.
# Returns list of (level, message) where level is 'warn'|'high'|'over'.
def resource_threshold_notes(res, warn, high, over):
    notes = []
    for name in ("BRAM", "DSP", "FF", "LUT", "URAM"):
        if name not in res:
            continue
        _used, _avail, pct_s = res[name]
        try:
            pct = float(str(pct_s).lstrip("~"))
        except ValueError:
            continue
        if pct >= over:
            notes.append(("over", f"{name} {pct:g}%"))
        elif pct >= high:
            notes.append(("high", f"{name} {pct:g}%"))
        elif pct >= warn:
            notes.append(("warn", f"{name} {pct:g}%"))
    return notes


# print_run_summary
# Print final pass/fail + timing/usage tables + resource warnings.
def print_run_summary(step_status, results, warn, high, over):
    print("===========================================")
    print(
        """
     ▗▄▄▖▗▖ ▗▖▗▖  ▗▖▗▖  ▗▖ ▗▄▖ ▗▄▄▖▗▖  ▗▖
    ▐▌   ▐▌ ▐▌▐▛▚▞▜▌▐▛▚▞▜▌▐▌ ▐▌▐▌ ▐▌▝▚▞▘ 
     ▝▀▚▖▐▌ ▐▌▐▌  ▐▌▐▌  ▐▌▐▛▀▜▌▐▛▀▚▖ ▐▌  
    ▗▄▄▞▘▝▚▄▞▘▐▌  ▐▌▐▌  ▐▌▐▌ ▐▌▐▌ ▐▌ ▐▌                                   
    """
    )
    width = max((len(name) for name in step_status), default=8)
    for name, status in step_status.items():
        if status == "PASSED":
            label = colour(status, GREEN)
        elif status == "FAILED":
            label = colour(status, RED)
        else:
            label = colour(status, BLUE)
        print(f"  {name:<{width}}  {label}")

    plain = ["Summary", ""]
    for name, status in step_status.items():
        plain.append(f"  {name}: {status}")

    # --- Timing ---
    has_timing = any(
        results.get(k) is not None
        for k in (
            "clock_target",
            "lat_max",
            "cosim_lat_max",
            "cosim_lat",
        )
    )

    # TODO: Like in the main run_tets file, if have time permitting, make this agnostic. Not a big priority though
    if has_timing:
        print("")
        print("Timing")
        plain.append("")
        plain.append("Timing")
        clk_t = results.get("clock_target") or "-"
        clk_e = results.get("clock_est") or "-"
        lat_lo = results.get("lat_min") or "-"
        lat_hi = results.get("lat_max") or "-"
        cosim = results.get("cosim_lat_max") or results.get("cosim_lat") or "-"
        rows = [
            f"  {'clock target (ns): '} {((str(clk_t)) if clk_t != '-' else '-')}",
            f"  {'clock est. (ns): '} {((str(clk_e)) if clk_e != '-' else '-')}",
            f"  {'csynth lat. (cycles): '} {(f'{lat_lo} to {lat_hi}' if lat_hi != '-' else '-')}",
            f"  {'cosim lat (cycles): '} {(str(cosim) if cosim == '-' else str(cosim))}",
        ]
        for line in rows:
            print(line)
            plain.append(line)

    # --- Module intervals (per DATAFLOW instance from csynth) ---
    modules = results.get("module_ii") or []
    if modules:
        print("")
        print("Module intervals")
        plain.append("")
        plain.append("Module intervals")
        width = max(len(name) for name, _ in modules)
        width = max(width, 8)
        for name, ii in modules:
            line = f"  {name:<{width}}  {ii}"
            print(line)
            plain.append(line)

    # --- Usage ---
    # Get the resource usage from the results dictionary
    res = results.get("res") or {}
    if res:
        print("")
        print("Usage")
        plain.append("")
        plain.append("Usage")
        hdr = f"  {'resource':<8} {'used':>8} {'avail':>8} {'pct':>6}"
        print(hdr)
        plain.append(hdr)
        for name in ("BRAM", "DSP", "FF", "LUT", "URAM"):
            if name not in res:
                continue
            used, avail, pct = res[name]
            line = f"  {name:<8} {used:>8} {avail:>8} {str(pct) + '%':>6}"
            print(line)
            plain.append(line)

    notes = resource_threshold_notes(res, warn, high, over)
    if notes:
        print("")
        print("Resource warnings")
        plain.append("")
        plain.append("Resource warnings")
        for level, msg in notes:
            tag = colour("ERROR", RED) if level == "over" else colour("WARNING", YELLOW)
            print(f"  {tag}  {msg}")
            plain.append(f"  {'ERROR' if level == 'over' else 'WARNING'}  {msg}")

    (OUTS / "run-summary.txt").write_text("\n".join(plain) + "\n")


# _count_warnings_errors
# Count WARNING / ERROR lines in a log body (ignore an existing result block).
def _count_warnings_errors(text):
    warnings = 0
    errors = 0
    for line in text.splitlines():
        if line.strip().startswith("===== Test Result"):
            break
        upper = line.upper()
        # Vitis tags like WARNING: [HLS ...] / ERROR: [HLS ...]
        if re.search(r"\bERROR\b|\bCRITICAL\b", upper):
            errors += 1
        elif re.search(r"\bWARNING\b", upper):
            warnings += 1
    return warnings, errors


# footer
# Append a result block to the log, e.g.
# ===== Test Result ======
# PASSED 2026-07-27T16:57:32
# Warnings: 3
# Errors: 0
def footer(log_path, status):
    body = ""
    if log_path.is_file():
        body = log_path.read_text(errors="replace")
        # drop a previous result block if we somehow re-append
        marker = "\n===== Test Result ======"
        if marker in body:
            body = body[: body.index(marker)]
            log_path.write_text(body)

    warnings, errors = _count_warnings_errors(body)
    with log_path.open("a") as f:
        f.write("\n===== Test Result ======\n")
        f.write(f"{status} {now()}\n")
        f.write(f"Warnings: {warnings}\n")
        f.write(f"Errors: {errors}\n")


# find_tool
# Find a tool in the PATH, or throw an error if not found.
def find_tool(name, bin_dir):
    if bin_dir:
        p = Path(bin_dir).expanduser() / name
        if p.is_file():
            return p
    which = shutil.which(name)
    if which:
        return Path(which)
    raise FileNotFoundError(f"Can't find {name}. Set VITIS_BIN_DIR in config.txt")


# component_paths
# Return the config/work paths for a given component.
def component_paths(workspace, component):
    comp = workspace / component
    if not comp.is_dir():
        raise FileNotFoundError(f"No component dir: {comp}")
    cfg = comp / "hls_config.cfg"
    if not cfg.is_file():
        raise FileNotFoundError(f"Missing {cfg}")

    work_name = "hls"
    meta = comp / "vitis-comp.json"
    if meta.is_file():
        data = json.loads(meta.read_text())
        work_name = (
            data.get("configuration", {}).get("work_dir")
            or data.get("work_dir")
            or work_name
        )
    work = comp / work_name
    work.mkdir(parents=True, exist_ok=True)
    return cfg, work


# run_cmd
# Run a command, always writing full output to log_path.
# Streams to the console; if suppress_info is True, skip lines that start with INFO:.
def run_cmd(cmd, cwd, log_path, env_extra=None, suppress_info=False):
    print("$ " + " ".join(cmd))

    env = dict(os.environ)
    env["PYTHONUNBUFFERED"] = "1"
    if env_extra:
        env.update(env_extra)

    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )

    lines = []
    log = log_path.open("w")
    log.write("$ " + " ".join(cmd) + "\n\n")

    for line in proc.stdout:
        text = line.rstrip("\n")
        lines.append(text)
        log.write(text + "\n")
        if should_show_line(text, suppress_info):
            print(format_tool_line(text), flush=True)

    proc.wait()
    log.close()
    return proc.returncode, "\n".join(lines)
