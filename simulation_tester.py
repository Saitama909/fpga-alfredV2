#!/usr/bin/env python3
"""Drive headless Unified Vitis HLS: C-sim -> C-synthesis -> report QoR tables."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# User configuration
# ---------------------------------------------------------------------------
WORKSPACE_PATH = "/home/riley/Desktop/COMP4601/project-repo"
COMPONENT_NAME = "ntt_core"

# Optional: leave empty to auto-detect from PATH.
VITIS_BIN_DIR = "/home/riley/Xilinx/2025.2/Vitis/bin"

# Results are written under <repo>/results/ by default.
REPO_ROOT = Path(__file__).resolve().parent
RESULTS_DIR = REPO_ROOT / "results"

# ---------------------------------------------------------------------------
# ANSI colours
# ---------------------------------------------------------------------------
RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
MAGENTA = "\033[35m"


def colour(text: str, *codes: str) -> str:
    if not sys.stdout.isatty():
        return text
    return f"{''.join(codes)}{text}{RESET}"


def section(title: str) -> None:
    print()
    print(colour(f"=== {title} ===", BOLD, CYAN))


def ok(msg: str) -> None:
    print(colour(f"✓ {msg}", GREEN))


def fail(msg: str) -> None:
    print(colour(f"✗ {msg}", RED), file=sys.stderr)


def warn(msg: str) -> None:
    print(colour(f"! {msg}", YELLOW))


@dataclass
class CliOptions:
    console_print: bool
    no_save: bool
    dry_run: bool

    @property
    def should_save(self) -> bool:
        return not self.no_save and not self.dry_run

    @property
    def should_parse_reports(self) -> bool:
        # Dry-run only checks that sim/synth succeed.
        return not self.dry_run


def parse_args(argv: list[str] | None = None) -> CliOptions:
    parser = argparse.ArgumentParser(
        description=(
            "Run Vitis HLS C simulation and C synthesis against a Unified Vitis "
            "workspace, then report timing/latency QoR."
        )
    )
    parser.add_argument(
        "--console-print",
        action="store_true",
        help=(
            "Print the full Timing, Latency (with detail), and Performance & "
            "Resource tables to the console. Default console output is only the "
            "Timing summary and Latency summary."
        ),
    )
    parser.add_argument(
        "--no-save",
        action="store_true",
        help="Do not write a results-TIMESTAMP.txt file under results/.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "Run C simulation and C synthesis only to verify they succeed. "
            "Skips report parsing and does not save results."
        ),
    )
    args = parser.parse_args(argv)
    return CliOptions(
        console_print=args.console_print,
        no_save=args.no_save,
        dry_run=args.dry_run,
    )


def find_vitis_tool(name: str) -> Path:
    if VITIS_BIN_DIR:
        candidate = Path(VITIS_BIN_DIR) / name
        if candidate.is_file():
            return candidate
    which = shutil.which(name)
    if which:
        return Path(which)
    raise FileNotFoundError(
        f"Could not find '{name}'. Set VITIS_BIN_DIR at the top of this script."
    )


def load_component_paths(workspace: Path, component: str) -> tuple[Path, Path]:
    """Return (hls_config.cfg, work_dir) for the HLS component."""
    comp_dir = workspace / component
    if not comp_dir.is_dir():
        raise FileNotFoundError(f"Component directory not found: {comp_dir}")

    cfg = comp_dir / "hls_config.cfg"
    if not cfg.is_file():
        raise FileNotFoundError(f"Missing config file: {cfg}")

    work_dir_name = "hls"
    meta = comp_dir / "vitis-comp.json"
    if meta.is_file():
        data = json.loads(meta.read_text())
        work_dir_name = (
            data.get("configuration", {}).get("work_dir")
            or data.get("work_dir")
            or work_dir_name
        )

    work_dir = comp_dir / work_dir_name
    work_dir.mkdir(parents=True, exist_ok=True)
    return cfg, work_dir


def run_with_timer(cmd: list[str], cwd: Path, label: str) -> subprocess.CompletedProcess:
    """Run a command, rewriting one console line with elapsed seconds."""
    print(colour(f"$ {' '.join(cmd)}", DIM))
    start = time.time()
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdout is not None
    output_lines: list[str] = []
    last_printed = -1

    while True:
        line = proc.stdout.readline()
        if line:
            output_lines.append(line.rstrip("\n"))
        elif proc.poll() is not None:
            break

        elapsed = int(time.time() - start)
        if elapsed != last_printed:
            last_printed = elapsed
            msg = f"working on it {elapsed:02d}"
            if sys.stdout.isatty():
                sys.stdout.write(f"\r{colour(msg, YELLOW)}\033[K")
                sys.stdout.flush()
            else:
                if elapsed == 0 or elapsed % 10 == 0:
                    print(msg)

    rest = proc.stdout.read()
    if rest:
        output_lines.extend(rest.splitlines())

    proc.wait()
    elapsed = int(time.time() - start)
    if sys.stdout.isatty():
        sys.stdout.write("\r\033[K")
        sys.stdout.flush()
    print(colour(f"finished {label} in {elapsed}s (exit {proc.returncode})", DIM))

    return subprocess.CompletedProcess(
        args=cmd,
        returncode=proc.returncode,
        stdout="\n".join(output_lines) + ("\n" if output_lines else ""),
        stderr="",
    )


def csim_passed(work_dir: Path, run: subprocess.CompletedProcess) -> bool:
    if run.returncode != 0:
        return False

    report_dir = work_dir / "hls" / "csim" / "report"
    logs = list(report_dir.glob("*_csim.log")) if report_dir.is_dir() else []
    combined = run.stdout
    for log in logs:
        combined += "\n" + log.read_text(errors="replace")

    if re.search(r"\bFAIL\b", combined):
        return False
    if re.search(r"\bPASS\b", combined):
        return True
    return "ERROR" not in combined.upper()


def find_report_dir(work_dir: Path) -> Path:
    report_dir = work_dir / "hls" / "syn" / "report"
    if not report_dir.is_dir():
        raise FileNotFoundError(f"Synthesis report directory missing: {report_dir}")
    return report_dir


def find_top_csynth_report(work_dir: Path) -> Path:
    report_dir = find_report_dir(work_dir)

    top_reports = [
        p
        for p in report_dir.glob("*_csynth.rpt")
        if "Pipeline" not in p.name and p.name != "csynth.rpt"
    ]
    if top_reports:
        return max(top_reports, key=lambda p: p.stat().st_mtime)

    summary = report_dir / "csynth.rpt"
    if summary.is_file():
        return summary
    raise FileNotFoundError(f"No csynth report found under {report_dir}")


def extract_section(text: str, header_regex: str, stop_regex: str) -> str | None:
    match = re.search(header_regex, text, flags=re.IGNORECASE | re.MULTILINE)
    if not match:
        return None
    start = match.start()
    stop = re.search(stop_regex, text[match.end() :], flags=re.IGNORECASE | re.MULTILINE)
    end = match.end() + stop.start() if stop else len(text)
    return text[start:end].rstrip() + "\n"


def wait_for_reports(work_dir: Path) -> None:
    ready = False
    for _ in range(30):
        try:
            report = find_top_csynth_report(work_dir)
            if report.stat().st_size > 0:
                ready = True
                break
        except FileNotFoundError:
            pass
        time.sleep(1)
        if sys.stdout.isatty():
            sys.stdout.write(f"\r{colour('waiting for report...', YELLOW)}\033[K")
            sys.stdout.flush()
    if sys.stdout.isatty():
        sys.stdout.write("\r\033[K")
        sys.stdout.flush()
    if not ready:
        raise FileNotFoundError("Timed out waiting for csynth report")


@dataclass
class QoRTables:
    top_report: Path
    summary_report: Path | None
    timing_summary: str | None
    latency_summary: str | None
    timing_full: str | None
    latency_full: str | None
    perf_res: str | None

    def full_text(self) -> str:
        parts: list[str] = []
        parts.append("=== Estimated Quality of Results ===\n")
        parts.append("--- Timing Estimates ---\n")
        parts.append((self.timing_full or self.timing_summary or "(missing)") + "\n")
        parts.append("--- Latency Estimates (cycles) ---\n")
        parts.append((self.latency_full or self.latency_summary or "(missing)") + "\n")
        parts.append("--- Performance & Resource Estimates ---\n")
        parts.append((self.perf_res or "(missing)") + "\n")
        return "\n".join(parts)


def collect_qor_tables(work_dir: Path) -> QoRTables:
    top_report = find_top_csynth_report(work_dir)
    top_text = top_report.read_text(errors="replace")
    summary_path = find_report_dir(work_dir) / "csynth.rpt"
    summary_text = summary_path.read_text(errors="replace") if summary_path.is_file() else ""

    timing_full = extract_section(
        top_text,
        r"^\+ Timing:\s*$",
        r"^\+ Latency:\s*$",
    )
    # Summary-only: Timing block without trailing blank noise is already small;
    # keep the whole + Timing section as the "summary" (it is only the clock table).
    timing_summary = timing_full

    latency_full = extract_section(
        top_text,
        r"^\+ Latency:\s*$",
        r"^={3,}\s*$",
    )
    latency_summary = extract_section(
        top_text,
        r"^\+ Latency:\s*$",
        r"^\s*\+ Detail:\s*$|^={3,}\s*$",
    )

    perf_res = extract_section(
        summary_text or top_text,
        r"^\* Performance & Resource Estimates:\s*$",
        r"^={3,}\s*$|^II Violation",
    )

    return QoRTables(
        top_report=top_report,
        summary_report=summary_path if summary_path.is_file() else None,
        timing_summary=timing_summary,
        latency_summary=latency_summary,
        timing_full=timing_full,
        latency_full=latency_full,
        perf_res=perf_res,
    )


def print_console_qor(qor: QoRTables, console_print: bool) -> None:
    print(colour(f"Top report     : {qor.top_report}", DIM))
    if qor.summary_report is not None:
        print(colour(f"Summary report : {qor.summary_report}", DIM))

    section("Estimated Quality of Results")

    print(colour("--- Timing Summary ---", BOLD, MAGENTA))
    if qor.timing_summary:
        print(qor.timing_summary)
    else:
        warn("Could not find Timing summary in report.")

    print(colour("--- Latency Summary (cycles) ---", BOLD, MAGENTA))
    if qor.latency_summary:
        print(qor.latency_summary)
    else:
        warn("Could not find Latency summary in report.")

    if not console_print:
        print(
            colour(
                "(Use --console-print for full Timing/Latency detail and "
                "Performance & Resource tables.)",
                DIM,
            )
        )
        return

    print(colour("--- Timing Estimates (full) ---", BOLD, MAGENTA))
    print(qor.timing_full or "(missing)")

    print(colour("--- Latency Estimates (full, cycles) ---", BOLD, MAGENTA))
    print(qor.latency_full or "(missing)")

    print(colour("--- Performance & Resource Estimates ---", BOLD, MAGENTA))
    if qor.perf_res:
        print(qor.perf_res)
    else:
        warn("Could not find Performance & Resource Estimates in report.")


def save_results(qor: QoRTables) -> Path:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    out_path = RESULTS_DIR / f"results-{int(time.time())}.txt"
    header = (
        f"Vitis HLS QoR snapshot\n"
        f"generated_unix: {int(time.time())}\n"
        f"top_report: {qor.top_report}\n"
        f"summary_report: {qor.summary_report}\n"
        f"{'-' * 72}\n\n"
    )
    out_path.write_text(header + qor.full_text(), encoding="utf-8")
    return out_path


def main(argv: list[str] | None = None) -> int:
    opts = parse_args(argv)

    workspace = Path(WORKSPACE_PATH).expanduser().resolve()
    if not workspace.is_dir():
        fail(f"WORKSPACE_PATH does not exist: {workspace}")
        return 1

    try:
        vitis_run = find_vitis_tool("vitis-run")
        vpp = find_vitis_tool("v++")
        cfg, work_dir = load_component_paths(workspace, COMPONENT_NAME)
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        fail(str(exc))
        return 1

    print(colour("Vitis HLS Simulation Tester", BOLD, CYAN))
    print(f"  workspace      : {workspace}")
    print(f"  component      : {COMPONENT_NAME}")
    print(f"  config         : {cfg}")
    print(f"  work_dir       : {work_dir}")
    print(f"  vitis-run      : {vitis_run}")
    print(f"  v++            : {vpp}")
    print(f"  --console-print: {opts.console_print}")
    print(f"  --no-save      : {opts.no_save}")
    print(f"  --dry-run      : {opts.dry_run}")

    env_bin = str(vitis_run.parent)
    path = os.environ.get("PATH", "")
    if env_bin not in path.split(os.pathsep):
        os.environ["PATH"] = env_bin + os.pathsep + path

    # ------------------------------------------------------------------
    # 1) C Simulation
    # ------------------------------------------------------------------
    section("Beginning C Simulation")
    csim = run_with_timer(
        [
            str(vitis_run),
            "--mode",
            "hls",
            "--csim",
            "--config",
            str(cfg),
            "--work_dir",
            str(work_dir),
        ],
        cwd=workspace,
        label="C simulation",
    )

    if not csim_passed(work_dir, csim):
        fail("C Simulation failed — skipping synthesis.")
        tail = "\n".join(csim.stdout.splitlines()[-40:])
        if tail.strip():
            print(tail)
        return 1
    ok("C Simulation passed")

    # ------------------------------------------------------------------
    # 2) C Synthesis
    # ------------------------------------------------------------------
    section("Beginning C Synthesis")
    synth = run_with_timer(
        [
            str(vpp),
            "-c",
            "--mode",
            "hls",
            "--config",
            str(cfg),
            "--work_dir",
            str(work_dir),
        ],
        cwd=workspace,
        label="C synthesis",
    )

    if synth.returncode != 0:
        fail("C Synthesis failed.")
        tail = "\n".join(synth.stdout.splitlines()[-40:])
        if tail.strip():
            print(tail)
        return 1
    ok("C Synthesis finished")

    if not opts.should_parse_reports:
        ok("Dry-run complete (sim + synth OK; results not parsed or saved)")
        return 0

    # ------------------------------------------------------------------
    # 3) Parse / print / optionally save QoR tables
    # ------------------------------------------------------------------
    section("Parsing Synthesis Report")
    try:
        wait_for_reports(work_dir)
        qor = collect_qor_tables(work_dir)
    except FileNotFoundError as exc:
        fail(str(exc))
        return 1

    print_console_qor(qor, console_print=opts.console_print)

    if opts.should_save:
        out_path = save_results(qor)
        ok(f"Saved full tables to {out_path}")
    elif opts.no_save:
        print(colour("Skipping save (--no-save).", DIM))

    ok("Done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
