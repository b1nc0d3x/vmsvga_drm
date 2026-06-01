# vmsvga_drm

A FreeBSD/aarch64 DRM-KMS driver for VMware SVGA-II graphics adapters
(PCI `0x15ad:0x0405` and `0x0406`), built on the in-base `drm2` stack —
no LinuxKPI required.

## Problem solved

On FreeBSD/aarch64 there is **no working DRM-KMS driver for any GPU
that VirtualBox arm64 actually exposes to the guest**. VBox arm64's
only emulated display is VMware SVGA-II; `vmwgfx` from `drm-kmod`
cannot run on aarch64 because `drm-kmod` is built on LinuxKPI, which
is x86-only. Without a native driver every FreeBSD-arm64 VBox guest
is stuck on the EFI loader's static framebuffer (`efifb`) with no
`/dev/dri/card0` — no DRI3, no dumb buffers, no path for an X11
modesetting driver, no path for any Mesa client.

`vmsvga_drm` closes that gap by binding to the SVGA-II device directly
via the in-base `drm2` stack, exposing a working `/dev/dri/card0`
with dumb-buffer ioctls. Userland clients allocate, map, write, and
free DRM buffers exactly as they would on any other DRM device.

## Status matrix

|                                       | VBox arm64       | VMware Fusion arm64       |
|---------------------------------------|------------------|---------------------------|
| PCI bind                              | ✓                | ✓                         |
| SVGA register handshake               | ✓ (`ID_2`)       | ✓ (`ID_3`)                |
| Mode set                              | ✓ readback OK    | ✓ readback OK             |
| `/dev/dri/card0` + KMS scaffold       | ✓                | ✓                         |
| Dumb buffer create / mmap / write     | ✓ end-to-end     | ✓ end-to-end              |
| Visible pixels via VRAM BAR scanout   | ✗ — host bug     | ✓ — first pixels 2026-05-31 |
| DRM-driven scanout (KMS modeset)      | ✗ — host bug     | partial (sysctl-driven; KMS wiring TODO) |

Fusion arm64 reaches **first pixels** via the legacy SVGA-II
linear-framebuffer path: mode-set via `SVGA_REG_{ENABLE, WIDTH,
HEIGHT, BITS_PER_PIXEL}`, then write pixels straight into the VRAM
BAR. Caps mask `0xfd260260` does **not** advertise `SCREEN_OBJECT_2`
or `DISPLAY_TOPOLOGY` — the legacy linear-FB path is the correct
first step on this host. FIFO is `mem_start=0` (unavailable) and is
not needed.

VBox arm64 still cannot accept pixels from the guest on its SVGA-II
emulation: writes to the FB BAR panic the host, and both
host-allocated and guest-allocated FIFO paths are blocked (host
reports `SVGA_REG_MEM_START=0` and ignores guest writes). vmsvga_drm
runs to its functional ceiling on this host — `/dev/dri/card0` works,
dumb buffers work — but DRM-driven scanout is impossible until Oracle
ships a working SVGA-II implementation.

VMware Workstation / ESXi / Fusion on Intel are expected to mirror
the Fusion arm64 path; not yet tested.

### Fusion arm64: required `.vmx` settings

Fusion generates per-VM ACPI tables based on the VM configuration.
Two `.vmx` keys gate whether the SVGA-II adapter ends up at a routed
BAR address:

```
monitor.phys_bits_used = "36"      # 32 caps phys addressing at 4 GB
memsize                = "6144"    # 2048 doesn't produce a high MMIO window
```

With `monitor.phys_bits_used = "32"` Fusion emits an SSDT whose
high-memory descriptor has `M64S = 0` (zero-length window). UEFI is
then forced to park BAR0 in the low PCI window at `0x3d000000`,
which on Fusion arm64 is **not routed to the SVGA device** — reads
return `0xffffffff` and no register access works. Bumping
`phys_bits_used` to `36` (64 GB addressing) and `memsize` to at least
`4096` makes Fusion emit a real high-memory window, and UEFI places
BAR0 inside it (`0xfff800000` in practice).

The VM must be powered off to edit `.vmx`.

## Build

```sh
# Cross-build from FreeBSD/amd64 to aarch64
cd src
env MACHINE=arm64 MACHINE_ARCH=aarch64 \
    CC="cc -target aarch64-unknown-freebsd15.0" \
    SYSDIR=/usr/src/sys make
```

Produces `src/vmsvga_drm.ko` (~36 KB).

### Prerequisite: a kernel with `device drm2` on aarch64

`drm2` on aarch64 is **not officially supported in upstream FreeBSD**
(canonical drm2 has historically been x86-only). The running kernel
must therefore be a custom build of `GENERIC` + `device drm2` +
`device fbd` — stock FreeBSD-arm64 `GENERIC` will not work, and
`drm2.ko` is not packaged as a loadable module in the in-tree
infrastructure.

In practice this means cross-building a custom `KERNCONF` (a one-line
include + two `device` adds is enough), installing the resulting
kernel beside `/boot/kernel/`, and pointing `loader.conf` at it via
`kernel="kernel.your_name"`. The driver itself doesn't care which
host you're on — what it needs is a kernel that has drm2 baked in.

This is a real friction point and one of the motivating reasons for
the ongoing FreeBSD upstream work on drm2 (e.g. PR #2220 for
PRIME/DRI3, PR #2228 for the kmod ldscript fix that lets DRM kmods
loader-preload on arm64).

## Load

```sh
# Runtime
sudo cp src/vmsvga_drm.ko /boot/modules/
sudo kldload vmsvga_drm

# Persist across reboot via rc.conf (recommended)
sudo sysrc kld_list+=vmsvga_drm
```

**Do not** preload via `/boot/loader.conf` until [FreeBSD PR
#2228](https://github.com/freebsd/freebsd-src/pull/2228) (the arm64
kmod ldscript fix) lands — the loader currently panics on DRM kmods
because the ELF header isn't at the offset the arm64 EFI loader
expects.

## Verify

```sh
# 1. Kernel attached
dmesg | grep vmsvga_drm
# expect: drm_load steps A..D + "drm: registered /dev/dri/card0"

# 2. Device + sysctls
ls /dev/dri
sysctl dev.vmsvga_drm.0 | head
# expect: card0 + controlD64; svga_id 0x90000002 (legacy) or
# 0x90000003 (Fusion arm64); caps, max_width / max_height, vram_size

# 3. End-to-end ioctl through the driver
cc -I/usr/local/include -o /usr/local/bin/dumb_test tools/dumb_test.c
dumb_test
# expect:
#   CREATE_DUMB: handle=1 pitch=4096 size=3145728
#   MAP_DUMB: offset=0x8000000000000000
#   mmap: va=...
#   write+readback OK across 3145728 bytes
#   DESTROY_DUMB: ok
```

If all three pass the driver is fully functional at every layer it
can be on the host.

### Fusion arm64: first-pixels probe

On Fusion arm64 there is a sysctl-driven probe that exercises the
full mode-set → VRAM-BAR scanout path without needing X or a Mesa
client:

```sh
# Pick a resolution within max_width × max_height
sudo sysctl dev.vmsvga_drm.0.want_width=1920
sudo sysctl dev.vmsvga_drm.0.want_height=1080

# Paint a four-quadrant colour pattern straight into the VRAM BAR
sudo sysctl dev.vmsvga_drm.0.test_pattern=1
# expect:
#   red    | green
#   ---------------
#   blue   | white

# Cycle the framebuffer through eight solid colours (~100 ms per frame)
sudo sysctl dev.vmsvga_drm.0.animate=80
```

These are diagnostic / smoke-test sysctls — not the eventual scanout
path. The next milestone is wiring `vmsvga_set_mode` into the DRM
atomic mode-set helper so userland `drmModeSetCrtc` ioctls drive the
chip, and adding a dumb-buffer → VRAM-BAR blit (or a direct
`SVGA_REG_FB_*` pointer override) so X / Wayland clients display
through `/dev/dri/card0`.

## Coexistence with X.org on VBox arm64

Because DRM-driven scanout doesn't reach the host on VBox arm64, X
should drive the EFI framebuffer directly via the `scfb` driver:

```
# /usr/local/etc/X11/xorg.conf.d/10-scfb.conf
Section "Device"
    Identifier "Card0"
    Driver "scfb"
EndSection
```

`scfb` paints into `efifb`; `vmsvga_drm` sits idle on `/dev/dri/card0`.
The two never fight. For a usable resolution add to `/boot/loader.conf`:

```
efi_max_resolution="1920x1080"
```

## Phase history

| Phase | Commit  | What landed                                              |
|-------|---------|----------------------------------------------------------|
| A     | d95f8a3 | PCI bind via `device_identify` + SVGA `ID_2` handshake   |
| B     | ce96904 | Mode set; FIFO discovery; documented FB BAR panic        |
| C.1   | 5f057c5 | `drm_device` registration + KMS scaffold + `/dev/dri/card0` |
| C.2   | a7952d1 | Dumb buffers in guest RAM via `cdev_pager` mmap          |
| D pr. | 456b13b | Guest-allocated FIFO probe; documented dead-end on VBox  |
| ID3   | 95edcea | Accept `SVGA_ID_3` (Fusion arm64) in negotiate           |
| D px. | b106036 | First pixels on Fusion arm64 via VRAM-BAR linear scanout |
| trim  | 8cf703f | Drop unreachable guest-FIFO probe and `gfifo_*` state    |

Detailed write-ups for every phase are in the commit messages.

## Hard-won implementation notes

- **Use `drm_get_platform_dev`, not `drm_get_pci_dev`**, even though
  the driver attaches under `vgapci`. The PCI variant calls
  `pci_get_domain/bus/slot/func` on `dev->dev` and panics for `vgapci`
  children; the platform variant just trusts `dev->dev`.
- On aarch64 the SVGA register file is **MMIO-mapped flat** (each
  register at `reg * 4`); only x86 uses the `INDEX/VALUE` port pair.
  Detect via the `SYS_RES` type on BAR0.
- VBox arm64 leaves BAR1 unpopulated and puts the framebuffer at
  BAR2. Walk BARs 1..5 looking for the largest memory BAR rather than
  hard-coding slot 1.
- **Never write the FB BAR with `bus_space_*` on VBox arm64** even
  if it appears KVA-mapped (`rman_get_virtual` returns a sane address).
  The host emulation crashes the guest on the first write.
- The `cdev_pager` trick for `dumb_create` is required: allocate the
  pager keyed on `gem_obj` and pre-insert every backing page with
  `PG_FICTITIOUS` set so the on-demand fault path never runs.
  Without this, `cdev_pager_allocate` creates a fresh empty pager
  per `mmap` and userspace writes never reach the kernel's mapping.
- On Fusion arm64 the SVGA caps mask (`0xfd260260`) does **not**
  advertise `SCREEN_OBJECT_2` or `DISPLAY_TOPOLOGY`. Don't chase
  those paths — the host honours the legacy SVGA-II linear-FB
  protocol instead: mode-set via `SVGA_REG_{ENABLE, WIDTH, HEIGHT,
  BITS_PER_PIXEL}`, then write pixels straight into the VRAM BAR at
  `y * SVGA_REG_BYTES_PER_LINE + x * 4`. No `SVGA_CMD_UPDATE`, no
  FIFO sync — the host re-scans the BAR every refresh.
- The Fusion arm64 BAR-routing problem turned out to be a per-VM
  `.vmx` configuration issue, not a FreeBSD bug. See the
  ["Required `.vmx` settings"](#fusion-arm64-required-vmx-settings)
  section above. Linux on a different VM does not hit it because the
  Ubuntu guest profile in Fusion defaults `monitor.phys_bits_used`
  to `36`.

## License

BSD-2-Clause throughout. SPDX-License-Identifier headers present on
every source file.

## Related repos

- [`virtio_drm`](https://github.com/b1nc0d3x/virtio_drm) — sibling
  driver for VirtIO GPU; same KMS / GEM scaffold, different transport.
  Closes the FreeBSD-arm64 graphics gap on QEMU / UTM / Cloud
  Hypervisor.
- FreeBSD upstream PR series on RK3399 USB-C DisplayPort + USB-PD
  (filed under `freebsd/freebsd-src` as #2197 / #2198 / #2211 / #2220
  / #2225 / #2228 / #2231 / #2236 / #2237).
