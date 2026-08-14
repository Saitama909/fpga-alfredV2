# Branch tester

## What is this?

It pulls `hls/src` from remote branches for the repo (plus optional local), runs the usual test harness on each one, then puts your tree back. Was useful for comparing pass/fail, timing, and utilisation side by side against everyones progress all at once.

## WIP

This file was always a work in progress and was designed moreso to quickly compare performance and utilisation between branches. It's far from finished but it was super helpful and I would have liked to had more time to develop a more extensive and less cluttered single-file testbench. -Riley.

## How to run?.

From the repo root:

```bash
python3 branch-tester/run_branch_tests.py [flags]
```

No flags = full run for whatever's listed in `branch-tester/config.json` → `test_branches`.

`"local"` isn't a remote. It snapshots your current `hls/src` (via `local-backup`) into `branches/local/`, runs the harness on it, and shows up as a column in `--compare`. Leave it out of the config if you don't want WIP tested. Or just force it:

```bash
python3 branch-tester/run_branch_tests.py --run-branch local
```

---

## `--force`

Re-runs every config branch even when:

- the remote commit still matches the local snapshot in `info.txt`, **and**
- `branches/<name>/outputs/run-summary.txt` is already there

Without `--force` those get skipped.

```bash
python3 branch-tester/run_branch_tests.py --force
```

Only works on a full config run at the moment, not with the exclusive flags below. Easy enough to add later if anyone wants it.

---

## Exclusive modes

These don't mix with each other (or with `--force`).

### `--run-branch NAME`

Re-fetch, re-archive, and re-test one remote branch. Ignores `test_branches` in the config, you still need a real remote name.

Always archives `hls/src` and always runs `testing/run_tests.py` for that branch, then puts local `hls/src` back from `local-backup`.

```bash
python3 branch-tester/run_branch_tests.py --run-branch jovan-mvp
```

### `--list-remote`

`git fetch --prune origin`, then print the remote branch short names.

```bash
python3 branch-tester/run_branch_tests.py --list-remote
```

### `--compare`

Side-by-side tables of the branch results.

**File matches:** one row per file under each branch's `hls/src`. Cells are an 8-char content hash (`-` if it's missing). Same hash = same colour, unique ones stay plain. Just so you can see which files match across branches.

With 2+ branches, best (lowest latency / interval / util) is green and worst is red. Cosim latency and interval come from `cosim-summary.txt` when it's there.

Also dumps a plain copy to `branch-tester/compare-summary.txt`.

Default columns are just the config `test_branches`. Pass `--all` to include every snapshot under `branch-tester/branches/`.

```bash
python3 branch-tester/run_branch_tests.py --compare
python3 branch-tester/run_branch_tests.py --compare --all
```

Before each `testing/run_tests.py` run we chuck the shared Vitis work_dir `hls/{sim,syn,csim}` cache so RTL/cosim doesn't reuse stale artefacts from the last branch.

### `--restore`

In case something stuffs up, or you cancel mid-run, this puts your `hls/src` back from `branch-tester/local-backup/`. Writes through the existing paths (symlink-safe atm so it doesn't replace files/inodes making Vitis sometimes very angry).

```bash
python3 branch-tester/run_branch_tests.py --restore
```

### `--delete-local`

Asks first, then deletes `branch-tester/branches/` and recreates an empty folder. Leaves `local-backup/` alone.

```bash
python3 branch-tester/run_branch_tests.py --delete-local
```
