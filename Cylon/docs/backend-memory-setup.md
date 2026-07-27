# Backend Memory Setup

Cylon's FEMU backend requires a contiguous physical memory region reserved from the host OS via the `memmap` kernel parameter in GRUB.

## Finding the Physical Address Range per NUMA Node

Before choosing `<start>`, identify which physical address ranges belong to each NUMA node.

### Using `lsmem`

`lsmem --output-all` shows each memory block's physical address range and its NUMA node:

```sh
$ lsmem --output-all
RANGE                                  SIZE  STATE REMOVABLE  BLOCK  NODE ZONES
0x0000000000000000-0x000000007fffffff    2G online       yes   0-15      0 DMA32,Normal
0x0000000100000000-0x000000407fffffff  254G online       yes  32-2079    0 Normal
0x0000004080000000-0x000000807fffffff  256G online       yes 2080-4127   1 Normal
```

Each row is a contiguous range. The `NODE` column shows which NUMA node owns it. Pick a range from the target node and choose `<start>` within it.

### Using `/proc/iomem`

`/proc/iomem` shows the raw physical memory map as seen by the kernel:

```sh
$ cat /proc/iomem | grep "System RAM"
00000000-0009ffff : System RAM
00100000-7fffffff : System RAM
100000000-407fffffff : System RAM
408000000-807fffffff : System RAM
```

To correlate these ranges with NUMA nodes, cross-reference with sysfs:

```sh
$ cat /sys/devices/system/node/node0/meminfo | grep MemTotal
Node 0 MemTotal: 268435456 kB   # 256 GiB → covers 0x0000000000000000–0x3fffffffff

$ cat /sys/devices/system/node/node1/meminfo | grep MemTotal
Node 1 MemTotal: 268435456 kB   # 256 GiB → covers 0x4000000000–0x7fffffffff
```

Or use `numactl -H` to see the total memory per node, then match it against the `System RAM` ranges in `/proc/iomem` by size and order.

### Choosing `<start>`

Once you know the physical range for your target node, pick `<start>` near the **end** of that range so the reserved region stays within the node:

```
# Node 1 covers 0x4000000000–0x7fffffffff (256G starting at 256G)
# Reserve 64G from the end: start = 256G + (256G - 64G) = 448G
memmap=64G!448G
```

> **Tip:** Avoid `<start>=0` or low addresses — those overlap with BIOS/PCI regions visible in `/proc/iomem`.

---

## Reserving the Memory

1. Edit `/etc/default/grub` on the **host machine**:
   ```
   GRUB_CMDLINE_LINUX="... memmap=<size>!<start>"
   ```
   For example, to reserve 64G starting at 128G:
   ```
   GRUB_CMDLINE_LINUX="... memmap=64G!128G"
   ```

2. Apply and reboot:
   ```sh
   sudo update-grub
   sudo reboot
   ```

3. Verify the reserved region appears as a `pmem` device:
   ```sh
   ls /dev/pmem*
   ```