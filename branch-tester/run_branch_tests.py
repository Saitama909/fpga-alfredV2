#!/usr/bin/env python3

# Usage:
#   python3 branch-tester/run_branch_tests.py
#   python3 branch-tester/run_branch_tests.py --force
#   python3 branch-tester/run_branch_tests.py --run-branch NAME
#   python3 branch-tester/run_branch_tests.py --run-branch local
#   python3 branch-tester/run_branch_tests.py --restore
#   python3 branch-tester/run_branch_tests.py --compare
#   python3 branch-tester/run_branch_tests.py --compare --all
#   python3 branch-tester/run_branch_tests.py --delete-local
#   python3 branch-tester/run_branch_tests.py --list-remote

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

##### Config Paths ############################################################
# Can modify these if you want to change paths to different directories.
CONFIG_PATH = HERE / "config.json"
BRANCHES_DIR = HERE / "branches"
BACKUP_DIR = HERE / "local-backup"
LOCAL_SRC = ROOT / "hls" / "src"
TEST_RUNNER = ROOT / "testing" / "run_tests.py"
TEST_CONFIG = ROOT / "testing" / "config.txt"
TEST_LOGS = ROOT / "testing" / "logs"
TEST_OUTS = ROOT / "testing" / "outputs"
COMPARE_OUT = HERE / "compare-summary.txt"
# Special config entry: test WIP from local-backup (not a remote branch).
LOCAL_BRANCH = "local"
###############################################################################

##### Colours stuff for the print/branches at the end #########################
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
BLUE = "\033[34m"
RESET = "\033[0m"
# Distinct colours for file-match SAME groups (different groups ≠ same colour).
SAME_COLOURS = [
    "\033[36m",  # cyan
    "\033[35m",  # magenta
    "\033[34m",  # blue
    "\033[93m",  # bright yellow
    "\033[96m",  # bright cyan
    "\033[95m",  # bright magenta
    "\033[94m",  # bright blue
    "\033[92m",  # bright green
    "\033[91m",  # bright red
    "\033[33m",  # yellow
]
###############################################################################


def colour(text: str, code: str) -> str:
    if not sys.stdout.isatty():
        return text
    return f"{code}{text}{RESET}"


def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def safe_name(branch: str) -> str:
    return branch.replace("/", "_")


def ask_yn(prompt: str, default_no: bool = True) -> bool:
    hint = "[y/N]" if default_no else "[Y/n]"
    try:
        ans = input(f"{prompt} {hint} ").strip().lower()
    except EOFError:
        return False
    if not ans:
        return not default_no
    return ans in ("y", "yes")


def run_git(args: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args],
        cwd=str(ROOT),
        text=True,
        capture_output=True,
        check=check,
    )


def load_config() -> dict:
    if not CONFIG_PATH.is_file():
        raise FileNotFoundError(f"Missing config: {CONFIG_PATH}")
    data = json.loads(CONFIG_PATH.read_text())
    if "test_branches" not in data or not isinstance(data["test_branches"], list):
        raise ValueError("config.json must contain a test_branches array")
    return data


def list_remote_branches() -> list[str]:
    proc = run_git(["for-each-ref", "refs/remotes/origin", "--format=%(refname:short)"])
    names = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line or line == "origin/HEAD" or line.endswith("/HEAD"):
            continue
        if line.startswith("origin/"):
            names.append(line[len("origin/") :])
    return sorted(set(names))


def remote_tip(branch: str) -> tuple[str, str]:
    """Return (full_sha, commit_date_iso) for origin/<branch>."""
    ref = f"origin/{branch}"
    sha = run_git(["rev-parse", ref]).stdout.strip()
    date = run_git(["log", "-1", "--format=%cI", ref]).stdout.strip()
    return sha, date


def read_info(path: Path) -> dict:
    info = {}
    if not path.is_file():
        return info
    for line in path.read_text().splitlines():
        if ":" in line:
            k, _, v = line.partition(":")
            info[k.strip()] = v.strip()
    return info


def write_info(
    path: Path,
    branch: str,
    sha: str,
    commit_date: str,
    pulled_at: str,
    remote_ref: str | None = None,
) -> None:
    ref = remote_ref if remote_ref is not None else f"origin/{branch}"
    path.write_text(
        "\n".join(
            [
                f"branch: {branch}",
                f"remote_ref: {ref}",
                f"commit: {sha}",
                f"commit_short: {sha[:12]}",
                f"commit_date: {commit_date}",
                f"pulled_at: {pulled_at}",
                "",
            ]
        )
    )


def src_content_hash(src_dir: Path) -> str:
    """Stable hash of all files under an hls/src tree (for local WIP skip detection)."""
    h = hashlib.sha256()
    for rel in sorted(iter_rel_files(src_dir)):
        h.update(rel.encode())
        h.update(b"\0")
        h.update((src_dir / rel).read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def iter_rel_files(root: Path) -> set[str]:
    if not root.is_dir():
        return set()
    out = set()
    for p in root.rglob("*"):
        if p.is_file() and not p.name.startswith("."):
            out.add(str(p.relative_to(root)))
    return out


# Rewrite file contents in place (follows symlinks). Never replaces the path.
def write_through(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def copy_dir_contents(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    if not src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        return
    shutil.copytree(src, dst)


##### Phase A: Fetch and archive remote branches ##############################
def fetch_and_archive(
    wanted: list[str], force_archive: bool = False
) -> tuple[list[str], list[str], list[str], list[str]]:
    # Returns (archived, skipped_unchanged, missing, selected).
    # If force_archive is True, always re-archive even when commit matches.
    print("Fetching origin...")
    run_git(["fetch", "--prune", "origin"])

    remotes = list_remote_branches()
    print("\nRemote branches:")
    for name in remotes:
        print(f"  {name}")
    print("")

    remote_set = set(remotes)
    selected = []
    missing = []
    for name in wanted:
        if name in remote_set:
            selected.append(name)
        else:
            missing.append(name)
            print(colour(f"WARNING  branch not on remote: {name} (skipping)", YELLOW))

    BRANCHES_DIR.mkdir(parents=True, exist_ok=True)
    archived = []
    skipped = []

    for name in selected:
        safe = safe_name(name)
        branch_dir = BRANCHES_DIR / safe
        src_dir = branch_dir / "hls" / "src"
        info_path = branch_dir / "info.txt"
        sha, cdate = remote_tip(name)
        prev = read_info(info_path)

        if (
            not force_archive
            and prev.get("commit") == sha
            and src_dir.is_dir()
            and any(src_dir.iterdir())
        ):
            print(
                f"  {name}: remote matches local snapshot "
                f"({sha[:12]}) — skipping archive"
            )
            skipped.append(name)
            continue

        print(f"  {name}: archiving hls/src @ {sha[:12]} ...")
        if src_dir.exists():
            shutil.rmtree(src_dir)
        src_dir.mkdir(parents=True, exist_ok=True)

        archive = subprocess.Popen(
            ["git", "archive", "--format=tar", f"origin/{name}:hls/src"],
            cwd=str(ROOT),
            stdout=subprocess.PIPE,
        )
        extract = subprocess.run(
            ["tar", "-x", "-C", str(src_dir)],
            stdin=archive.stdout,
            cwd=str(ROOT),
            capture_output=True,
            text=True,
        )
        if archive.stdout:
            archive.stdout.close()
        archive.wait()
        if archive.returncode != 0 or extract.returncode != 0:
            err = extract.stderr or "git archive / tar failed"
            raise RuntimeError(f"Failed to archive origin/{name}:hls/src: {err}")

        write_info(info_path, name, sha, cdate, now_iso())
        archived.append(name)

    print("\nArchive summary")
    print(f"  selected:  {', '.join(selected) or '(none)'}")
    print(f"  archived:  {', '.join(archived) or '(none)'}")
    print(f"  unchanged: {', '.join(skipped) or '(none)'}")
    print(f"  missing:   {', '.join(missing) or '(none)'}")
    return archived, skipped, missing, selected


##### Phase B: Backup and restore local hls/src ###############################
def backup_local_src() -> None:
    if BACKUP_DIR.exists():
        shutil.rmtree(BACKUP_DIR)
    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    if not LOCAL_SRC.is_dir():
        raise FileNotFoundError(f"Local src missing: {LOCAL_SRC}")
    for rel in sorted(iter_rel_files(LOCAL_SRC)):
        src = LOCAL_SRC / rel
        dst = BACKUP_DIR / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        # Read through symlink; write a normal backup file
        dst.write_text(
            src.read_text(encoding="utf-8", errors="replace"), encoding="utf-8"
        )
    print(f"Backed up local hls/src → {BACKUP_DIR}")


def restore_local_src() -> int:
    if not BACKUP_DIR.is_dir():
        print(colour(f"ERROR  no backup at {BACKUP_DIR}", RED), file=sys.stderr)
        return 1
    n = 0
    for rel in sorted(iter_rel_files(BACKUP_DIR)):
        local = LOCAL_SRC / rel
        if not local.exists() and not local.is_symlink():
            print(
                colour(
                    f"WARNING  skipping restore for missing local path: {rel}", YELLOW
                )
            )
            continue
        write_through(
            local, (BACKUP_DIR / rel).read_text(encoding="utf-8", errors="replace")
        )
        n += 1
        print(f"  restored {rel}")
    print(f"Restored {n} file(s) from local-backup")
    return 0


# Snapshot local-backup into branches/local/hls/src.
# Returns True if a usable snapshot exists (archived or reused).
def prepare_local_snapshot(force_archive: bool = False) -> bool:
    if not BACKUP_DIR.is_dir() or not any(BACKUP_DIR.rglob("*")):
        print(colour("WARNING  local-backup empty; cannot test 'local'", YELLOW))
        return False

    branch_dir = BRANCHES_DIR / LOCAL_BRANCH
    src_dir = branch_dir / "hls" / "src"
    info_path = branch_dir / "info.txt"
    sha = src_content_hash(BACKUP_DIR)
    prev = read_info(info_path)

    if (
        not force_archive
        and prev.get("commit") == sha
        and src_dir.is_dir()
        and any(src_dir.iterdir())
    ):
        print(
            f"  {LOCAL_BRANCH}: WIP matches snapshot "
            f"({sha[:12]}) — skipping archive"
        )
        return True

    print(f"  {LOCAL_BRANCH}: snapshotting local-backup @ {sha[:12]} ...")
    if src_dir.exists():
        shutil.rmtree(src_dir)
    src_dir.mkdir(parents=True, exist_ok=True)
    for rel in sorted(iter_rel_files(BACKUP_DIR)):
        dst = src_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(
            (BACKUP_DIR / rel).read_text(encoding="utf-8", errors="replace"),
            encoding="utf-8",
        )
    write_info(
        info_path,
        LOCAL_BRANCH,
        sha,
        now_iso(),
        now_iso(),
        remote_ref="local-backup",
    )
    return True


def local_matches_snapshot() -> bool:
    info = read_info(BRANCHES_DIR / LOCAL_BRANCH / "info.txt")
    sha = info.get("commit")
    if not sha or not BACKUP_DIR.is_dir():
        return False
    return sha == src_content_hash(BACKUP_DIR)


##### Phase C: Apply branch sources to local hls/src ##########################
# Swap matched file contents into local hls/src. Returns False if user declines after unmatched warning.


def apply_branch_sources(branch: str) -> bool:
    safe = safe_name(branch)
    branch_src = BRANCHES_DIR / safe / "hls" / "src"
    local_files = iter_rel_files(LOCAL_SRC)
    branch_files = iter_rel_files(branch_src)
    matched = sorted(local_files & branch_files)
    unmatched = sorted(local_files.symmetric_difference(branch_files))

    print(f"\n--- branch: {branch} ---")
    print(f"  matched: {len(matched)}  unmatched: {len(unmatched)}")
    if unmatched:
        print(colour("WARNING  unmatched files:", YELLOW))
        for rel in unmatched:
            side = "local-only" if rel in local_files else "branch-only"
            print(f"    {rel} ({side})")
        if not ask_yn(
            "Continue with matched files only for this branch?", default_no=True
        ):
            print(f"  skipping {branch}")
            return False

    if not matched:
        print(colour(f"WARNING  no matched files for {branch}; skipping", YELLOW))
        return False

    for rel in matched:
        text = (branch_src / rel).read_text(encoding="utf-8", errors="replace")
        write_through(LOCAL_SRC / rel, text)
        print(f"  wrote contents → hls/src/{rel}")
    return True


def _parse_testing_config() -> dict[str, str]:
    vals: dict[str, str] = {}
    if not TEST_CONFIG.is_file():
        return vals
    for line in TEST_CONFIG.read_text(errors="replace").splitlines():
        s = line.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        vals[k.strip()] = v.strip()
    return vals


# Resolve Vitis component work_dir (e.g. .../ntt_core/fqmul) from testing/config.txt.
def resolve_vitis_work_dir() -> Path | None:
    vals = _parse_testing_config()
    ws = vals.get("WORKSPACE_PATH")
    comp = vals.get("COMPONENT_NAME")
    if not ws or not comp:
        return None
    comp_dir = Path(ws) / comp
    if not comp_dir.is_dir():
        return None
    work_name = "hls"
    meta = comp_dir / "vitis-comp.json"
    if meta.is_file():
        try:
            data = json.loads(meta.read_text())
            work_name = (
                data.get("configuration", {}).get("work_dir")
                or data.get("work_dir")
                or work_name
            )
        except (OSError, json.JSONDecodeError):
            pass
    return comp_dir / work_name


# Wipe shared Vitis HLS sim/syn/csim so branch swaps cannot reuse stale RTL.
def clean_vitis_hls_cache() -> None:
    work = resolve_vitis_work_dir()
    if work is None or not work.is_dir():
        print(
            colour(
                "WARNING  could not resolve Vitis work_dir; skip cache clean", YELLOW
            )
        )
        return
    hls = work / "hls"
    cleaned = []
    for sub in ("sim", "syn", "csim"):
        target = hls / sub
        if target.exists():
            shutil.rmtree(target)
            cleaned.append(sub)
    if cleaned:
        print(f"  cleaned Vitis cache: {work}/hls/{{{','.join(cleaned)}}}")
    else:
        print(f"  Vitis cache already clean under {work}/hls/")


def run_tests_for_branch(branch: str) -> int:
    print(f"\nRunning testing/run_tests.py for {branch} ...")
    clean_vitis_hls_cache()
    proc = subprocess.run(
        [sys.executable, str(TEST_RUNNER)],
        cwd=str(ROOT),
    )
    return proc.returncode


def collect_artifacts(branch: str) -> None:
    safe = safe_name(branch)
    dest = BRANCHES_DIR / safe
    copy_dir_contents(TEST_LOGS, dest / "logs")
    copy_dir_contents(TEST_OUTS, dest / "outputs")
    print(f"  copied logs/outputs → {dest}")


def has_test_results(branch: str) -> bool:
    return (BRANCHES_DIR / safe_name(branch) / "outputs" / "run-summary.txt").is_file()


def remote_matches_snapshot(branch: str) -> bool:
    if branch == LOCAL_BRANCH:
        return local_matches_snapshot()
    info = read_info(BRANCHES_DIR / safe_name(branch) / "info.txt")
    sha = info.get("commit")
    if not sha:
        return False
    tip, _ = remote_tip(branch)
    return tip == sha


def full_run(wanted: list[str], force: bool = False) -> int:
    remotes = [b for b in wanted if b != LOCAL_BRANCH]
    want_local = LOCAL_BRANCH in wanted

    _, _, _, selected = fetch_and_archive(remotes)
    if not selected and not want_local:
        print("No branches to test.")
        return 1

    backup_local_src()
    failed = False
    try:
        if want_local:
            if not prepare_local_snapshot(force_archive=force):
                print(colour("WARNING  skipping local WIP tests", YELLOW))
                want_local = False

        # Preserve config order; only remotes that were selected (+ local).
        selected_set = set(selected)
        to_test = [
            b for b in wanted if (b == LOCAL_BRANCH and want_local) or b in selected_set
        ]

        for branch in to_test:
            if (
                not force
                and remote_matches_snapshot(branch)
                and has_test_results(branch)
            ):
                reason = (
                    "WIP unchanged and results exist"
                    if branch == LOCAL_BRANCH
                    else "remote unchanged and results exist"
                )
                print(
                    f"\n--- branch: {branch} ---\n"
                    f"  {reason} — skipping tests "
                    f"(use --force to re-run)"
                )
                continue
            if not apply_branch_sources(branch):
                continue
            rc = run_tests_for_branch(branch)
            if rc != 0:
                failed = True
                print(colour(f"WARNING  run_tests exited {rc} for {branch}", YELLOW))
            collect_artifacts(branch)
    finally:
        print("\nRestoring local hls/src from backup...")
        restore_local_src()

    return 1 if failed else 0


##### Compare branch outputs/run-summary.txt tables ############################
def parse_run_summary(path: Path) -> dict:
    text = path.read_text(errors="replace")
    data = {
        "steps": {},
        "timing": {},
        "usage": {},
    }
    section = None
    for line in text.splitlines():
        raw = line.rstrip()
        s = raw.strip()
        if s == "Summary":
            section = "summary"
            continue
        if s == "Timing":
            section = "timing"
            continue
        if s == "Usage":
            section = "usage"
            continue
        if s.startswith("Resource warnings"):
            section = None
            continue
        if not s:
            continue

        if section == "summary":
            m = re.match(r"([A-Za-z][A-Za-z0-9 _-]*):\s*(PASSED|FAILED|SKIPPED)\b", s)
            if m:
                data["steps"][m.group(1).strip()] = m.group(2)
        elif section == "timing":
            if "clock target" in s:
                data["timing"]["clock_target"] = s.split(":", 1)[-1].strip()
            elif "clock est" in s:
                data["timing"]["clock_est"] = s.split(":", 1)[-1].strip()
            elif "csynth lat" in s:
                data["timing"]["csynth_lat"] = s.split(":", 1)[-1].strip()
            elif "csynth interval" in s:
                data["timing"]["csynth_ii"] = s.split(":", 1)[-1].strip()
            elif "cosim lat" in s:
                data["timing"]["cosim_lat"] = s.split(":", 1)[-1].strip()
            elif "cosim interval" in s:
                data["timing"]["cosim_ii"] = s.split(":", 1)[-1].strip()
        elif section == "usage":
            if s.lower().startswith("resource"):
                continue
            parts = s.split()
            if len(parts) >= 4 and parts[0] in ("BRAM", "DSP", "FF", "LUT", "URAM"):
                pct = parts[-1].rstrip("%")
                data["usage"][parts[0]] = {
                    "used": parts[1],
                    "avail": parts[2],
                    "pct": pct,
                }
    return data


def _parse_lat_num(val: str) -> float | None:
    if not val or val == "-":
        return None
    # Prefer the last number so "448 to 448" / "471 to 480" use the max (csynth max).
    nums = re.findall(r"\d+(?:\.\d+)?", val)
    if not nums:
        return None
    return float(nums[-1])


# Prefer lat_max / ii_max from outputs/cosim-summary.txt when present.
def _enrich_cosim_from_summary(branch_dir: Path, data: dict) -> None:
    path = branch_dir / "outputs" / "cosim-summary.txt"
    if not path.is_file():
        return
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("lat_max="):
            val = line.split("=", 1)[1].strip()
            if val and val.lower() != "unknown":
                data["timing"]["cosim_lat"] = val
        elif line.startswith("ii_max="):
            val = line.split("=", 1)[1].strip()
            if val and val.lower() != "unknown":
                data["timing"]["cosim_ii"] = val


# Map relative path → md5 for files under branches/<name>/hls/src.
def _hls_src_file_hashes(branch_dir: Path) -> dict[str, str]:
    src = branch_dir / "hls" / "src"
    out: dict[str, str] = {}
    if not src.is_dir():
        return out
    for p in sorted(src.rglob("*")):
        if p.is_file():
            rel = p.relative_to(src).as_posix()
            out[rel] = hashlib.md5(p.read_bytes()).hexdigest()
    return out


def _colour_extremes(values: dict[str, float | None], cell: str, branch: str) -> str:
    nums = {b: v for b, v in values.items() if v is not None}
    if len(nums) < 2 or branch not in nums:
        return cell
    lo = min(nums.values())
    hi = max(nums.values())
    if lo == hi:
        return cell
    if nums[branch] == lo:
        return colour(cell, GREEN)
    if nums[branch] == hi:
        return colour(cell, RED)
    return cell


def _header_name(name: str, max_len: int = 15) -> str:
    """Truncate branch names for table headers (full name still used as key)."""
    return name if len(name) <= max_len else name[:max_len]


def mode_compare(show_all: bool = False) -> int:
    branch_dirs = (
        sorted(p for p in BRANCHES_DIR.iterdir() if p.is_dir())
        if BRANCHES_DIR.is_dir()
        else []
    )
    rows = []
    name_to_dir: dict[str, Path] = {}
    for d in branch_dirs:
        summary = d / "outputs" / "run-summary.txt"
        if not summary.is_file():
            continue
        info = read_info(d / "info.txt")
        name = info.get("branch", d.name)
        data = parse_run_summary(summary)
        _enrich_cosim_from_summary(d, data)
        rows.append((name, data))
        name_to_dir[name] = d

    if not rows:
        print("No branch outputs/run-summary.txt found under branch-tester/branches/")
        return 1

    if not show_all:
        wanted = load_config()["test_branches"]
        by_name = {n: data for n, data in rows}
        rows = [(n, by_name[n]) for n in wanted if n in by_name]
        missing = [n for n in wanted if n not in by_name]
        if missing:
            print(
                colour(
                    "WARNING  config branches with no local results (skipped): "
                    + ", ".join(missing),
                    YELLOW,
                )
            )
        if not rows:
            print(
                "No config-branch results to compare "
                "(use --compare --all to show every local snapshot)."
            )
            return 1

    names = [n for n, _ in rows]
    headers = [_header_name(n) for n in names]
    # Column width fits data cells ("448 to 448") and truncated headers.
    width = max(15, max(len(h) for h in headers))

    plain_lines = ["Branch compare", f"time={now_iso()}", ""]

    def emit(line: str = "") -> None:
        print(line)
        plain_lines.append(re.sub(r"\033\[[0-9;]*m", "", line))

    ##### Summary table
    emit("Summary (pass/fail)")
    step_keys = []
    for _, data in rows:
        for k in data["steps"]:
            if k not in step_keys:
                step_keys.append(k)
    hdr = f"  {'step':<16}" + "".join(f" {h:>{width}}" for h in headers)
    emit(hdr)
    for step in step_keys:
        cells = []
        for n, data in rows:
            st = data["steps"].get(step, "-")
            if st == "PASSED":
                cells.append(
                    colour(f"{st:>{width}}", GREEN)
                    if sys.stdout.isatty()
                    else f"{st:>{width}}"
                )
            elif st == "FAILED":
                cells.append(
                    colour(f"{st:>{width}}", RED)
                    if sys.stdout.isatty()
                    else f"{st:>{width}}"
                )
            else:
                cells.append(f"{st:>{width}}")
        # plain version without colour for file
        plain_cells = [f"{data['steps'].get(step, '-'):>{width}}" for _, data in rows]
        print(f"  {step:<16}" + "".join(f" {c}" for c in cells))
        plain_lines.append(f"  {step:<16}" + "".join(f" {c}" for c in plain_cells))
    emit("")

    ##### Timing table
    emit("Timing")
    timing_keys = [
        ("clock_target", "clock target"),
        ("clock_est", "clock est"),
        ("csynth_lat", "csynth lat"),
        ("csynth_ii", "csynth ii"),
        ("cosim_lat", "cosim lat"),
        ("cosim_ii", "cosim ii"),
    ]
    emit(f"  {'metric':<16}" + "".join(f" {h:>{width}}" for h in headers))
    for key, label in timing_keys:
        raw = {n: data["timing"].get(key, "-") for n, data in rows}
        nums = (
            {n: _parse_lat_num(v) for n, v in raw.items()}
            if key.endswith("lat") or key.endswith("ii")
            else {}
        )
        coloured = []
        plain_cells = []
        for n, _ in rows:
            cell = f"{raw[n]:>{width}}"
            plain_cells.append(cell)
            if (key.endswith("lat") or key.endswith("ii")) and len(rows) > 1:
                coloured.append(_colour_extremes(nums, cell, n))
            else:
                coloured.append(cell)
        print(f"  {label:<16}" + "".join(f" {c}" for c in coloured))
        plain_lines.append(f"  {label:<16}" + "".join(f" {c}" for c in plain_cells))

    # Δ = cosim − csynth (highlights when measured RTL exceeds HLS estimate)
    delta_raw = {}
    delta_nums = {}
    for n, data in rows:
        csyn = _parse_lat_num(data["timing"].get("csynth_lat", "-"))
        cosi = _parse_lat_num(data["timing"].get("cosim_lat", "-"))
        if csyn is None or cosi is None:
            delta_raw[n] = "-"
            delta_nums[n] = None
        else:
            d = int(cosi - csyn)
            delta_raw[n] = f"{d:+d}"
            delta_nums[n] = float(d)
    coloured = []
    plain_cells = []
    for n, _ in rows:
        cell = f"{delta_raw[n]:>{width}}"
        plain_cells.append(cell)
        if len(rows) > 1 and delta_nums.get(n) is not None:
            # lower (more negative / smaller) is better
            coloured.append(_colour_extremes(delta_nums, cell, n))
        else:
            coloured.append(cell)
    print(f"  {'Δ cosim-csynth':<16}" + "".join(f" {c}" for c in coloured))
    plain_lines.append(
        f"  {'Δ cosim-csynth':<16}" + "".join(f" {c}" for c in plain_cells)
    )
    emit("")

    ##### Usage table
    emit("Usage (% util)")
    resources = ["BRAM", "DSP", "FF", "LUT", "URAM"]
    emit(f"  {'resource':<16}" + "".join(f" {h:>{width}}" for h in headers))
    for res in resources:
        raw = {}
        nums = {}
        for n, data in rows:
            u = data["usage"].get(res)
            if not u:
                raw[n] = "-"
                nums[n] = None
            else:
                raw[n] = f"{u['pct']}%"
                try:
                    nums[n] = float(u["pct"])
                except ValueError:
                    nums[n] = None
        coloured = []
        plain_cells = []
        for n, _ in rows:
            cell = f"{raw[n]:>{width}}"
            plain_cells.append(cell)
            if len(rows) > 1:
                coloured.append(_colour_extremes(nums, cell, n))
            else:
                coloured.append(cell)
        print(f"  {res:<16}" + "".join(f" {c}" for c in coloured))
        plain_lines.append(f"  {res:<16}" + "".join(f" {c}" for c in plain_cells))
    emit("")

    ##### File matches (content hash; same hash are the same colour)
    emit("File matches")
    file_maps: dict[str, dict[str, str]] = {}
    all_files: list[str] = []
    for n, _ in rows:
        d = name_to_dir[n]
        fmap = _hls_src_file_hashes(d)
        file_maps[n] = fmap
        for rel in fmap:
            if rel not in all_files:
                all_files.append(rel)
    all_files.sort()

    # Short ids used in the table; colour keyed by id so identical numbers match.
    def short_id(h: str) -> str:
        return h[:8]

    id_counts: dict[str, int] = {}
    for rel in all_files:
        for n, _ in rows:
            h = file_maps[n].get(rel)
            if h is not None:
                id_counts[short_id(h)] = id_counts.get(short_id(h), 0) + 1

    colour_for_id: dict[str, str] = {}
    colour_i = 0
    for sid, count in sorted(id_counts.items()):
        if count >= 2:
            colour_for_id[sid] = SAME_COLOURS[colour_i % len(SAME_COLOURS)]
            colour_i += 1

    label_w = max(16, max((len(f) for f in all_files), default=16))
    emit(f"  {'file':<{label_w}}" + "".join(f" {h:>{width}}" for h in headers))
    for rel in all_files:
        coloured = []
        plain_cells = []
        for n, _ in rows:
            h = file_maps[n].get(rel)
            if h is None:
                cell = f"{'-':>{width}}"
                coloured.append(cell)
                plain_cells.append(cell)
            else:
                sid = short_id(h)
                cell = f"{sid:>{width}}"
                plain_cells.append(cell)
                if sid in colour_for_id:
                    coloured.append(colour(cell, colour_for_id[sid]))
                else:
                    coloured.append(cell)
        print(f"  {rel:<{label_w}}" + "".join(f" {c}" for c in coloured))
        plain_lines.append(
            f"  {rel:<{label_w}}" + "".join(f" {c}" for c in plain_cells)
        )

    COMPARE_OUT.write_text("\n".join(plain_lines) + "\n")
    print(f"\nWrote {COMPARE_OUT}")
    return 0


def mode_delete_local() -> int:
    if not BRANCHES_DIR.exists():
        print("Nothing to delete (branches/ missing).")
        return 0
    if not ask_yn(f"Delete {BRANCHES_DIR}/ ?", default_no=True):
        print("Aborted.")
        return 0
    shutil.rmtree(BRANCHES_DIR)
    BRANCHES_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Deleted contents of {BRANCHES_DIR}")
    return 0


def mode_list_remote() -> int:
    print("Fetching origin...")
    run_git(["fetch", "--prune", "origin"])
    remotes = list_remote_branches()
    print("\nRemote branches:")
    if not remotes:
        print("  (none)")
        return 0
    for name in remotes:
        print(f"  {name}")
    print(f"\n{len(remotes)} branch(es)")
    return 0


# Force re-archive + re-test a single remote branch (or local WIP).
def mode_run_branch(branch: str) -> int:
    if branch == LOCAL_BRANCH:
        print(f"Single-branch run: {LOCAL_BRANCH} (snapshot local-backup + tests)")
        BRANCHES_DIR.mkdir(parents=True, exist_ok=True)
        backup_local_src()
        failed = False
        try:
            if not prepare_local_snapshot(force_archive=True):
                return 1
            if not apply_branch_sources(LOCAL_BRANCH):
                return 1
            rc = run_tests_for_branch(LOCAL_BRANCH)
            if rc != 0:
                failed = True
                print(
                    colour(f"WARNING  run_tests exited {rc} for {LOCAL_BRANCH}", YELLOW)
                )
            collect_artifacts(LOCAL_BRANCH)
        finally:
            print("\nRestoring local hls/src from backup...")
            restore_local_src()
        return 1 if failed else 0

    print(f"Single-branch run: {branch} (force archive + tests)")
    _, _, missing, selected = fetch_and_archive([branch], force_archive=True)
    if missing or not selected:
        print(colour(f"ERROR  branch not on remote: {branch}", RED), file=sys.stderr)
        return 1

    backup_local_src()
    failed = False
    try:
        if not apply_branch_sources(branch):
            return 1
        rc = run_tests_for_branch(branch)
        if rc != 0:
            failed = True
            print(colour(f"WARNING  run_tests exited {rc} for {branch}", YELLOW))
        collect_artifacts(branch)
    finally:
        print("\nRestoring local hls/src from backup...")
        restore_local_src()
    return 1 if failed else 0


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Cross-branch HLS source tester")
    p.add_argument(
        "--force",
        action="store_true",
        help="re-run tests even if remote commit is unchanged and results already exist",
    )
    p.add_argument(
        "--run-branch",
        metavar="NAME",
        help="force re-fetch and re-test a single remote branch "
        "(or 'local' for WIP from local-backup)",
    )
    p.add_argument(
        "--restore", action="store_true", help="restore local hls/src from local-backup"
    )
    p.add_argument(
        "--compare",
        action="store_true",
        help="compare branch outputs/run-summary.txt tables",
    )
    p.add_argument(
        "--all",
        action="store_true",
        help="with --compare, include every local branch snapshot (default: config only)",
    )
    p.add_argument(
        "--delete-local", action="store_true", help="purge branch-tester/branches/"
    )
    p.add_argument(
        "--list-remote", action="store_true", help="fetch and list remote branches"
    )
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    exclusive = [
        bool(args.run_branch),
        args.restore,
        args.compare,
        args.delete_local,
        args.list_remote,
    ]
    if sum(1 for f in exclusive if f) > 1:
        print(
            "ERROR: --run-branch, --restore, --compare, --delete-local, and "
            "--list-remote are exclusive",
            file=sys.stderr,
        )
        return 2
    if args.force and any(exclusive):
        print(
            "ERROR: --force only applies to a full config run "
            "(not with --run-branch/--restore/--compare/--delete-local/--list-remote)",
            file=sys.stderr,
        )
        return 2
    if args.all and not args.compare:
        print("ERROR: --all only applies with --compare", file=sys.stderr)
        return 2

    if args.run_branch:
        return mode_run_branch(args.run_branch)
    if args.restore:
        return restore_local_src()
    if args.compare:
        return mode_compare(show_all=args.all)
    if args.delete_local:
        return mode_delete_local()
    if args.list_remote:
        return mode_list_remote()

    cfg = load_config()
    return full_run(cfg["test_branches"], force=args.force)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        print(f"git command failed: {e}", file=sys.stderr)
        if e.stderr:
            print(e.stderr, file=sys.stderr)
        sys.exit(1)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
