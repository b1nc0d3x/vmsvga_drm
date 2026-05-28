/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, Kyle Crenshaw <b1nc0d3x@gmail.com>
 *
 * vmsvga_drm: FreeBSD drm2 driver for VMware SVGA-II graphics
 * adapters (PCI vendor 0x15ad device 0x0405 / 0x0406), the only
 * GPU exposed to VirtualBox arm64 guests and the legacy 2D path
 * used by VMware Fusion / Workstation / ESXi.
 *
 * Bring-up plan, in order:
 *
 *   Phase A (this file): newbus probe + attach, BAR mapping,
 *                        SVGA_REG_ID handshake, capabilities &
 *                        VRAM/FIFO sizing, sysctls.  Loads cleanly
 *                        but exposes no DRM device yet.
 *   Phase B:             FIFO init + SVGA_CMD_UPDATE emit; first
 *                        "tell the host we changed pixel X" works.
 *   Phase C:             drm_device registration + dumb-buffer +
 *                        single CRTC/encoder/virtual-connector.
 *                        /dev/dri/card0 appears.
 *   Phase D:             fbd glue + vt(4) console handover so the
 *                        boot console moves onto the SVGA scanout.
 *
 * Reference: linux/drivers/gpu/drm/vmwgfx (2D subset only),
 * the legacy xorg vmware-svga driver, and our own virtio_drm.c
 * (same shape, different transport).
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "vmsvga_reg.h"

#define	VMSVGA_DRM_DRIVER_NAME	"vmsvga_drm"
#define	VMSVGA_DRM_DRIVER_DESC	"FreeBSD VMware SVGA-II DRM-KMS"

struct vmsvga_softc {
	device_t		 dev;

	/*
	 * BAR0 -- indexed register pair (INDEX/VALUE).  On x86 this
	 * is an IO-port range; on arm64 VBox wires it as MMIO.  We
	 * probe the SYS_RES type at attach to decide which.
	 */
	int			 reg_rid;
	int			 reg_type;	/* SYS_RES_IOPORT or _MEMORY */
	struct resource		*reg_res;
	bus_space_tag_t		 reg_bst;
	bus_space_handle_t	 reg_bsh;

	/* BAR1 -- linear framebuffer (VRAM). */
	int			 fb_rid;
	struct resource		*fb_res;
	bus_addr_t		 fb_pa;
	bus_size_t		 fb_size;

	/* BAR2 -- FIFO command ring. */
	int			 fifo_rid;
	struct resource		*fifo_res;
	bus_size_t		 fifo_size;
	uint32_t volatile	*fifo;		/* virtual mapping */

	/* Negotiated SVGA state. */
	uint32_t		 svga_id;
	uint32_t		 capabilities;
	uint32_t		 max_width;
	uint32_t		 max_height;
	uint32_t		 host_bpp;
	uint32_t		 vram_size;
};

/* ---------- low-level SVGA register access ----------
 *
 * Two transports:
 *
 *   - IO-port mode (classic x86): BAR0 holds a 2-port pair.
 *     Write index to PORT+0, then read/write data at PORT+1.
 *
 *   - MMIO mode (arm64 VBox, modern PCIe paths): BAR0 is a
 *     flat MMIO window where each SVGA register lives at
 *     offset (reg * 4).  No index/value indirection.
 *
 * We pick at attach time based on which BAR0 type the host
 * presented.
 */

static inline uint32_t
vmsvga_read_reg(struct vmsvga_softc *sc, uint16_t reg)
{
	if (sc->reg_type == SYS_RES_IOPORT) {
		bus_space_write_4(sc->reg_bst, sc->reg_bsh,
		    SVGA_INDEX_PORT * 4, reg);
		return (bus_space_read_4(sc->reg_bst, sc->reg_bsh,
		    SVGA_VALUE_PORT * 4));
	}
	return (bus_space_read_4(sc->reg_bst, sc->reg_bsh,
	    (bus_size_t)reg * 4));
}

static inline void
vmsvga_write_reg(struct vmsvga_softc *sc, uint16_t reg, uint32_t val)
{
	if (sc->reg_type == SYS_RES_IOPORT) {
		bus_space_write_4(sc->reg_bst, sc->reg_bsh,
		    SVGA_INDEX_PORT * 4, reg);
		bus_space_write_4(sc->reg_bst, sc->reg_bsh,
		    SVGA_VALUE_PORT * 4, val);
		return;
	}
	bus_space_write_4(sc->reg_bst, sc->reg_bsh,
	    (bus_size_t)reg * 4, val);
}

/* ---------- newbus identify / probe / attach ---------- */

/*
 * Register ourselves as a child of every vgapci0 whose underlying
 * PCI device matches VMware SVGA-II.  vga_pci_attach calls
 * bus_identify_children() which walks all drivers registered to
 * the vgapci class and invokes this hook -- the analogue of
 * agp_i810_identify().  Without it, DRIVER_MODULE on a vgapci
 * parent never sees a child to probe and the driver stays inert.
 */
static void
vmsvga_identify(driver_t *driver, device_t parent)
{
	uint16_t vendor, device;

	vendor = pci_get_vendor(parent);
	device = pci_get_device(parent);
	if (vendor != PCI_VENDOR_ID_VMWARE)
		return;
	if (device != PCI_DEVICE_ID_VMWARE_SVGA2 &&
	    device != PCI_DEVICE_ID_VMWARE_SVGA2_VBOX)
		return;
	if (device_find_child(parent, "vmsvga_drm",
	    DEVICE_UNIT_ANY) != NULL)
		return;
	device_add_child(parent, "vmsvga_drm", DEVICE_UNIT_ANY);
}

static int
vmsvga_probe(device_t dev)
{
	device_t parent;
	uint16_t vendor, device;

	parent = device_get_parent(dev);
	vendor = pci_get_vendor(parent);
	device = pci_get_device(parent);
	if (vendor != PCI_VENDOR_ID_VMWARE)
		return (ENXIO);
	if (device != PCI_DEVICE_ID_VMWARE_SVGA2 &&
	    device != PCI_DEVICE_ID_VMWARE_SVGA2_VBOX)
		return (ENXIO);

	device_set_desc(dev, VMSVGA_DRM_DRIVER_DESC);
	return (BUS_PROBE_VENDOR);
}

static int
vmsvga_alloc_resources(struct vmsvga_softc *sc)
{
	pcicfgregs *cfg __unused;

	/*
	 * BAR0: registers.  Try IO first (x86 convention), fall
	 * back to memory (arm64 VBox).
	 */
	sc->reg_rid = PCIR_BAR(0);
	sc->reg_type = SYS_RES_IOPORT;
	sc->reg_res = bus_alloc_resource_any(sc->dev, sc->reg_type,
	    &sc->reg_rid, RF_ACTIVE);
	if (sc->reg_res == NULL) {
		sc->reg_rid = PCIR_BAR(0);
		sc->reg_type = SYS_RES_MEMORY;
		sc->reg_res = bus_alloc_resource_any(sc->dev, sc->reg_type,
		    &sc->reg_rid, RF_ACTIVE);
	}
	if (sc->reg_res == NULL) {
		device_printf(sc->dev,
		    "failed to allocate BAR0 (svga regs)\n");
		return (ENXIO);
	}
	sc->reg_bst = rman_get_bustag(sc->reg_res);
	sc->reg_bsh = rman_get_bushandle(sc->reg_res);

	/*
	 * VRAM framebuffer and (optional) FIFO command ring live in
	 * PCI memory BARs.  Slot assignment varies by host: classic
	 * ESXi/VMware uses BAR1=VRAM + BAR2=FIFO; VBox arm64 leaves
	 * BAR1 unpopulated and exposes only BAR0+BAR2 (VRAM).  Walk
	 * BARs 1..5 and bucket by size: the largest memory BAR is
	 * the framebuffer, any second one is the FIFO.
	 */
	for (int slot = 1; slot <= 5; slot++) {
		int rid = PCIR_BAR(slot);
		struct resource *r;
		bus_size_t sz;

		r = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY,
		    &rid, RF_ACTIVE);
		if (r == NULL)
			continue;
		sz = rman_get_size(r);
		if (sc->fb_res == NULL || sz > sc->fb_size) {
			if (sc->fb_res != NULL) {
				/* Demote previous fb to fifo candidate. */
				if (sc->fifo_res == NULL) {
					sc->fifo_res  = sc->fb_res;
					sc->fifo_rid  = sc->fb_rid;
					sc->fifo_size = sc->fb_size;
					sc->fifo      = (uint32_t volatile *)
					    rman_get_virtual(sc->fifo_res);
				} else {
					bus_release_resource(sc->dev,
					    SYS_RES_MEMORY, sc->fb_rid,
					    sc->fb_res);
				}
			}
			sc->fb_res  = r;
			sc->fb_rid  = rid;
			sc->fb_pa   = rman_get_start(r);
			sc->fb_size = sz;
		} else if (sc->fifo_res == NULL) {
			sc->fifo_res  = r;
			sc->fifo_rid  = rid;
			sc->fifo_size = sz;
			sc->fifo      = (uint32_t volatile *)
			    rman_get_virtual(r);
		} else {
			bus_release_resource(sc->dev, SYS_RES_MEMORY,
			    rid, r);
		}
	}
	if (sc->fb_res == NULL) {
		device_printf(sc->dev,
		    "no framebuffer BAR found\n");
		return (ENXIO);
	}

	return (0);
}

static void
vmsvga_free_resources(struct vmsvga_softc *sc)
{
	if (sc->fifo_res != NULL) {
		bus_release_resource(sc->dev, SYS_RES_MEMORY,
		    sc->fifo_rid, sc->fifo_res);
		sc->fifo_res = NULL;
	}
	if (sc->fb_res != NULL) {
		bus_release_resource(sc->dev, SYS_RES_MEMORY,
		    sc->fb_rid, sc->fb_res);
		sc->fb_res = NULL;
	}
	if (sc->reg_res != NULL) {
		bus_release_resource(sc->dev, sc->reg_type,
		    sc->reg_rid, sc->reg_res);
		sc->reg_res = NULL;
	}
}

static int
vmsvga_negotiate_id(struct vmsvga_softc *sc)
{
	uint32_t id;

	/*
	 * Write the highest version we want, read back the highest
	 * the host supports.  Convention: write SVGA_ID_2, accept
	 * whatever comes back as long as it's >= SVGA_ID_0.
	 */
	vmsvga_write_reg(sc, SVGA_REG_ID, SVGA_ID_2);
	id = vmsvga_read_reg(sc, SVGA_REG_ID);
	if (id != SVGA_ID_2) {
		vmsvga_write_reg(sc, SVGA_REG_ID, SVGA_ID_1);
		id = vmsvga_read_reg(sc, SVGA_REG_ID);
	}
	if (id != SVGA_ID_2 && id != SVGA_ID_1 && id != SVGA_ID_0) {
		device_printf(sc->dev,
		    "SVGA ID handshake failed (got 0x%08x)\n", id);
		return (ENXIO);
	}
	sc->svga_id = id;
	return (0);
}

static void
vmsvga_read_hw_info(struct vmsvga_softc *sc)
{
	sc->capabilities = vmsvga_read_reg(sc, SVGA_REG_CAPABILITIES);
	sc->max_width    = vmsvga_read_reg(sc, SVGA_REG_MAX_WIDTH);
	sc->max_height   = vmsvga_read_reg(sc, SVGA_REG_MAX_HEIGHT);
	sc->host_bpp     = vmsvga_read_reg(sc, SVGA_REG_HOST_BITS_PER_PIXEL);
	sc->vram_size    = vmsvga_read_reg(sc, SVGA_REG_VRAM_SIZE);
}

static void
vmsvga_sysctl_setup(struct vmsvga_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *children;

	ctx      = device_get_sysctl_ctx(sc->dev);
	tree     = device_get_sysctl_tree(sc->dev);
	children = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "svga_id",
	    CTLFLAG_RD, &sc->svga_id, 0,
	    "Negotiated SVGA interface ID (SVGA_ID_0/1/2)");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "capabilities",
	    CTLFLAG_RD, &sc->capabilities, 0,
	    "SVGA_REG_CAPABILITIES bitmask");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "max_width",
	    CTLFLAG_RD, &sc->max_width, 0, "Host max width");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "max_height",
	    CTLFLAG_RD, &sc->max_height, 0, "Host max height");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "host_bpp",
	    CTLFLAG_RD, &sc->host_bpp, 0, "Host bits-per-pixel");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "vram_size",
	    CTLFLAG_RD, &sc->vram_size, 0, "VRAM size in bytes");
}

static int
vmsvga_attach(device_t dev)
{
	struct vmsvga_softc *sc;
	int err;

	sc = device_get_softc(dev);
	sc->dev = dev;

	pci_enable_busmaster(dev);

	err = vmsvga_alloc_resources(sc);
	if (err != 0)
		goto fail;

	err = vmsvga_negotiate_id(sc);
	if (err != 0)
		goto fail;

	vmsvga_read_hw_info(sc);
	vmsvga_sysctl_setup(sc);

	device_printf(dev,
	    "SVGA-II id=0x%08x caps=0x%08x max=%ux%u host_bpp=%u "
	    "vram=%u bytes fifo=%lu bytes (reg BAR=%s, fb_pa=0x%jx, "
	    "fifo BAR=%s)\n",
	    sc->svga_id, sc->capabilities,
	    sc->max_width, sc->max_height, sc->host_bpp,
	    sc->vram_size, (unsigned long)sc->fifo_size,
	    sc->reg_type == SYS_RES_IOPORT ? "ioport" : "memory",
	    (uintmax_t)sc->fb_pa,
	    sc->fifo_res != NULL ? "present" : "absent");

	return (0);
fail:
	vmsvga_free_resources(sc);
	return (err);
}

static int
vmsvga_detach(device_t dev)
{
	struct vmsvga_softc *sc;

	sc = device_get_softc(dev);
	vmsvga_free_resources(sc);
	return (0);
}

static device_method_t vmsvga_methods[] = {
	DEVMETHOD(device_identify,	vmsvga_identify),
	DEVMETHOD(device_probe,		vmsvga_probe),
	DEVMETHOD(device_attach,	vmsvga_attach),
	DEVMETHOD(device_detach,	vmsvga_detach),

	DEVMETHOD_END
};

static driver_t vmsvga_driver = {
	"vmsvga_drm",
	vmsvga_methods,
	sizeof(struct vmsvga_softc)
};

DRIVER_MODULE(vmsvga_drm, vgapci, vmsvga_driver, NULL, NULL);
MODULE_VERSION(vmsvga_drm, 1);
MODULE_PNP_INFO("U16:vendor;U16:device", vgapci, vmsvga_drm,
    ((const struct {
	uint16_t v, d;
    }[]) {
	{ PCI_VENDOR_ID_VMWARE, PCI_DEVICE_ID_VMWARE_SVGA2 },
	{ PCI_VENDOR_ID_VMWARE, PCI_DEVICE_ID_VMWARE_SVGA2_VBOX },
    }), 2);
