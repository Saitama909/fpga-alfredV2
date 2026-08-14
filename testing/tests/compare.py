# Compare C-sim vs RTL co-sim (PASS/FAIL + latency).

from .common import (
    GRAY,
    GREEN,
    LOGS,
    OUTS,
    PASS_GREEN,
    RED,
    colour,
    footer,
    now,
    print_failed,
    print_passed,
    print_running,
)


# _pass_label
# Generate a label for a pass/fail result.
def _pass_label(val, coloured=False):
    if val is True:
        text = "PASSED"
        return colour(text, GREEN) if coloured else text
    if val is False:
        text = "FAILED"
        return colour(text, RED) if coloured else text
    return "not run"


# _as_int
# Convert a value to an integer.
def _as_int(val):
    try:
        return int(val)
    except (TypeError, ValueError):
        return None


# _delta_plain
# Generate a plain text delta between a reference and measured value.
def _delta_plain(ref, measured):
    if ref is None or measured is None:
        return "-"
    diff = measured - ref
    if diff < 0:
        return f"↓ {abs(diff)}"
    if diff > 0:
        return f"↑ {diff}"
    return "-"


# _delta_console
# Generate a console coloured delta between a reference and measured value.
def _delta_console(ref, measured):
    if ref is None or measured is None:
        return colour("-", GRAY)
    diff = measured - ref
    if diff < 0:
        return colour(f"↓ {abs(diff)}", PASS_GREEN)
    if diff > 0:
        return colour(f"↑ {diff}", RED)
    return colour("-", GRAY)


# _latency_table
# Generate a latency table.
def _latency_table(ref, measured, delta_fn):
    ref_s = str(ref) if ref is not None else "-"
    meas_s = str(measured) if measured is not None else "-"
    return [
        "Latency (cycles)",
        f"  {'':<10} {'cycles':>8}   Δ",
        f"  {'csynth':<10} {ref_s:>8}   -",
        f"  {'cosim':<10} {meas_s:>8}   {delta_fn(ref, measured)}",
    ]


# _print_console
# Print the console summary.
def _print_console(results, ref, measured):
    print("Functional")
    print(f"  csim:  {_pass_label(results['csim'], coloured=True)}")
    print(f"  cosim: {_pass_label(results['cosim'], coloured=True)}")
    print("")
    for line in _latency_table(ref, measured, _delta_console):
        print(line)


# run
# Write compare summary. Returns True if both sims passed.
def run(ctx, results):
    print_running("compare")
    log = LOGS / "compare-log.txt"

    ref = _as_int(results.get("lat_max"))
    measured = _as_int(results.get("cosim_lat_max") or results.get("cosim_lat"))

    lines = [
        f"time={now()}",
        "",
        "Functional",
        f"  csim:  {_pass_label(results['csim'])}",
        f"  cosim: {_pass_label(results['cosim'])}",
        "",
    ] + _latency_table(ref, measured, _delta_plain)

    incomplete = results["csim"] is None or results["cosim"] is None
    both_ok = (not incomplete) and bool(results["csim"] and results["cosim"])

    text = "\n".join(lines) + "\n"
    log.write_text(text)
    (OUTS / "compare-summary.txt").write_text(text)
    footer(log, "FAILED" if incomplete or not both_ok else "PASSED")
    print("")
    _print_console(results, ref, measured)
    if incomplete or not both_ok:
        print_failed("compare")
        return False
    print_passed("compare")
    return True
