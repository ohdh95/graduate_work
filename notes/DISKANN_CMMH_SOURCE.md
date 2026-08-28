# CMM-H DiskANN source

The CMM-H-based DiskANN search implementation used by the formal EXP-27 run is
tracked as the `DiskANN` submodule.

- Repository: `https://github.com/ohdh95/cmm-d`
- User baseline: `main` at `728988c4fb795f91560e4cd42db962684fc37821`
- Experiment branch: `experiment/exp27-three-layout`
- Experiment source: `e03ecfd7a4a3d00ea57818dfbddd951cf1d3b3cb`
- Source tree: `24348524be10b18a4e42f3f2b09bad682cf7809e`
- EXP-27 search binary SHA-256: `b3028471a1dc4cf7e826bb466737e19628ac218b900d13008fe26bf3bf210271`

The experiment branch preserves the user's DiskANN changes and adds the
runtime-selectable CMM-H backing, NUMA placement, fixed-record layout mapping,
multi-layout cell protocol, raw query statistics, and fail-closed formal-run
checks used by EXP-27.

## Verification

Run the focused host-side checks from the DiskANN repository root:

```bash
CMMH_SEARCH_BINARY=/path/to/search_disk_index \
  scripts/run_cmmh_smoke_tests.sh
```

The source and binary above passed the focused smoke suite and the formal
EXP-27 CLI rejection tests before publication on 2026-08-28.

Clone this workspace branch with the DiskANN source using:

```bash
git clone --recurse-submodules \
  --branch experiment/async-sw-prefetch \
  https://github.com/ohdh95/graduate_work.git
```
