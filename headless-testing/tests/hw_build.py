# Hardware build stub (not implemented yet).

from .common import LOGS, BLUE, colour, footer, print_running


# run
# Stub only. Always returns True so it never fails the overall run.
def run(ctx, results):
    print_running("hw build")
    log = LOGS / "hw_build-log.txt"
    log.write_text("not implemented\n")
    footer(log, "SKIPPED")
    print(f"{colour('SKIP', BLUE)} hw build")
    return True
