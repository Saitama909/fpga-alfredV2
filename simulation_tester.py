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
    jobs: int

    @property
    def should_save(self) -> bool:
        return not self.no_save and not self.dry_run

    @property
    def should_parse_reports(self) -> bool:
        # Dry-run only checks that sim/synth succeed.
        return not self.dry_run


def parse_args(argv: list[str] | None = None) -> CliOptions:
    default_jobs = os.cpu_count() or 1
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
        help="Do not write a results-TIMESTAMP.md file under results/.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "Run C simulation and C synthesis only to verify they succeed. "
            "Skips report parsing and does not save results."
        ),
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=default_jobs,
        metavar="N",
        help=(
            "Parallel job/thread hint for Vitis HLS (passed as --hls.jobs and "
            f"XILINX_NUM_THREADS). Default: {default_jobs} (this machine's CPU count). "
            "C-synth is still often mostly single-threaded; this helps where the "
            "tools can parallelise."
        ),
    )
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    return CliOptions(
        console_print=args.console_print,
        no_save=args.no_save,
        dry_run=args.dry_run,
        jobs=args.jobs,
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


def run_streaming(
    cmd: list[str],
    cwd: Path,
    label: str,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess:
    """Run a command, streaming its stdout/stderr live to this console."""
    print(colour(f"$ {' '.join(cmd)}", DIM))
    start = time.time()
    env = {**os.environ, "PYTHONUNBUFFERED": "1"}
    if extra_env:
        env.update(extra_env)
    # Merge stderr into stdout so warnings/errors appear in order with the log.
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )
    assert proc.stdout is not None
    output_lines: list[str] = []

    for line in proc.stdout:
        text = line.rstrip("\n")
        output_lines.append(text)
        print(text, flush=True)

    proc.wait()
    elapsed = int(time.time() - start)
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


def _is_table_border(line: str) -> bool:
    s = line.strip()
    return bool(s) and s.startswith("+") and set(s) <= set("+-")


def _is_table_row(line: str) -> bool:
    s = line.strip()
    return s.startswith("|") and s.endswith("|") and len(s) > 1


def _split_table_row(line: str) -> list[str]:
    # Keep empty cells; drop the blank entries from leading/trailing '|'.
    return [cell.strip() for cell in line.strip().split("|")[1:-1]]


def _unique_headers(headers: list[str]) -> list[str]:
    seen: dict[str, int] = {}
    out: list[str] = []
    for raw in headers:
        base = raw if raw else "col"
        n = seen.get(base, 0)
        seen[base] = n + 1
        out.append(base if n == 0 else f"{base} ({n + 1})")
    return out


def _friendly_headers(headers: list[str]) -> list[str]:
    """Rename common HLS duplicate min/max header patterns."""
    # Latency summary: min max min max min max Type
    if len(headers) == 7 and [h.lower() for h in headers[:6]] == [
        "min",
        "max",
        "min",
        "max",
        "min",
        "max",
    ]:
        return [
            "Latency min (cycles)",
            "Latency max (cycles)",
            "Absolute min",
            "Absolute max",
            "Interval min",
            "Interval max",
            headers[6] or "Pipeline",
        ]

    # Instance detail: Instance Module min max min max min max Type
    if (
        len(headers) == 9
        and headers[0].lower() == "instance"
        and headers[1].lower() == "module"
    ):
        return [
            "Instance",
            "Module",
            "Latency min (cycles)",
            "Latency max (cycles)",
            "Absolute min",
            "Absolute max",
            "Interval min",
            "Interval max",
            headers[8] or "Pipeline",
        ]

    # Loop detail often already has decent names after merge.
    return _unique_headers(headers)


def _merge_header_rows(header_rows: list[list[str]]) -> list[str]:
    """Combine multi-line HLS headers when column counts match; else use last row."""
    if not header_rows:
        return []
    if len(header_rows) == 1:
        return _friendly_headers(header_rows[0])

    widths = [len(r) for r in header_rows]
    # Perf-style tables often differ by a trailing empty cell (14 vs 15).
    # Latency-style tables use a short spanning header (4) above a fine header (7)
    # — those must NOT be zip-merged or columns misalign.
    if max(widths) - min(widths) <= 1:
        width = max(widths)
        padded = [r + [""] * (width - len(r)) for r in header_rows]
        merged: list[str] = []
        for col in zip(*padded):
            parts = [c for c in col if c]
            deduped: list[str] = []
            for part in parts:
                if not deduped or deduped[-1] != part:
                    deduped.append(part)
            merged.append(" ".join(deduped) if deduped else "")
        return _friendly_headers(merged)

    return _friendly_headers(header_rows[-1])


def _markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    if not headers:
        return ""
    width = len(headers)
    normalised: list[list[str]] = []
    for row in rows:
        cells = list(row[:width]) + [""] * max(0, width - len(row))
        # Escape pipes so markdown stays intact.
        normalised.append([c.replace("|", "\\|") for c in cells])
    header_cells = [h.replace("|", "\\|") for h in headers]

    lines = [
        "| " + " | ".join(header_cells) + " |",
        "| " + " | ".join("---" for _ in header_cells) + " |",
    ]
    for row in normalised:
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def hls_ascii_to_markdown(text: str) -> str:
    """Convert HLS report ASCII tables in a section into GitHub-flavoured markdown tables."""
    if not text or not text.strip():
        return "_Missing from synthesis report._"

    lines = text.splitlines()
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if _is_table_border(line):
            # Parse one ASCII table: border, header rows, border, data rows, border.
            i += 1
            header_rows: list[list[str]] = []
            while i < len(lines) and _is_table_row(lines[i]):
                header_rows.append(_split_table_row(lines[i]))
                i += 1
            if i < len(lines) and _is_table_border(lines[i]):
                i += 1
            data_rows: list[list[str]] = []
            while i < len(lines) and _is_table_row(lines[i]):
                data_rows.append(_split_table_row(lines[i]))
                i += 1
            if i < len(lines) and _is_table_border(lines[i]):
                i += 1

            headers = _merge_header_rows(header_rows)
            if headers and data_rows:
                out.append(_markdown_table(headers, data_rows))
                out.append("")
            elif headers:
                # Header-only table — still emit headers with no body.
                out.append(_markdown_table(headers, []))
                out.append("")
            continue

        stripped = line.strip()
        if not stripped:
            if out and out[-1] != "":
                out.append("")
            i += 1
            continue

        # Section labels from the report (* Summary:, + Detail:, etc.).
        if stripped.startswith("+") or stripped.startswith("*"):
            label = stripped.lstrip("+* ").rstrip(":")
            out.append(f"**{label}**")
            out.append("")
        elif stripped.startswith("Name Prefix:"):
            out.append(f"_{stripped}_")
            out.append("")
        else:
            out.append(stripped)
        i += 1

    # Collapse excessive blank lines.
    cleaned: list[str] = []
    for line in out:
        if line == "" and cleaned and cleaned[-1] == "":
            continue
        cleaned.append(line)
    return "\n".join(cleaned).rstrip() + "\n"


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
    utilization_summary: str | None

    def headline_metrics(self) -> dict[str, str]:
        """Best-effort parse of key numbers for the markdown summary table."""
        metrics: dict[str, str] = {}
        src = self.latency_summary or self.latency_full or ""
        # Latency summary data row: | min | max | abs_min | abs_max | iv_min | iv_max | type |
        lat = re.search(
            r"\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*([0-9.]+\s*\w+)\s*\|\s*([0-9.]+\s*\w+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|",
            src,
        )
        if lat:
            metrics["latency_min_cycles"] = lat.group(1)
            metrics["latency_max_cycles"] = lat.group(2)
            metrics["latency_min_time"] = lat.group(3)
            metrics["latency_max_time"] = lat.group(4)
            metrics["interval_min"] = lat.group(5)
            metrics["interval_max"] = lat.group(6)

        timing = self.timing_summary or self.timing_full or ""
        clk = re.search(
            r"\|\s*ap_clk\s*\|\s*([0-9.]+)\s*ns\s*\|\s*([0-9.]+)\s*ns\s*\|\s*([0-9.]+)\s*ns\s*\|",
            timing,
        )
        if clk:
            metrics["clock_target_ns"] = clk.group(1)
            metrics["clock_estimated_ns"] = clk.group(2)
            metrics["clock_uncertainty_ns"] = clk.group(3)

        util = self.utilization_summary or ""
        total = re.search(
            r"\|\s*Total\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
            util,
        )
        available = re.search(
            r"\|\s*Available\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
            util,
        )
        pct = re.search(
            r"\|\s*Utilization\s*\(%\)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
            util,
        )
        if total and available and pct:
            metrics["bram"] = f"{total.group(1).strip()} / {available.group(1).strip()} ({pct.group(1).strip()}%)"
            metrics["dsp"] = f"{total.group(2).strip()} / {available.group(2).strip()} ({pct.group(2).strip()}%)"
            metrics["ff"] = f"{total.group(3).strip()} / {available.group(3).strip()} ({pct.group(3).strip()}%)"
            metrics["lut"] = f"{total.group(4).strip()} / {available.group(4).strip()} ({pct.group(4).strip()}%)"
            metrics["uram"] = f"{total.group(5).strip()} / {available.group(5).strip()} ({pct.group(5).strip()}%)"
            metrics["lut_pct"] = pct.group(4).strip().lstrip("~")
        else:
            # Fallback: top row of Performance & Resource Estimates.
            perf = self.perf_res or ""
            top = re.search(
                r"\|\+\s*pqcrystals_kyber768_ref_ntt\s*\|[^|]*\|[^|]*\|[^|]*\|[^|]*\|[^|]*\|[^|]*\|\s*(\d+)\s*\|[^|]*\|[^|]*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
                perf,
            )
            if top:
                metrics["top_latency_cycles"] = top.group(1).strip()
                metrics["bram"] = top.group(2).strip()
                metrics["dsp"] = top.group(3).strip()
                metrics["ff"] = top.group(4).strip()
                metrics["lut"] = top.group(5).strip()
                m = re.search(r"\((\d+)%\)", metrics["lut"])
                if m:
                    metrics["lut_pct"] = m.group(1)
        return metrics

    def resource_usage_text(self) -> str:
        """Compact top-level resource table as plain aligned text."""
        util = self.utilization_summary or ""
        total = re.search(
            r"\|\s*Total\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
            util,
        )
        available = re.search(
            r"\|\s*Available\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
            util,
        )
        pct = re.search(
            r"\|\s*Utilization\s*\(%\)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
            util,
        )
        if total and available and pct:
            rows = []
            for i, name in enumerate(("BRAM", "DSP", "FF", "LUT", "URAM"), start=1):
                rows.append([
                    name,
                    total.group(i).strip(),
                    available.group(i).strip(),
                    pct.group(i).strip(),
                ])
            return aligned_table_text(
                ["Resource", "Total", "Available", "Utilization (%)"],
                rows,
            )

        metrics = self.headline_metrics()
        if not any(k in metrics for k in ("bram", "dsp", "ff", "lut")):
            return "Missing top-level resource usage from synthesis report.\n"

        rows = []
        for key, label in (
            ("bram", "BRAM"),
            ("dsp", "DSP"),
            ("ff", "FF"),
            ("lut", "LUT"),
            ("uram", "URAM"),
        ):
            if key in metrics:
                rows.append([label, metrics[key]])
        return aligned_table_text(["Resource", "Usage"], rows)

    def as_markdown(self) -> str:
        ts = int(time.time())
        metrics = self.headline_metrics()
        lines: list[str] = [
            "# Vitis HLS QoR Results",
            "",
            f"Generated (unix timestamp): `{ts}`",
            "",
            "## Source reports",
            "",
            f"- **Top report:** `{self.top_report}`",
        ]
        if self.summary_report is not None:
            lines.append(f"- **Summary report:** `{self.summary_report}`")
        lines.append("")

        lines.extend(["## Headline metrics", ""])
        if metrics:
            label_map = [
                ("latency_max_cycles", "Max latency (cycles)"),
                ("latency_min_cycles", "Min latency (cycles)"),
                ("latency_max_time", "Max latency (absolute, from report)"),
                ("latency_min_time", "Min latency (absolute, from report)"),
                ("interval_max", "Max initiation interval"),
                ("interval_min", "Min initiation interval"),
                ("clock_target_ns", "Clock target (ns)"),
                ("clock_estimated_ns", "Clock estimated (ns)"),
                ("clock_uncertainty_ns", "Clock uncertainty (ns)"),
                ("bram", "BRAM"),
                ("dsp", "DSP"),
                ("ff", "FF"),
                ("lut", "LUT"),
                ("uram", "URAM"),
            ]
            rows = [[label, metrics[key]] for key, label in label_map if key in metrics]
            lines.append(fence(aligned_table_text(["Metric", "Value"], rows)).rstrip())
        else:
            lines.append("_Could not parse headline metrics from the report tables._")
        lines.append("")

        lines.append("## Resource usage summary (top-level)")
        lines.append("")
        lines.append(fence(self.resource_usage_text()).rstrip())
        lines.append("")

        def add_section(title: str, body: str | None) -> None:
            lines.append(f"## {title}")
            lines.append("")
            if body and body.strip():
                # Keep original HLS ASCII tables in a code block so wide
                # columns are not squashed by markdown table rendering.
                lines.append(fence(body).rstrip())
            else:
                lines.append("_Missing from synthesis report._")
            lines.append("")

        add_section("Timing summary", self.timing_summary)
        add_section("Latency summary (cycles)", self.latency_summary)
        add_section("Timing estimates (full)", self.timing_full)
        add_section("Latency estimates (full, cycles)", self.latency_full)
        add_section("Performance & resource estimates", self.perf_res)
        return "\n".join(lines) + "\n"


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

    # Top-level Utilization Estimates summary (Total / Available / %).
    # Stop on the next named '== Section' header, not on '====' underlines.
    util_block = extract_section(
        top_text,
        r"^== Utilization Estimates\s*$",
        r"^== [A-Za-z]",
    )
    utilization_summary = None
    if util_block:
        utilization_summary = extract_section(
            util_block,
            r"^\* Summary:\s*$",
            r"^\s*\+ Detail:\s*$|^== [A-Za-z]",
        )

    return QoRTables(
        top_report=top_report,
        summary_report=summary_path if summary_path.is_file() else None,
        timing_summary=timing_summary,
        latency_summary=latency_summary,
        timing_full=timing_full,
        latency_full=latency_full,
        perf_res=perf_res,
        utilization_summary=utilization_summary,
    )


def aligned_table_text(headers: list[str], rows: list[list[str]]) -> str:
    """Build a plain monospace-aligned table string."""
    if not headers:
        return ""
    width = len(headers)
    norm_rows = [list(r[:width]) + [""] * max(0, width - len(r)) for r in rows]
    cols = list(zip(*([headers] + norm_rows))) if norm_rows else [[h] for h in headers]
    widths = [max(len(str(c)) for c in col) for col in cols]

    def fmt_row(cells: list[str]) -> str:
        return "  ".join(str(cells[i]).ljust(widths[i]) for i in range(width))

    out = [fmt_row(headers), "  ".join("-" * w for w in widths)]
    for row in norm_rows:
        out.append(fmt_row(row))
    return "\n".join(out) + "\n"


def fence(text: str, lang: str = "text") -> str:
    """Wrap text in a markdown fenced code block."""
    body = text.rstrip("\n")
    return f"```{lang}\n{body}\n```\n"


def print_console_table(headers: list[str], rows: list[list[str]]) -> None:
    """Print a simple aligned plain-text table (no markdown chrome)."""
    print(aligned_table_text(headers, rows), end="")

def print_console_qor(qor: QoRTables, console_print: bool) -> None:
    print(colour(f"Top report     : {qor.top_report}", DIM))
    if qor.summary_report is not None:
        print(colour(f"Summary report : {qor.summary_report}", DIM))

    section("Estimated Quality of Results")
    metrics = qor.headline_metrics()

    print(colour("--- Timing Summary ---", BOLD, MAGENTA))
    if all(k in metrics for k in ("clock_target_ns", "clock_estimated_ns", "clock_uncertainty_ns")):
        print_console_table(
            ["Clock", "Target (ns)", "Estimated (ns)", "Uncertainty (ns)"],
            [[
                "ap_clk",
                metrics["clock_target_ns"],
                metrics["clock_estimated_ns"],
                metrics["clock_uncertainty_ns"],
            ]],
        )
    elif qor.timing_summary:
        print(qor.timing_summary)
    else:
        warn("Could not find Timing summary in report.")
    print()

    print(colour("--- Latency Summary (cycles) ---", BOLD, MAGENTA))
    if all(
        k in metrics
        for k in (
            "latency_min_cycles",
            "latency_max_cycles",
            "latency_min_time",
            "latency_max_time",
            "interval_min",
            "interval_max",
        )
    ):
        print_console_table(
            [
                "Lat min",
                "Lat max",
                "Abs min",
                "Abs max",
                "II min",
                "II max",
            ],
            [[
                metrics["latency_min_cycles"],
                metrics["latency_max_cycles"],
                metrics["latency_min_time"],
                metrics["latency_max_time"],
                metrics["interval_min"],
                metrics["interval_max"],
            ]],
        )
    elif qor.latency_summary:
        print(qor.latency_summary)
    else:
        warn("Could not find Latency summary in report.")
    print()

    print(colour("--- Resource Usage Summary (top-level) ---", BOLD, MAGENTA))
    util = qor.utilization_summary or ""
    total = re.search(
        r"\|\s*Total\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
        util,
    )
    available = re.search(
        r"\|\s*Available\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
        util,
    )
    pct = re.search(
        r"\|\s*Utilization\s*\(%\)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
        util,
    )
    if total and available and pct:
        rows = []
        for i, name in enumerate(("BRAM", "DSP", "FF", "LUT", "URAM"), start=1):
            rows.append([
                name,
                total.group(i).strip(),
                available.group(i).strip(),
                pct.group(i).strip(),
            ])
        print_console_table(
            ["Resource", "Total", "Available", "Utilization (%)"],
            rows,
        )
    else:
        print(qor.resource_usage_text(), end="")

    lut_pct = metrics.get("lut_pct")
    if lut_pct:
        try:
            if float(lut_pct) > 100:
                warn(f"LUT utilization is {lut_pct}% — design exceeds device capacity.")
        except ValueError:
            pass
    print()

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
    out_path = RESULTS_DIR / f"results-{int(time.time())}.md"
    out_path.write_text(qor.as_markdown(), encoding="utf-8")
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
    print(f"  --jobs         : {opts.jobs}")

    env_bin = str(vitis_run.parent)
    path = os.environ.get("PATH", "")
    if env_bin not in path.split(os.pathsep):
        os.environ["PATH"] = env_bin + os.pathsep + path

    jobs_env = {"XILINX_NUM_THREADS": str(opts.jobs)}
    jobs_args = ["--hls.jobs", str(opts.jobs)]

    # ------------------------------------------------------------------
    # 1) C Simulation
    # ------------------------------------------------------------------
    section("Beginning C Simulation")
    csim = run_streaming(
        [
            str(vitis_run),
            "--mode",
            "hls",
            "--csim",
            "--config",
            str(cfg),
            "--work_dir",
            str(work_dir),
            *jobs_args,
        ],
        cwd=workspace,
        label="C simulation",
        extra_env=jobs_env,
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
    synth = run_streaming(
        [
            str(vpp),
            "-c",
            "--mode",
            "hls",
            "--config",
            str(cfg),
            "--work_dir",
            str(work_dir),
            *jobs_args,
        ],
        cwd=workspace,
        label="C synthesis",
        extra_env=jobs_env,
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
