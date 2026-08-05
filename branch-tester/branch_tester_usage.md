# Branch-tester flags

Run from the repo root:

```bash
python3 branch-tester/run_branch_tests.py [flags]
```

With **no flags**, the tool runs the full flow for every branch in.

---

## Full-run modifiers

### `--force`

Re-run tests for every config branch even when:

- the remote commit still matches the local snapshot in `info.txt`, **and**
- `branches/<name>/outputs/run-summary.txt` already exists

Without `--force`, those branches are skipped

```bash
python3 branch-tester/run_branch_tests.py --force
```

#### Note

Currently this only works on a **full config run** and not with the exclusive flags below. But can make it work in future if people want that.

---

## Exclusive modes

### `--run-branch NAME`

Force **re-fetch + re-archive + re-test** a single remote branch (ignores `test_branches` in the config, except you still need a valid remote name).

Always re-archives `hls/src` and always runs `testing/run_tests.py` for that branch, then restores local `hls/src` from `local-backup`.

```bash
python3 branch-tester/run_branch_tests.py --run-branch jovan-mvp
```

### `--list-remote`

`git fetch --prune origin`, then print all remote branch short names.

```bash
python3 branch-tester/run_branch_tests.py --list-remote
```

### `--compare`

See the output/comparing of multiple branches at once.

**File matches**: rows are files under each branch’s `hls/src`. Cells show an 8-char content hash (`-` if missing). Identical hashes share a colour and unique hashes stay uncoloured. Basically just trying to show which files are the same between branches.

With 2+ branches, best (lowest latency / util) is green and worst is red. Cosim latency is taken from `cosim-summary.txt` when present.

Also writes a plain copy to `branch-tester/compare-summary.txt`.

By default only **config** `test_branches` appear as columns. Pass `--all` to include every local snapshot under `branch-tester/branches/`.

```bash
python3 branch-tester/run_branch_tests.py --compare
python3 branch-tester/run_branch_tests.py --compare --all
```

Before each branch’s `testing/run_tests.py` run, the tool wipes the shared Vitis work_dir `hls/{sim,syn,csim}` cache so RTL/cosim cannot reuse stale artifacts across branch source swaps.

### `--restore`

Incase something effs up, or you cancel the run while its operating, use the following command to automatically restore your work from before the test ran. 

Rewrite local `hls/src` file **contents** from `branch-tester/local-backup/` symlink-safe; does not replace files/inodes).

```bash
python3 branch-tester/run_branch_tests.py --restore
```

### `--delete-local`

Prompt `Y/N`, then delete `branch-tester/branches/` (recreates an empty folder). Does **not** delete `local-backup/`.

```bash
python3 branch-tester/run_branch_tests.py --delete-local
```

