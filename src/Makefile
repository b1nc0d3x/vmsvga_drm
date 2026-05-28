# SPDX-License-Identifier: BSD-2-Clause
#
# Out-of-tree kernel module for the vmsvga_drm driver.
#
# Binds the FreeBSD drm2 stack to VMware SVGA-II graphics adapters
# (PCI vendor 0x15ad device 0x0406) as exposed by VirtualBox arm64
# guests, VMware Fusion / Workstation, and VMware ESXi.
#
# Cross-build on a FreeBSD/amd64 build host for arm64:
#
#   make TARGET_ARCH=aarch64
#
# Native build on the target:
#
#   make
#
# Produces vmsvga_drm.ko in the current directory.

KMOD=	vmsvga_drm
SRCS=	vmsvga_drm.c
SRCS+=	bus_if.h device_if.h pci_if.h fb_if.h
SRCS+=	opt_syscons.h opt_platform.h opt_acpi.h opt_teken.h opt_kbd.h opt_evdev.h opt_splash.h opt_drm.h opt_vm.h

.if defined(TARGET_ARCH)
MACHINE_ARCH=	${TARGET_ARCH}
.endif

SYSDIR?=	/usr/src/sys

CFLAGS+=	-I${SYSDIR}

.include <bsd.kmod.mk>
