#!/usr/bin/env python3

#####  run_tests.py ########################################
# The core/root test runner.

# Reads config.txt, then runs whichever steps are set to be enabled. Each step lives in its own file under testing/tests/.

# Usage:
# python3 testing/run_tests.py           # Normal run.
# python3 testing/run_tests.py --purge   # clear logs/outputs only, then exit. Use this if you want to clean the folders manually before testing.
# The program automatically cleans the folders before running the tests.
###########################################################

import argparse
import json
import os
import sys
import time
from pathlib import Path

TEST_VERSION = "0.2 (Added interval stats)"

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from tests import csim, csynth, cosim, compare, hw_build
from tests.common import (
    LOGS,
    OUTS,
    component_paths,
    find_tool,
    print_done,
    print_run_summary,
    print_skip,
    purge_folders,
)

CONFIG = HERE / "config.txt"


# load_config
# Loads the config.txt file and returns a dictionary of the config settings
def load_config(path):
    cfg = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        cfg[k.strip()] = v.strip()
    return cfg


# on
# Checks if a given key is set to true in the config dictionary (basically so can support "yes", "true" etc)
def on(cfg, key, default=False):
    raw = cfg.get(key)
    if raw is None:
        return default
    return raw.lower() in ("1", "true", "yes", "on")


def as_int(cfg, key, default):
    try:
        return int(cfg.get(key, str(default)))
    except ValueError:
        return default


# parse_args
# Parses the command line arguments and returns a namespace object with the arguments
def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Vitis HLS test runner")
    p.add_argument(
        "--purge",
        action="store_true",
        help="delete files in testing/logs and testing/outputs, then exit",
    )
    return p.parse_args(argv)


def print_intro():
    print("===========================================")
    print(
        f"""
    ▗▖  ▗▖▗▄▄▄▖▗▄▄▄▖    ▗▄▄▄▖▗▄▄▄▖ ▗▄▄▖▗▄▄▄▖▗▄▄▄▖▗▄▄▖ 
    ▐▛▚▖▐▌  █    █        █  ▐▌   ▐▌     █  ▐▌   ▐▌ ▐▌
    ▐▌ ▝▜▌  █    █        █  ▐▛▀▀▘ ▝▀▚▖  █  ▐▛▀▀▘▐▛▀▚▖
    ▐▌  ▐▌  █    █        █  ▐▙▄▄▖▗▄▄▞▘  █  ▐▙▄▄▖▐▌ ▐▌
            
    Is your code Oliver Diessel Approved?                       
    vers: {TEST_VERSION}                       
    """
    )
    print("===========================================")


#####  main function
def main(argv=None):
    args = parse_args(argv)
    t0 = time.monotonic()

    LOGS.mkdir(parents=True, exist_ok=True)
    OUTS.mkdir(parents=True, exist_ok=True)

    if args.purge:
        n = purge_folders()
        print(f"purged {n} files")
        print_done(time.monotonic() - t0)
        return 0

    if not CONFIG.is_file():
        print(f"Missing config: {CONFIG}", file=sys.stderr)
        return 1

    cfg = load_config(CONFIG)
    workspace = Path(cfg.get("WORKSPACE_PATH", "")).expanduser().resolve()
    component = cfg.get("COMPONENT_NAME", "ntt_core")
    vitis_bin = cfg.get("VITIS_BIN_DIR", "")

    if not workspace.is_dir():
        print(f"Bad WORKSPACE_PATH: {workspace}", file=sys.stderr)
        return 1

    try:
        vitis_run = find_tool("vitis-run", vitis_bin)
        vpp = find_tool("v++", vitis_bin)
        hls_cfg, work_dir = component_paths(workspace, component)
    except (FileNotFoundError, OSError, json.JSONDecodeError) as e:
        print(e, file=sys.stderr)
        return 1

    # so child tools can find the rest of Vitis
    bin_dir = str(vitis_run.parent)
    path = os.environ.get("PATH", "")
    if bin_dir not in path.split(os.pathsep):
        os.environ["PATH"] = bin_dir + os.pathsep + path

    print_intro()

    if on(cfg, "PURGE_FOLDERS", default=True):
        n = purge_folders()
        print(f"purged {n} files")

    print(f"workspace: {workspace}")
    print(f"component: {component}")
    print(f"work_dir: {work_dir}")

    # Print context inf and results dictionaries
    ctx = {
        "vitis_run": vitis_run,
        "vpp": vpp,
        "hls_cfg": hls_cfg,
        "work_dir": work_dir,
        "util_warn": as_int(cfg, "UTIL_WARN", 70),
        "util_high": as_int(cfg, "UTIL_HIGH", 90),
        "util_over": as_int(cfg, "UTIL_OVER", 100),
        "suppress_info": on(cfg, "SUPPRESS_INFO_MESSAGES"),
    }
    results = {
        "csim": None,
        "cosim": None,
        "lat_min": None,
        "lat_max": None,
        "clock_target": None,
        "clock_est": None,
        "abs_min": None,
        "abs_max": None,
        "cosim_lat": None,
        "cosim_lat_min": None,
        "cosim_lat_avg": None,
        "cosim_lat_max": None,
        "cosim_ii": None,
        "res": {},
    }
    failed = False
    step_status = {}

    # Each of the steps to be run
    # currently the hw build step is just a stub till jovan implements
    # TODO: (time permitting and if can be bothered), make this agnostic so that it just pulls the testing files from the directory and doesnt care what they are.
    steps = [
        ("RUN_CSIM", csim, "csim"),
        ("RUN_CSYNTH", csynth, "csynth"),
        ("RUN_COSIM", cosim, "cosim"),
        ("RUN_COMPARE", compare, "compare"),
        ("RUN_HW_BUILD", hw_build, "hw build"),
    ]

    for flag, mod, label in steps:
        if on(cfg, flag):
            ok = mod.run(ctx, results)
            if flag == "RUN_HW_BUILD":
                # stub always "runs" but is not a real pass/fail yet
                step_status[label] = "SKIPPED"
            else:
                step_status[label] = "PASSED" if ok else "FAILED"
                if not ok:
                    failed = True
        else:
            print_skip(label)
            step_status[label] = "SKIPPED"

    print_run_summary(
        step_status,
        results,
        ctx["util_warn"],
        ctx["util_high"],
        ctx["util_over"],
    )
    print_done(time.monotonic() - t0)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
