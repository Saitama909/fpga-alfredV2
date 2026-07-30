# RTL co-simulation step.

import re
import shutil

from .common import (
    LOGS,
    OUTS,
    footer,
    now,
    print_failed,
    print_passed,
    print_running,
    run_cmd,
)


# _find_cosim_report
# Find the latest cosim report file.
def _find_cosim_report(work_dir):
    report_dir = work_dir / "hls" / "sim" / "report"
    if not report_dir.is_dir():
        return None
    reports = list(report_dir.glob("*_cosim.rpt"))
    if not reports:
        return None
    return max(reports, key=lambda p: p.stat().st_mtime)


# _passed
# Check if the cosim passed.
def _passed(work_dir, stdout):
    text = stdout
    rpt = _find_cosim_report(work_dir)
    if rpt is not None:
        text += "\n" + rpt.read_text(errors="replace")
    if re.search(r"\bFAIL\b", text) and not re.search(
        r"C/RTL co-simulation finished:\s*PASS", text, re.I
    ):
        # allow PASS lines from the testbench; fail if cosim itself failed
        if re.search(r"co-simulation finished:\s*FAIL|Cosim.*FAIL", text, re.I):
            return False
    if re.search(
        r"C/RTL co-simulation finished:\s*PASS|\|\s*Verilog\s*\|\s*Pass\s*\|",
        text,
        re.I,
    ):
        return True
    if re.search(r"\bFAIL\b", text):
        return False
    return False


# _parse_cosim_report
# Parse the cosim report text and return a dictionary of the results.
# ..TODO: Time permitting, make this and other file files pull the report out of xls instead of this.
#        Currently relying on LLMs to strip/parse the log file and pull out the useful information.
def _parse_cosim_report(rpt_path):
    text = rpt_path.read_text(errors="replace")
    m = re.search(
        r"\|\s*Verilog\s*\|\s*(\w+)\s*"
        r"\|\s*(\d+|NA)\s*"
        r"\|\s*(\d+|NA)\s*"
        r"\|\s*(\d+|NA)\s*"
        r"\|\s*(\d+|NA)\s*"
        r"\|\s*(\d+|NA)\s*"
        r"\|\s*(\d+|NA)\s*"
        r"\|\s*(\d+|NA)\s*\|",
        text,
    )
    if not m:
        return {}
    return {
        "status": m.group(1),
        "lat_min": m.group(2),
        "lat_avg": m.group(3),
        "lat_max": m.group(4),
        "ii_min": m.group(5),
        "ii_avg": m.group(6),
        "ii_max": m.group(7),
        "total_cycles": m.group(8),
    }


# _print_summary
# Print the cosim summary.
def _print_summary(info):
    print("")
    print("Co-sim summary")
    if not info:
        return
    print(
        f"  latency: {info.get('lat_min')} / {info.get('lat_avg')} / {info.get('lat_max')} cycles"
    )
    print(
        f"  interval: {info.get('ii_min')} / {info.get('ii_avg')} / {info.get('ii_max')} cycles"
    )
    if info.get("total_cycles"):
        print(f"  total: {info['total_cycles']} cycles")
    print("")


# run
# Run RTL co-sim. Returns True on success.
def run(ctx, results):
    print_running("cosim")
    log = LOGS / "cosim-log.txt"
    cmd = [
        str(ctx["vitis_run"]),
        "--mode",
        "hls",
        "--cosim",
        "--config",
        str(ctx["hls_cfg"]),
        "--work_dir",
        str(ctx["work_dir"]),
    ]
    rc, out = run_cmd(
        cmd,
        ctx["hls_cfg"].parent,
        log,
        suppress_info=ctx.get("suppress_info", False),
    )

    info = {}
    rpt = _find_cosim_report(ctx["work_dir"])
    if rpt is not None:
        shutil.copy2(rpt, OUTS / "cosim-report.rpt")
        info = _parse_cosim_report(rpt)

    ok = rc == 0 and _passed(ctx["work_dir"], out)
    if info.get("status", "").lower() == "pass":
        ok = rc == 0

    results["cosim"] = ok
    results["cosim_lat"] = info.get("lat_max")  # main number for compare
    results["cosim_lat_min"] = info.get("lat_min")
    results["cosim_lat_avg"] = info.get("lat_avg")
    results["cosim_lat_max"] = info.get("lat_max")
    results["cosim_ii"] = info.get("ii_max")

    summary_lines = [
        f"cosim_pass={ok}",
        f"status={info.get('status', 'unknown')}",
        f"lat_min={info.get('lat_min', 'unknown')}",
        f"lat_avg={info.get('lat_avg', 'unknown')}",
        f"lat_max={info.get('lat_max', 'unknown')}",
        f"ii_max={info.get('ii_max', 'unknown')}",
        f"total_cycles={info.get('total_cycles', 'unknown')}",
        f"time={now()}",
    ]
    (OUTS / "cosim-summary.txt").write_text("\n".join(summary_lines) + "\n")

    _print_summary(info)
    footer(log, "PASSED" if ok else "FAILED")
    if ok:
        print_passed("cosim")
    else:
        print_failed("cosim")
    return ok
