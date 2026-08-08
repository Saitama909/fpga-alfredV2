# C synthesis step + utilisation / latency summary.


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


# _find_report
# Find the latest csynth report file.
def _find_report(work_dir):
    report_dir = work_dir / "hls" / "syn" / "report"
    if not report_dir.is_dir():
        raise FileNotFoundError(f"No report dir: {report_dir}")
    tops = [
        p
        for p in report_dir.glob("*_csynth.rpt")
        if "Pipeline" not in p.name and p.name != "csynth.rpt"
    ]
    if tops:
        return max(tops, key=lambda p: p.stat().st_mtime)
    summary = report_dir / "csynth.rpt"
    if summary.is_file():
        return summary
    raise FileNotFoundError(f"No csynth report under {report_dir}")


# _section
# Find the section of text between start_re and stop_re.
def _section(text, start_re, stop_re):
    m = re.search(start_re, text, re.M | re.I)
    if not m:
        return ""
    n = re.search(stop_re, text[m.end() :], re.M | re.I)
    end = m.end() + n.start() if n else len(text)
    return text[m.start() : end]


# _parse_module_intervals
# Pull per-instance Interval (max) from Latency → Detail → Instance.
# Also picks up Loop rows when that section is not N/A.
def _parse_module_intervals(text):
    out = []
    seen = set()

    def _add(label, ii_max):
        if not label:
            return
        if label in seen:
            n = 2
            while f"{label}_{n}" in seen:
                n += 1
            label = f"{label}_{n}"
        seen.add(label)
        out.append((label, str(ii_max)))

    detail = _section(text, r"^\+ Latency:\s*$", r"^={3,}\s*$")
    if not detail:
        detail = text

    inst_blk = _section(detail, r"^\s*\* Instance:\s*$", r"^\s*\* Loop:\s*$")
    row_re = re.compile(
        r"\|\s*([A-Za-z0-9_]+)\s*"
        r"\|\s*([A-Za-z0-9_]+)\s*"
        r"\|\s*(\d+)\s*"
        r"\|\s*(\d+)\s*"
        r"\|\s*[^|]+\|"
        r"\s*[^|]+\|"
        r"\s*(\d+)\s*"
        r"\|\s*(\d+)\s*\|"
    )
    for line in inst_blk.splitlines():
        m = row_re.search(line)
        if not m:
            continue
        inst, _mod, _lo, _hi, _ii_lo, ii_hi = m.groups()
        if inst.lower() == "instance":
            continue
        label = inst[:-3] if inst.endswith("_U0") else inst
        if label == "entry_proc" and ii_hi == "0":
            continue
        _add(label, ii_hi)

    loop_blk = _section(detail, r"^\s*\* Loop:\s*$", r"^={3,}\s*$")
    if loop_blk and not re.search(r"^\s*N/A\s*$", loop_blk, re.M):
        for line in loop_blk.splitlines():
            if "N/A" in line:
                continue
            name_m = re.match(r"\|\s*([A-Za-z0-9_:-]+)\s*\|", line)
            nums = re.findall(r"\|\s*(\d+)\s*", line)
            if not name_m or len(nums) < 2:
                continue
            name = name_m.group(1)
            if name.lower() in ("loop", "name"):
                continue
            _add(f"loop:{name}", nums[-1])

    return out


# _parse
# Parse the csynth report text and return a dictionary of the results.
# ..TODO: Time permitting, make this and other file files pull the report out of xls instead of this.
#        Currently relying on LLMs to strip/parse the log file and pull out the useful information.
def _parse(text):
    out = {}
    timing = _section(text, r"^\+ Timing:\s*$", r"^\+ Latency:\s*$")
    clk = re.search(
        r"\|\s*ap_clk\s*\|\s*([0-9.]+)\s*ns\s*\|\s*([0-9.]+)\s*ns\s*\|\s*([0-9.]+)\s*ns\s*\|",
        timing,
    )
    if clk:
        out["clock_target"] = clk.group(1)
        out["clock_est"] = clk.group(2)
        out["clock_unc"] = clk.group(3)

    lat = _section(text, r"^\+ Latency:\s*$", r"^\s*\+ Detail:\s*$|^={3,}\s*$")
    row = re.search(
        r"\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*([0-9.]+\s*\w+)\s*\|\s*([0-9.]+\s*\w+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|",
        lat,
    )
    if row:
        out["lat_min"] = row.group(1)
        out["lat_max"] = row.group(2)
        out["abs_min"] = row.group(3)
        out["abs_max"] = row.group(4)
        out["ii_min"] = row.group(5)
        out["ii_max"] = row.group(6)

    out["module_ii"] = _parse_module_intervals(text)

    util_block = _section(text, r"^== Utilization Estimates\s*$", r"^== [A-Za-z]")
    util = (
        _section(util_block, r"^\* Summary:\s*$", r"^\s*\+ Detail:\s*$|^== [A-Za-z]")
        or util_block
    )
    total = re.search(
        r"\|\s*Total\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
        util,
    )
    avail = re.search(
        r"\|\s*Available\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
        util,
    )
    pct = re.search(
        r"\|\s*Utilization\s*\(%\)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|",
        util,
    )
    if total and avail and pct:
        for i, name in enumerate(("BRAM", "DSP", "FF", "LUT", "URAM"), start=1):
            out[name] = (
                total.group(i).strip(),
                avail.group(i).strip(),
                pct.group(i).strip().lstrip("~"),
            )
        out["lut_pct"] = out["LUT"][2]
    return out


# _lut_note
# Generate a note about LUT utilisation.
def _lut_note(pct, warn, high, over):
    if pct >= 120:
        return f"LUT util {pct:g}% >= 120% (won't fit)"
    if pct >= over:
        return f"LUT util {pct:g}% >= {over}% (over capacity)"
    if pct >= high:
        return f"LUT util {pct:g}% >= {high}%"
    if pct >= warn:
        return f"LUT util {pct:g}% >= {warn}%"
    return f"LUT util {pct:g}% < {warn}%"


# _print_summary
# Print the csynth summary.
def _print_summary(m, warn, high, over):
    print("")
    print("C-synth summary")
    if "clock_target" in m:
        print(f"  timing: {m['clock_target']} ns target, " f"{m['clock_est']} ns est")
    if "lat_max" in m:
        print(f"  latency: {m['lat_min']} .. {m['lat_max']} cycles")
    if "ii_max" in m:
        print(f"  interval: {m['ii_min']} .. {m['ii_max']} cycles")
    modules = m.get("module_ii") or []
    if modules:
        print("  module intervals:")
        width = max(len(name) for name, _ in modules)
        for name, ii in modules:
            print(f"    {name:<{width}}  {ii}")
    for name in ("BRAM", "DSP", "FF", "LUT", "URAM"):
        if name in m:
            t, a, p = m[name]
            print(f"  {name}: {t} / {a} ({p}%)")
    print("")


# run
# Run C-synth. Returns True on success.
def run(ctx, results):
    print_running("csynth")
    log = LOGS / "csynth-log.txt"
    cmd = [
        str(ctx["vpp"]),
        "-c",
        "--mode",
        "hls",
        "--config",
        str(ctx["hls_cfg"]),
        "--work_dir",
        str(ctx["work_dir"]),
    ]
    rc, _out = run_cmd(
        cmd,
        ctx["hls_cfg"].parent,
        log,
        suppress_info=ctx.get("suppress_info", False),
    )
    if rc != 0:
        footer(log, "FAILED")
        print_failed("csynth (bad exit code)")
        return False

    try:
        rpt = _find_report(ctx["work_dir"])
    except FileNotFoundError as e:
        footer(log, "FAILED")
        print_failed(f"csynth ({e})")
        return False

    shutil.copy2(rpt, OUTS / "csynth-report.rpt")
    m = _parse(rpt.read_text(errors="replace"))
    results["lat_min"] = m.get("lat_min")
    results["lat_max"] = m.get("lat_max")
    results["ii_min"] = m.get("ii_min")
    results["ii_max"] = m.get("ii_max")
    results["clock_target"] = m.get("clock_target")
    results["clock_est"] = m.get("clock_est")
    results["abs_min"] = m.get("abs_min")
    results["abs_max"] = m.get("abs_max")
    results["module_ii"] = m.get("module_ii") or []
    for name in ("BRAM", "DSP", "FF", "LUT", "URAM"):
        if name in m:
            results["res"][name] = m[name]

    lines = [f"csynth summary {now()}", ""]
    for k, v in m.items():
        if k == "module_ii":
            for name, ii in v or []:
                lines.append(f"module_ii.{name}={ii}")
        elif k in ("BRAM", "DSP", "FF", "LUT", "URAM") and isinstance(v, (tuple, list)):
            used, avail, pct = v
            lines.append(f"{k}={used}/{avail} ({pct}%)")
        else:
            lines.append(f"{k}={v}")
    if "lut_pct" in m:
        try:
            lines.append(
                _lut_note(
                    float(m["lut_pct"]),
                    ctx["util_warn"],
                    ctx["util_high"],
                    ctx["util_over"],
                )
            )
        except ValueError:
            pass
    (OUTS / "csynth-summary.txt").write_text("\n".join(lines) + "\n")

    _print_summary(m, ctx["util_warn"], ctx["util_high"], ctx["util_over"])
    footer(log, "PASSED")
    print_passed("csynth")
    return True
