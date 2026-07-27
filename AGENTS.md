# Graduate Work Workspace Instructions

- Use `/home/ohdh95/graduate_work` as the primary workspace for this project.
- Use the single `Cylon` repository for all Cylon source, build, and runtime work.
- Keep upstream commit `4c5e196c09676db18114f4d09509b290c7385978` and the `master` branch as the reproducible baseline; make experiment changes on dedicated branches.
- Put experiment configurations in `configs`, automation in `scripts`, notes in `notes`, and measurements in `results`.
- Preserve raw measurements; write derived tables and plots under `results/processed`.
- Keep the original baseline (`96 GiB NAND`, `5% cache`, `16 GiB guest DRAM`, `FIFO`, `prefetch 0`) available while implementing experimental variants.
- Record source commits, patches, full commands, CPU/NUMA placement, dataset checksums, and random seeds for measured runs.
