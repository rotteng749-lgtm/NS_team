# NS_team

NS_team is a KPatch-Next based kernel patch module (KPM) for Android. When
loaded on a rooted device it exposes a single character device, `/dev/ns_team`,
with a stable ioctl interface for cross-process memory access, module base
lookup, process lookup and touch-event injection.

> Rebranded and maintained by **NS_team**. Derived from the [KPatch-Next](https://github.com/bmax121/KPatch-Next)
> framework and the original `wanbai-driver` by ALEX5402. Distributed under
> GPL-2.0-or-later — see [Credits](#credits).

## Features

The driver resolves all required kernel symbols at load time via
`kallsyms_lookup_name`, so it works across a wide range of arm64 kernels
(4.4+, including GKI 5.x/6.x) without a rebuild per device.

| Operation | Value | Purpose |
|-----------|-------|---------|
| `OP_INIT_KEY`    | `0x800` | Handshake / key init |
| `OP_READ_MEM`    | `0x801` | Read another process' memory |
| `OP_WRITE_MEM`   | `0x802` | Write another process' memory |
| `OP_MODULE_BASE` | `0x803` | Resolve the base address of a mapped module |
| `OP_GET_PID`     | `0x804` | Resolve a PID from a process/package name |
| `OP_TOUCH_INIT`  | `0x805` | Auto-locate the touchscreen `input_dev` |
| `OP_TOUCH_EVENT` | `0x806` | Inject an input event |
| `OP_CALLFUNC_1`  | `0x900` | Alias of touch init (compatibility) |

Notable implementation details:

- **VMA walking instead of `/proc/<pid>/maps`.** The `mm_struct.mmap`,
  `vm_area_struct.vm_next` and `vm_file` offsets are probed at runtime, so the
  driver does not depend on `seq_file`/`copy_to_user` semantics that broke
  after `set_fs()` removal.
- **Version-aware `FOLL_FORCE`.** The flag changed in kernel 6.3; the value is
  detected at load time to avoid `FOLL_NOWAIT` regressions.
- **`file_operations` layout probing.** The `unlocked_ioctl` slot is derived
  from `def_chr_fops`/`chrdev_open` instead of being hard-coded.
- **Safe reads.** All internal pointer walking uses
  `copy_from_kernel_nofault`/`probe_kernel_read` so a bad pointer cannot panic
  the device.

## Layout

```
kpms/ns_team/      # the KPM driver source (driver.c, Makefile, driver.lds)
kernel/            # KPatch-Next base + patch framework headers and runtime
user/              # kpatch userspace loader (loads .kpm files on device)
tools/             # kptools / symbol helpers
demo/ns_team.h     # example userspace client for the /dev/ns_team ABI
build-kpm.sh       # build helper (toolchain auto-detection)
download-toolchain.txt  # commands to fetch the aarch64 toolchain
```

## Building

### Option A — GitHub Actions

Every push to `main` triggers the **Build KPM** workflow, which downloads the
ARM bare-metal toolchain, builds the module and uploads `ns_team.kpm` as a
build artifact. Grab it from the *Actions* tab.

### Option B — locally

1. Fetch the toolchain once (see `download-toolchain.txt`).
2. Build:

   ```bash
   ./build-kpm.sh ns_team
   # or point at your own toolchain:
   ./build-kpm.sh ns_team /opt/gcc-arm/bin/aarch64-none-elf-
   ```

   The output is `kpms/ns_team/ns_team.kpm`.

## Loading on a device

```bash
adb push kpms/ns_team/ns_team.kpm /data/local/tmp/
adb push <kpatch_binary>          /data/local/tmp/
adb shell su -c '/data/local/tmp/kpatch load /data/local/tmp/ns_team.kpm'
adb shell su -c 'ls -la /dev/ns_team'
```

## Kernel Module Debugging Guide

This guide explains how to retrieve kernel logs and debug kernel panics for the
module. It assumes the device is rooted and you have a shell with `su`
privileges.

### 1. Monitoring live kernel logs

Open a terminal or `adb shell` and gain root:

```bash
su
dmesg -w
```

To follow only NS_team output (the driver prefixes its logs with `ns_team:`):

```bash
dmesg -w | grep -i "ns_team"
```

Useful helpers:

- Clear existing logs before loading the module: `dmesg -c`
- Read the raw kernel message buffer: `cat /proc/kmsg`
- List mapping-related symbols: `cat /proc/kallsyms | grep " vmap"`

### 2. Retrieving kernel panic logs after a reboot

Live logs are lost on a panic, but most kernels persist them in memory.

**Method A — pstore (modern kernels):**

```bash
ls -l /sys/fs/pstore/
cat /sys/fs/pstore/console-ramoops-0
# or
cat /sys/fs/pstore/dmesg-ramoops-0
```

**Method B — last_kmsg (older kernels):**

```bash
cat /proc/last_kmsg
```

Copy the log off the device for easier reading:

```bash
cat /sys/fs/pstore/console-ramoops-0 > /sdcard/panic_log.txt
```

### 3. Investigating a crash

- Grab `console-ramoops-0` immediately after the phone comes back on.
- Search for `Call trace:` — it shows the exact function sequence that faulted.
- Check the `PC` (program counter) and `LR` (link register) addresses shown in
  the trace; `pc is at <function+offset/size>` maps back to the offending line
  in this driver.

## Credits

NS_team builds on the following upstream work:

- **KPatch-Next** by bmax121 — <https://github.com/bmax121/KPatch-Next>
- The original **wanbai-driver** by ALEX5402 — <https://github.com/ALEX5402/wanbai-driver>

This project is a rebrand and cleanup of the above and remains licensed under
the **GNU General Public License v2.0 or later** (see `LICENSE`).
