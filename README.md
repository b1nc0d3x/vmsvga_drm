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

|                                   | VBox arm64       | VMware Fusion arm64 |
|-----------------------------------|------------------|---------------------|
| PCI bind                          | ✓                | expected ✓ (untested) |
| SVGA register handshake (`ID_2`)  | ✓                | expected ✓          |
| Mode set                          | ✓ readback OK    | expected ✓          |
| `/dev/dri/card0` + KMS scaffold   | ✓                | expected ✓          |
| Dumb buffer create / mmap / write | ✓ end-to-end     | expected ✓          |
| Visible pixels via DRM            | ✗ — host bug     | expected ✓          |

VBox arm64 cannot accept pixels from the guest at all on its SVGA-II
emulation: writes to the FB BAR panic the host, and both host-allocated
and guest-allocated FIFO paths are blocked (host reports
`SVGA_REG_MEM_START=0` and ignores guest writes). vmsvga_drm runs to
its functional ceiling on this host — `/dev/dri/card0` works, dumb
buffers work — but DRM-driven scanout is impossible until Oracle ships
a working SVGA-II implementation. On VMware Fusion / Workstation / ESXi
the same driver should produce visible pixels with no code changes;
not yet tested by the author.

## Build

```sh
# Cross-build from FreeBSD/amd64 to aarch64
cd src
env MACHINE=arm64 MACHINE_ARCH=aarch64 \
    CC="cc -target aarch64-unknown-freebsd15.0" \
    SYSDIR=/usr/src/sys make
```

Produces `src/vmsvga_drm.ko` (~36 KB).

Requires the running kernel to have `device drm2` statically linked.
Loading `drm2` itself as a module is not supported by the in-tree
infrastructure; use a custom `KERNCONF` (e.g. `GENERIC` + `device
drm2 + device fbd`).

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
# expect: card0 + controlD64; svga_id=0x90000002, caps, max, vram

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
| D     | 456b13b | Guest-allocated FIFO probe; documented dead-end          |

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

## License

BSD-2-Clause throughout. SPDX-License-Identifier headers present on
every source file.

## Related work by the same author

- [`virtio_drm`](https://github.com/b1nc0d3x/virtio_drm) — sibling
  driver for VirtIO GPU; same KMS / GEM scaffold, different transport.
  Closes the FreeBSD-arm64 graphics gap on QEMU / UTM / Cloud
  Hypervisor.
- FreeBSD upstream PR series on RK3399 USB-C DisplayPort + USB-PD
  (filed under `freebsd/freebsd-src` as #2197 / #2198 / #2211 / #2220
  / #2225 / #2228 / #2231 / #2236 / #2237).
