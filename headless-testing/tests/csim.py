# C simulation step.

import re

from .common import LOGS, footer, print_failed, print_passed, print_running, run_cmd


def _passed(work_dir, stdout):
    text = stdout
    report = work_dir / "hls" / "csim" / "report"
    if report.is_dir():
        for p in report.glob("*_csim.log"):
            text += "\n" + p.read_text(errors="replace")
    if re.search(r"\bFAIL\b", text):
        return False
    if "CSim done with 0 errors" in text:
        return True
    if re.search(r"\bPASS\b", text):
        return True
    return "ERROR" not in text.upper()


# run
# Run C-sim. Returns True on success.
def run(ctx, results):
    print_running("csim")
    log = LOGS / "csim-log.txt"
    cmd = [
        str(ctx["vitis_run"]),
        "--mode",
        "hls",
        "--csim",
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
    ok = rc == 0 and _passed(ctx["work_dir"], out)
    results["csim"] = ok
    footer(log, "PASSED" if ok else "FAILED")
    if ok:
        print_passed("csim")
    else:
        print_failed("csim")
    return ok
