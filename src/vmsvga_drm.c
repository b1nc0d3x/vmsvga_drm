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
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/rwlock.h>
#include <sys/sysctl.h>
#include <sys/pctrie.h>
#include <sys/vmem.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h>
#include <vm/vm_phys.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <dev/drm2/drmP.h>
#include <dev/drm2/drm_crtc.h>
#include <dev/drm2/drm_crtc_helper.h>

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

	/*
	 * FIFO command ring.  On classic SVGA-II this is a separate
	 * memory BAR (typically BAR2).  On VBox arm64 there is no
	 * second BAR -- the FIFO physical address lives at
	 * SVGA_REG_MEM_START and falls inside BAR0's MMIO window, so
	 * we reuse reg_bst/reg_bsh with an offset.  fifo_offset is
	 * the byte offset from reg_bsh; fifo_res != NULL means we
	 * had a dedicated BAR (classic path).
	 */
	int			 fifo_rid;
	struct resource		*fifo_res;
	bus_size_t		 fifo_offset;	/* offset inside reg_bsh */
	bus_size_t		 fifo_size;
	uint32_t		 fifo_pa;	/* SVGA_REG_MEM_START */
	bool			 fifo_ready;

	/* Negotiated SVGA state. */
	uint32_t		 svga_id;
	uint32_t		 capabilities;
	uint32_t		 max_width;
	uint32_t		 max_height;
	uint32_t		 host_bpp;
	uint32_t		 vram_size;

	/* Current programmed mode (Phase B sanity test). */
	uint32_t		 cur_width;
	uint32_t		 cur_height;
	uint32_t		 cur_bpp;
	uint32_t		 cur_bytes_per_line;
	uint32_t		 cur_enable;

	/* Phase B sysctl trigger knobs. */
	uint32_t		 want_width;
	uint32_t		 want_height;
	uint32_t		 want_bpp;

	/* Phase C: DRM-KMS scaffold. */
	struct drm_device	*drm_dev;
	struct drm_crtc		 crtc;
	struct drm_encoder	 encoder;
	struct drm_connector	 connector;

	/* Phase C.2: dumb-buffer bookkeeping. */
	TAILQ_HEAD(, vmsvga_gem_bo) bos;
	struct mtx		 bos_mtx;
	uint32_t		 next_resource_id;

	/* Phase D: guest-allocated FIFO probe. */
	vm_offset_t		 gfifo_vbase;
	vm_paddr_t		 gfifo_pbase;
	vm_page_t	       *gfifo_pages;
	size_t			 gfifo_npages;
	size_t			 gfifo_size;
	bool			 gfifo_ready;
};

/* GEM bo backing one dumb buffer.  All memory is in guest RAM --
 * we never write the FB BAR on VBox arm64.  Userland mmap and our
 * kernel-side bo->vbase share the same physical pages via the
 * cdev_pager (tegra pattern). */
struct vmsvga_gem_bo {
	struct drm_gem_object	gem_obj;
	vm_offset_t		vbase;	/* kernel VA */
	bus_addr_t		pbase;	/* physaddr of pages[0] */
	size_t			size;
	size_t			npages;
	vm_page_t	       *pages;	/* npages entries */
	vm_object_t		cdev_pager;
	uint32_t		resource_id;
	TAILQ_ENTRY(vmsvga_gem_bo) link;
};

struct vmsvga_drm_framebuffer {
	struct drm_framebuffer	base;
	struct vmsvga_gem_bo   *bo;
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

/* ---------- FIFO access ----------
 *
 * The command FIFO is a 32-bit ring buffer with a fixed 4-word
 * header (MIN, MAX, NEXT_CMD, STOP) followed by the command
 * payload area.
 *
 *    fifo[SVGA_FIFO_MIN]      = first byte offset of payload
 *    fifo[SVGA_FIFO_MAX]      = byte offset just past last byte
 *    fifo[SVGA_FIFO_NEXT_CMD] = byte offset where guest writes next
 *    fifo[SVGA_FIFO_STOP]     = byte offset host has consumed up to
 *
 * All offsets are byte offsets from fifo[0].  Producer writes
 * commands at NEXT_CMD, advancing it (wrapping at MAX back to
 * MIN).  Consumer (host) advances STOP.
 */

static inline uint32_t
vmsvga_fifo_read(struct vmsvga_softc *sc, uint32_t word_off)
{
	return (bus_space_read_4(sc->reg_bst, sc->reg_bsh,
	    sc->fifo_offset + word_off * 4));
}

static inline void
vmsvga_fifo_write(struct vmsvga_softc *sc, uint32_t word_off, uint32_t val)
{
	bus_space_write_4(sc->reg_bst, sc->reg_bsh,
	    sc->fifo_offset + word_off * 4, val);
}

static int
vmsvga_fifo_init(struct vmsvga_softc *sc)
{
	uint32_t header_size;

	sc->fifo_pa   = vmsvga_read_reg(sc, SVGA_REG_MEM_START);
	sc->fifo_size = vmsvga_read_reg(sc, SVGA_REG_MEM_SIZE);
	if (sc->fifo_pa == 0 || sc->fifo_size == 0) {
		device_printf(sc->dev,
		    "FIFO disabled by host (mem_start=0x%08x mem_size=%lu)\n",
		    sc->fifo_pa, (unsigned long)sc->fifo_size);
		return (ENXIO);
	}

	/*
	 * The FIFO physical address must lie inside BAR0 (the only
	 * MMIO window we mapped on VBox arm64).  Translate to a
	 * byte offset from reg_bsh.
	 */
	if (sc->fifo_res != NULL) {
		/* Classic path: we already mapped a dedicated FIFO BAR.
		 * fifo_offset stays 0 in that branch; nothing more to do. */
	} else {
		bus_addr_t reg_start = rman_get_start(sc->reg_res);
		bus_size_t reg_size  = rman_get_size(sc->reg_res);

		if (sc->fifo_pa < reg_start ||
		    sc->fifo_pa + sc->fifo_size > reg_start + reg_size) {
			device_printf(sc->dev,
			    "FIFO at 0x%08x size %lu is outside BAR0 "
			    "(0x%jx +%ju); cannot map\n",
			    sc->fifo_pa, (unsigned long)sc->fifo_size,
			    (uintmax_t)reg_start, (uintmax_t)reg_size);
			return (ENXIO);
		}
		sc->fifo_offset = sc->fifo_pa - reg_start;
	}

	/*
	 * Initialise the FIFO header.  MIN is the first byte after
	 * the header; MAX is the total ring size; NEXT_CMD and STOP
	 * both start at MIN (empty ring).
	 */
	header_size = SVGA_FIFO_NUM_REGS * 4;
	vmsvga_fifo_write(sc, SVGA_FIFO_MIN,      header_size);
	vmsvga_fifo_write(sc, SVGA_FIFO_MAX,      sc->fifo_size);
	vmsvga_fifo_write(sc, SVGA_FIFO_NEXT_CMD, header_size);
	vmsvga_fifo_write(sc, SVGA_FIFO_STOP,     header_size);

	/* CONFIG_DONE = 1 hands the FIFO over to the host. */
	vmsvga_write_reg(sc, SVGA_REG_CONFIG_DONE, 1);
	sc->fifo_ready = true;

	device_printf(sc->dev,
	    "FIFO ready pa=0x%08x size=%lu offset_in_BAR0=0x%jx "
	    "(MIN=%u MAX=%u NEXT=%u STOP=%u)\n",
	    sc->fifo_pa, (unsigned long)sc->fifo_size,
	    (uintmax_t)sc->fifo_offset,
	    vmsvga_fifo_read(sc, SVGA_FIFO_MIN),
	    vmsvga_fifo_read(sc, SVGA_FIFO_MAX),
	    vmsvga_fifo_read(sc, SVGA_FIFO_NEXT_CMD),
	    vmsvga_fifo_read(sc, SVGA_FIFO_STOP));
	return (0);
}

/*
 * Wait for host to drain enough FIFO room for one more dword.
 * In real life this would poll SVGA_REG_BUSY / wait on FENCE.
 * For Phase B we just spin a few times.
 */
static int
vmsvga_fifo_reserve(struct vmsvga_softc *sc, uint32_t bytes)
{
	uint32_t min, max, next, stop, free;
	int spins;

	min = vmsvga_fifo_read(sc, SVGA_FIFO_MIN);
	max = vmsvga_fifo_read(sc, SVGA_FIFO_MAX);
	for (spins = 0; spins < 1000; spins++) {
		next = vmsvga_fifo_read(sc, SVGA_FIFO_NEXT_CMD);
		stop = vmsvga_fifo_read(sc, SVGA_FIFO_STOP);
		if (next >= stop)
			free = (max - next) + (stop - min);
		else
			free = stop - next;
		if (free > bytes)
			return (0);
		/* Kick the host. */
		vmsvga_write_reg(sc, SVGA_REG_SYNC, 1);
		(void)vmsvga_read_reg(sc, SVGA_REG_BUSY);
		DELAY(100);
	}
	return (EBUSY);
}

static void
vmsvga_fifo_push(struct vmsvga_softc *sc, uint32_t dword)
{
	uint32_t min, max, next;

	min  = vmsvga_fifo_read(sc, SVGA_FIFO_MIN);
	max  = vmsvga_fifo_read(sc, SVGA_FIFO_MAX);
	next = vmsvga_fifo_read(sc, SVGA_FIFO_NEXT_CMD);

	bus_space_write_4(sc->reg_bst, sc->reg_bsh,
	    sc->fifo_offset + next, dword);
	next += 4;
	if (next == max)
		next = min;
	vmsvga_fifo_write(sc, SVGA_FIFO_NEXT_CMD, next);
}

static int
vmsvga_fifo_emit_update(struct vmsvga_softc *sc,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	int err;

	if (!sc->fifo_ready)
		return (ENXIO);
	err = vmsvga_fifo_reserve(sc, 5 * 4);
	if (err != 0) {
		device_printf(sc->dev,
		    "FIFO reserve timeout\n");
		return (err);
	}
	vmsvga_fifo_push(sc, SVGA_CMD_UPDATE);
	vmsvga_fifo_push(sc, x);
	vmsvga_fifo_push(sc, y);
	vmsvga_fifo_push(sc, w);
	vmsvga_fifo_push(sc, h);
	/* Kick. */
	vmsvga_write_reg(sc, SVGA_REG_SYNC, 1);
	(void)vmsvga_read_reg(sc, SVGA_REG_BUSY);
	return (0);
}

/* ---------- mode set ---------- */

static int
vmsvga_set_mode(struct vmsvga_softc *sc, uint32_t w, uint32_t h, uint32_t bpp)
{
	if (w == 0 || h == 0)
		return (EINVAL);
	if (w > sc->max_width || h > sc->max_height)
		return (EINVAL);
	if (bpp != 32 && bpp != 24 && bpp != 16)
		return (EINVAL);

	vmsvga_write_reg(sc, SVGA_REG_ENABLE,         0);
	vmsvga_write_reg(sc, SVGA_REG_WIDTH,          w);
	vmsvga_write_reg(sc, SVGA_REG_HEIGHT,         h);
	vmsvga_write_reg(sc, SVGA_REG_BITS_PER_PIXEL, bpp);
	vmsvga_write_reg(sc, SVGA_REG_ENABLE,         1);

	sc->cur_width          = vmsvga_read_reg(sc, SVGA_REG_WIDTH);
	sc->cur_height         = vmsvga_read_reg(sc, SVGA_REG_HEIGHT);
	sc->cur_bpp            = vmsvga_read_reg(sc, SVGA_REG_BITS_PER_PIXEL);
	sc->cur_bytes_per_line = vmsvga_read_reg(sc, SVGA_REG_BYTES_PER_LINE);
	sc->cur_enable         = vmsvga_read_reg(sc, SVGA_REG_ENABLE);

	device_printf(sc->dev,
	    "mode set %ux%u@%ubpp -> readback %ux%u@%ubpp "
	    "stride=%u enable=%u\n",
	    w, h, bpp,
	    sc->cur_width, sc->cur_height, sc->cur_bpp,
	    sc->cur_bytes_per_line, sc->cur_enable);
	return (0);
}

/* ====================================================================
 * Phase C -- DRM-KMS scaffold
 *
 * Goal: /dev/dri/card0 appears, modeset ioctls work, dumb buffers
 * land in guest RAM (never the FB BAR -- Phase B proved writing
 * the BAR panics VBox arm64).  Real pixel display deferred to a
 * later phase that goes via SVGA_CMD_DEFINE_SCREEN_OBJECT_v2.
 *
 * Single head: one CRTC, one virtual encoder, one virtual
 * connector that always reports "connected" with a fixed mode
 * range up to max_width x max_height.
 * ==================================================================== */

/* ---- GEM dumb buffer + framebuffer ---- */

/*
 * cdev_pager ops: pages are pre-inserted at dumb_create time, so
 * cdev_pg_fault should never fire.  ctor/dtor are no-ops.  Same
 * shape as virtio_drm and tegra.
 */
static int
vmsvga_gem_pager_fault(vm_object_t vm_obj, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	return (VM_PAGER_FAIL);
}

static int
vmsvga_gem_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	if (color != NULL)
		*color = 0;
	return (0);
}

static void
vmsvga_gem_pager_dtor(void *handle)
{
}

static struct cdev_pager_ops vmsvga_gem_pager_ops = {
	.cdev_pg_fault	= vmsvga_gem_pager_fault,
	.cdev_pg_ctor	= vmsvga_gem_pager_ctor,
	.cdev_pg_dtor	= vmsvga_gem_pager_dtor,
};

/* drm_driver.gem_free_object */
static void
vmsvga_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct vmsvga_gem_bo *bo = (struct vmsvga_gem_bo *)gem_obj;
	struct vmsvga_softc *sc = device_get_softc(gem_obj->dev->dev);
	size_t i;

	mtx_lock(&sc->bos_mtx);
	TAILQ_REMOVE(&sc->bos, bo, link);
	mtx_unlock(&sc->bos_mtx);

	if (bo->cdev_pager != NULL) {
		vm_object_deallocate(bo->cdev_pager);
		bo->cdev_pager = NULL;
	}
	if (bo->vbase != 0) {
		pmap_qremove(bo->vbase, bo->npages);
		vmem_free(kernel_arena, bo->vbase, bo->size);
	}
	if (bo->pages != NULL) {
		for (i = 0; i < bo->npages; i++) {
			if (bo->pages[i] == NULL)
				continue;
			bo->pages[i]->flags &= ~PG_FICTITIOUS;
			bo->pages[i]->oflags |= VPO_UNMANAGED;
			vm_page_unwire_noq(bo->pages[i]);
			vm_page_free(bo->pages[i]);
		}
		free(bo->pages, M_DEVBUF);
	}
	drm_gem_object_release(gem_obj);
	free(bo, M_DEVBUF);
}

/* drm_driver.dumb_create */
static int
vmsvga_dumb_create(struct drm_file *file_priv, struct drm_device *ddev,
    struct drm_mode_create_dumb *args)
{
	struct vmsvga_softc *sc = device_get_softc(ddev->dev);
	struct vmsvga_gem_bo *bo;
	struct pctrie_iter pages_iter;
	vm_page_t m;
	size_t i, size;
	int tries, error;

	args->pitch = args->width * (args->bpp / 8);
	args->size  = (uint64_t)args->pitch * args->height;
	size = round_page(args->size);
	if (size == 0)
		return (-EINVAL);

	bo = malloc(sizeof(*bo), M_DEVBUF, M_WAITOK | M_ZERO);
	bo->size   = size;
	bo->npages = atop(size);
	bo->pages  = malloc(sizeof(vm_page_t) * bo->npages, M_DEVBUF,
	    M_WAITOK | M_ZERO);

	tries = 0;
retry_alloc:
	m = vm_page_alloc_noobj_contig(VM_ALLOC_WIRED | VM_ALLOC_ZERO,
	    bo->npages, 0, ~0UL, PAGE_SIZE, 0, VM_MEMATTR_DEFAULT);
	if (m == NULL) {
		if (tries++ < 3) {
			vm_page_reclaim_contig(0, bo->npages, 0, ~0UL,
			    PAGE_SIZE, 0);
			goto retry_alloc;
		}
		free(bo->pages, M_DEVBUF);
		free(bo, M_DEVBUF);
		return (-ENOMEM);
	}
	for (i = 0; i < bo->npages; i++, m++) {
		m->valid = VM_PAGE_BITS_ALL;
		bo->pages[i] = m;
	}
	bo->pbase = VM_PAGE_TO_PHYS(bo->pages[0]);

	if (vmem_alloc(kernel_arena, size, M_WAITOK | M_BESTFIT,
	    &bo->vbase) != 0) {
		error = -ENOMEM;
		goto err_pages;
	}
	pmap_qenter(bo->vbase, bo->pages, bo->npages);

	bo->resource_id = atomic_fetchadd_32(&sc->next_resource_id, 1);

	error = drm_gem_object_init(ddev, &bo->gem_obj, size);
	if (error != 0)
		goto err_vmap;
	error = drm_gem_create_mmap_offset(&bo->gem_obj);
	if (error != 0)
		goto err_gem;

	/*
	 * Pre-allocate the cdev_pager keyed by gem_obj and insert all our
	 * pages into it.  drm_gem_mmap_single() later calls
	 * cdev_pager_allocate() with the same handle and gets THIS pager
	 * back, so userland mmap shares the exact same vm_pages we hold
	 * via bo->vbase.
	 */
	bo->cdev_pager = cdev_pager_allocate(&bo->gem_obj, OBJT_MGTDEVICE,
	    &vmsvga_gem_pager_ops, size, 0, 0, NULL);
	if (bo->cdev_pager == NULL) {
		error = -ENOMEM;
		goto err_gem;
	}
	vm_page_iter_init(&pages_iter, bo->cdev_pager);
	VM_OBJECT_WLOCK(bo->cdev_pager);
	for (i = 0; i < bo->npages; i++) {
		bo->pages[i]->oflags &= ~VPO_UNMANAGED;
		bo->pages[i]->flags  |= PG_FICTITIOUS;
		if (vm_page_iter_insert(bo->pages[i], bo->cdev_pager,
		    i, &pages_iter) != 0) {
			VM_OBJECT_WUNLOCK(bo->cdev_pager);
			error = -EINVAL;
			goto err_pager;
		}
	}
	VM_OBJECT_WUNLOCK(bo->cdev_pager);

	error = drm_gem_handle_create(file_priv, &bo->gem_obj, &args->handle);
	if (error != 0)
		goto err_pager;

	mtx_lock(&sc->bos_mtx);
	TAILQ_INSERT_TAIL(&sc->bos, bo, link);
	mtx_unlock(&sc->bos_mtx);

	drm_gem_object_unreference_unlocked(&bo->gem_obj);
	return (0);

err_pager:
	vm_object_deallocate(bo->cdev_pager);
	bo->cdev_pager = NULL;
err_gem:
	drm_gem_object_release(&bo->gem_obj);
err_vmap:
	pmap_qremove(bo->vbase, bo->npages);
	vmem_free(kernel_arena, bo->vbase, bo->size);
err_pages:
	for (i = 0; i < bo->npages; i++) {
		if (bo->pages[i] == NULL)
			continue;
		vm_page_unwire_noq(bo->pages[i]);
		vm_page_free(bo->pages[i]);
	}
	free(bo->pages, M_DEVBUF);
	free(bo, M_DEVBUF);
	return (error);
}

static int
vmsvga_dumb_map_offset(struct drm_file *file_priv, struct drm_device *ddev,
    uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem_obj;
	int error = 0;

	DRM_LOCK(ddev);
	gem_obj = drm_gem_object_lookup(ddev, file_priv, handle);
	if (gem_obj == NULL) {
		DRM_UNLOCK(ddev);
		return (-EINVAL);
	}
	error = drm_gem_create_mmap_offset(gem_obj);
	if (error == 0) {
		*offset = DRM_GEM_MAPPING_OFF(gem_obj->map_list.key) |
		    DRM_GEM_MAPPING_KEY;
	}
	drm_gem_object_unreference(gem_obj);
	DRM_UNLOCK(ddev);
	return (error);
}

static int
vmsvga_dumb_destroy(struct drm_file *file_priv, struct drm_device *ddev,
    uint32_t handle)
{
	return (drm_gem_handle_delete(file_priv, handle));
}

/* ---- framebuffer funcs (now wrap a GEM bo) ---- */

static void
vmsvga_drm_fb_destroy(struct drm_framebuffer *drm_fb)
{
	struct vmsvga_drm_framebuffer *fb =
	    (struct vmsvga_drm_framebuffer *)drm_fb;

	if (fb->bo != NULL)
		drm_gem_object_unreference_unlocked(&fb->bo->gem_obj);
	drm_framebuffer_cleanup(drm_fb);
	free(fb, M_DEVBUF);
}

static int
vmsvga_drm_fb_create_handle(struct drm_framebuffer *drm_fb,
    struct drm_file *file_priv, unsigned int *handle)
{
	struct vmsvga_drm_framebuffer *fb =
	    (struct vmsvga_drm_framebuffer *)drm_fb;

	return (drm_gem_handle_create(file_priv, &fb->bo->gem_obj, handle));
}

static const struct drm_framebuffer_funcs vmsvga_drm_fb_funcs = {
	.destroy	= vmsvga_drm_fb_destroy,
	.create_handle	= vmsvga_drm_fb_create_handle,
};

static int
vmsvga_drm_fb_create(struct drm_device *ddev, struct drm_file *file,
    struct drm_mode_fb_cmd2 *mode_cmd, struct drm_framebuffer **fb_out)
{
	struct drm_gem_object *gem_obj;
	struct vmsvga_drm_framebuffer *fb;
	int error;

	gem_obj = drm_gem_object_lookup(ddev, file, mode_cmd->handles[0]);
	if (gem_obj == NULL)
		return (-ENOENT);

	fb = malloc(sizeof(*fb), M_DEVBUF, M_WAITOK | M_ZERO);
	fb->bo = (struct vmsvga_gem_bo *)gem_obj;
	fb->base.pitches[0] = mode_cmd->pitches[0];
	fb->base.offsets[0] = mode_cmd->offsets[0];
	fb->base.width  = mode_cmd->width;
	fb->base.height = mode_cmd->height;
	fb->base.depth         = 24;
	fb->base.bits_per_pixel = 32;
	error = drm_framebuffer_init(ddev, &fb->base, &vmsvga_drm_fb_funcs);
	if (error != 0) {
		drm_gem_object_unreference_unlocked(gem_obj);
		free(fb, M_DEVBUF);
		return (error);
	}
	*fb_out = &fb->base;
	return (0);
}

static void
vmsvga_drm_output_poll_changed(struct drm_device *ddev)
{
}

static const struct drm_mode_config_funcs vmsvga_drm_mode_config_funcs = {
	.fb_create		= vmsvga_drm_fb_create,
	.output_poll_changed	= vmsvga_drm_output_poll_changed,
};

/* ---- CRTC (one for now) ---- */

static void
vmsvga_drm_crtc_dpms(struct drm_crtc *crtc, int mode)
{
}

static bool
vmsvga_drm_crtc_mode_fixup(struct drm_crtc *crtc,
    const struct drm_display_mode *mode,
    struct drm_display_mode *adjusted_mode)
{
	return (true);
}

static int
vmsvga_drm_crtc_mode_set(struct drm_crtc *crtc,
    struct drm_display_mode *mode, struct drm_display_mode *adjusted,
    int x, int y, struct drm_framebuffer *old_fb)
{
	struct vmsvga_softc *sc =
	    __containerof(crtc, struct vmsvga_softc, crtc);

	/* Mode-set side: write the SVGA registers (proven path). */
	return (vmsvga_set_mode(sc, mode->hdisplay, mode->vdisplay, 32));
}

static int
vmsvga_drm_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
    struct drm_framebuffer *old_fb)
{
	return (0);
}

static void
vmsvga_drm_crtc_prepare(struct drm_crtc *crtc) { }
static void
vmsvga_drm_crtc_commit(struct drm_crtc *crtc)  { }

static void
vmsvga_drm_crtc_gamma_set(struct drm_crtc *crtc, u16 *r, u16 *g, u16 *b,
    uint32_t start, uint32_t size)
{
}

static int
vmsvga_drm_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
    struct drm_pending_vblank_event *event)
{
	return (-ENOSYS);	/* page flips need Phase D's GMR plumbing */
}

static void
vmsvga_drm_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static const struct drm_crtc_funcs vmsvga_drm_crtc_funcs = {
	.gamma_set	= vmsvga_drm_crtc_gamma_set,
	.set_config	= drm_crtc_helper_set_config,
	.destroy	= vmsvga_drm_crtc_destroy,
	.page_flip	= vmsvga_drm_crtc_page_flip,
};

static const struct drm_crtc_helper_funcs vmsvga_drm_crtc_helper_funcs = {
	.dpms		= vmsvga_drm_crtc_dpms,
	.mode_fixup	= vmsvga_drm_crtc_mode_fixup,
	.mode_set	= vmsvga_drm_crtc_mode_set,
	.mode_set_base	= vmsvga_drm_crtc_mode_set_base,
	.prepare	= vmsvga_drm_crtc_prepare,
	.commit		= vmsvga_drm_crtc_commit,
};

/* ---- encoder ---- */

static bool
vmsvga_drm_encoder_mode_fixup(struct drm_encoder *e,
    const struct drm_display_mode *m,
    struct drm_display_mode *am)
{
	return (true);
}

static void
vmsvga_drm_encoder_mode_set(struct drm_encoder *e,
    struct drm_display_mode *m, struct drm_display_mode *am) { }

static void
vmsvga_drm_encoder_dpms(struct drm_encoder *e, int s) { }
static void
vmsvga_drm_encoder_prepare(struct drm_encoder *e)     { }
static void
vmsvga_drm_encoder_commit(struct drm_encoder *e)      { }

static void
vmsvga_drm_encoder_destroy(struct drm_encoder *e)
{
	drm_encoder_cleanup(e);
}

static const struct drm_encoder_funcs vmsvga_drm_encoder_funcs = {
	.destroy	= vmsvga_drm_encoder_destroy,
};

static const struct drm_encoder_helper_funcs
    vmsvga_drm_encoder_helper_funcs = {
	.dpms		= vmsvga_drm_encoder_dpms,
	.mode_fixup	= vmsvga_drm_encoder_mode_fixup,
	.mode_set	= vmsvga_drm_encoder_mode_set,
	.prepare	= vmsvga_drm_encoder_prepare,
	.commit		= vmsvga_drm_encoder_commit,
};

/* ---- virtual connector ---- */

static int
vmsvga_drm_connector_get_modes(struct drm_connector *conn)
{
	struct vmsvga_softc *sc =
	    __containerof(conn, struct vmsvga_softc, connector);
	struct drm_display_mode *mode;
	int count;

	count = drm_add_modes_noedid(conn, sc->max_width, sc->max_height);

	/* Mark a 1024x768 mode as preferred if the list contains one. */
	list_for_each_entry(mode, &conn->probed_modes, head) {
		if (mode->hdisplay == 1024 && mode->vdisplay == 768) {
			mode->type |= DRM_MODE_TYPE_PREFERRED;
			break;
		}
	}
	return (count);
}

static int
vmsvga_drm_connector_mode_valid(struct drm_connector *conn,
    struct drm_display_mode *mode)
{
	struct vmsvga_softc *sc =
	    __containerof(conn, struct vmsvga_softc, connector);

	if (mode->hdisplay > sc->max_width ||
	    mode->vdisplay > sc->max_height)
		return (MODE_BAD);
	return (MODE_OK);
}

static struct drm_encoder *
vmsvga_drm_connector_best_encoder(struct drm_connector *conn)
{
	struct vmsvga_softc *sc =
	    __containerof(conn, struct vmsvga_softc, connector);

	/* Single-head: encoder is right next to the connector in the softc. */
	return (&sc->encoder);
}

static enum drm_connector_status
vmsvga_drm_connector_detect(struct drm_connector *conn, bool force)
{
	return (connector_status_connected);
}

static void
vmsvga_drm_connector_destroy(struct drm_connector *conn)
{
	drm_connector_cleanup(conn);
}

static const struct drm_connector_helper_funcs
    vmsvga_drm_connector_helper_funcs = {
	.get_modes	= vmsvga_drm_connector_get_modes,
	.mode_valid	= vmsvga_drm_connector_mode_valid,
	.best_encoder	= vmsvga_drm_connector_best_encoder,
};

static const struct drm_connector_funcs vmsvga_drm_connector_funcs = {
	.dpms		= drm_helper_connector_dpms,
	.detect		= vmsvga_drm_connector_detect,
	.fill_modes	= drm_helper_probe_single_connector_modes,
	.destroy	= vmsvga_drm_connector_destroy,
};

/* ---- drm_load / drm_unload ---- */

static int
vmsvga_drm_load(struct drm_device *ddev, unsigned long flags)
{
	struct vmsvga_softc *sc = device_get_softc(ddev->dev);

	device_printf(ddev->dev, "drm_load: step A: mode_config_init\n");
	drm_mode_config_init(ddev);
	ddev->mode_config.min_width  = 0;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_width  = sc->max_width;
	ddev->mode_config.max_height = sc->max_height;
	ddev->mode_config.funcs = __DECONST(struct drm_mode_config_funcs *,
	    &vmsvga_drm_mode_config_funcs);

	device_printf(ddev->dev, "drm_load: step B: crtc_init\n");
	drm_crtc_init(ddev, &sc->crtc, &vmsvga_drm_crtc_funcs);
	drm_mode_crtc_set_gamma_size(&sc->crtc, 256);
	drm_crtc_helper_add(&sc->crtc, &vmsvga_drm_crtc_helper_funcs);

	device_printf(ddev->dev, "drm_load: step C: encoder_init\n");
	sc->encoder.possible_crtcs = 0x1;
	drm_encoder_init(ddev, &sc->encoder, &vmsvga_drm_encoder_funcs,
	    DRM_MODE_ENCODER_VIRTUAL);
	drm_encoder_helper_add(&sc->encoder,
	    &vmsvga_drm_encoder_helper_funcs);

	device_printf(ddev->dev, "drm_load: step D: connector_init\n");
	drm_connector_init(ddev, &sc->connector,
	    &vmsvga_drm_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	drm_connector_helper_add(&sc->connector,
	    &vmsvga_drm_connector_helper_funcs);
	drm_mode_connector_attach_encoder(&sc->connector, &sc->encoder);
	sc->connector.encoder = &sc->encoder;

	device_printf(ddev->dev,
	    "drm_load: done -- mode_config %ux%u..%ux%u, 1 CRTC ready\n",
	    ddev->mode_config.min_width, ddev->mode_config.min_height,
	    ddev->mode_config.max_width, ddev->mode_config.max_height);
	return (0);
}

static int
vmsvga_drm_unload(struct drm_device *ddev)
{
	drm_mode_config_cleanup(ddev);
	return (0);
}

#ifndef DRIVER_PRIME
#define DRIVER_PRIME 0
#endif

static struct drm_driver vmsvga_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME,
	.load		 = vmsvga_drm_load,
	.unload		 = vmsvga_drm_unload,
	.gem_free_object = vmsvga_gem_free_object,
	.gem_pager_ops	 = &vmsvga_gem_pager_ops,
	.dumb_create	 = vmsvga_dumb_create,
	.dumb_map_offset = vmsvga_dumb_map_offset,
	.dumb_destroy	 = vmsvga_dumb_destroy,
	.name		 = "vmsvga_drm",
	.desc		 = VMSVGA_DRM_DRIVER_DESC,
	.date		 = "20260527",
	.major		 = 0,
	.minor		 = 1,
};

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

static int
vmsvga_sysctl_apply_mode(SYSCTL_HANDLER_ARGS)
{
	struct vmsvga_softc *sc = arg1;
	int val = 0, err;

	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (val != 1)
		return (EINVAL);
	return (vmsvga_set_mode(sc, sc->want_width, sc->want_height,
	    sc->want_bpp));
}

/*
 * Phase D probe: try to negotiate a guest-allocated FIFO with the
 * host.  Classic SVGA-II expects host-provided FIFO base via
 * SVGA_REG_MEM_START -- but VBox arm64 reports MEM_START=0 even
 * though MEM_SIZE is non-zero (2 MB).  Hypothesis: the host wants
 * the guest to provide its own FIFO ring in guest RAM, in which
 * case writing a guest physaddr to MEM_START should stick.
 *
 * Sequence:
 *   1. allocate `size` bytes of physically contiguous wired pages
 *   2. write gfifo_pbase to SVGA_REG_MEM_START
 *   3. write size            to SVGA_REG_MEM_SIZE
 *   4. read SVGA_REG_MEM_START -- if it equals what we wrote, the
 *      host accepted our base
 *   5. init the FIFO header at the start of the buffer
 *   6. write SVGA_REG_CONFIG_DONE = 1
 *   7. emit SVGA_CMD_UPDATE for the full framebuffer + SYNC
 *
 * Done as a sysctl to isolate any guest-panic risk from kldload.
 */
static int
vmsvga_sysctl_fifo_probe(SYSCTL_HANDLER_ARGS)
{
	struct vmsvga_softc *sc = arg1;
	int val = 0, err, tries;
	size_t size, i;
	vm_page_t m;
	uint32_t header[SVGA_FIFO_NUM_REGS], readback;

	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (val != 1)
		return (EINVAL);
	if (sc->gfifo_ready) {
		device_printf(sc->dev, "fifo_probe: already initialised\n");
		return (EBUSY);
	}

	size = 256 * 1024;	/* small ring; keep blast radius bounded */
	sc->gfifo_npages = atop(size);
	sc->gfifo_pages  = malloc(sizeof(vm_page_t) * sc->gfifo_npages,
	    M_DEVBUF, M_WAITOK | M_ZERO);

	tries = 0;
retry_alloc:
	m = vm_page_alloc_noobj_contig(VM_ALLOC_WIRED | VM_ALLOC_ZERO,
	    sc->gfifo_npages, 0, ~0UL, PAGE_SIZE, 0, VM_MEMATTR_DEFAULT);
	if (m == NULL) {
		if (tries++ < 3) {
			vm_page_reclaim_contig(0, sc->gfifo_npages, 0, ~0UL,
			    PAGE_SIZE, 0);
			goto retry_alloc;
		}
		free(sc->gfifo_pages, M_DEVBUF);
		sc->gfifo_pages = NULL;
		return (ENOMEM);
	}
	for (i = 0; i < sc->gfifo_npages; i++, m++) {
		m->valid = VM_PAGE_BITS_ALL;
		sc->gfifo_pages[i] = m;
	}
	sc->gfifo_pbase = VM_PAGE_TO_PHYS(sc->gfifo_pages[0]);
	sc->gfifo_size  = size;

	if (vmem_alloc(kernel_arena, size, M_WAITOK | M_BESTFIT,
	    &sc->gfifo_vbase) != 0) {
		device_printf(sc->dev, "fifo_probe: vmem_alloc failed\n");
		for (i = 0; i < sc->gfifo_npages; i++) {
			vm_page_unwire_noq(sc->gfifo_pages[i]);
			vm_page_free(sc->gfifo_pages[i]);
		}
		free(sc->gfifo_pages, M_DEVBUF);
		sc->gfifo_pages = NULL;
		return (ENOMEM);
	}
	pmap_qenter(sc->gfifo_vbase, sc->gfifo_pages, sc->gfifo_npages);
	bzero((void *)sc->gfifo_vbase, size);

	device_printf(sc->dev,
	    "fifo_probe: allocated %zu bytes guest RAM at pa=0x%jx va=0x%jx\n",
	    size, (uintmax_t)sc->gfifo_pbase, (uintmax_t)sc->gfifo_vbase);

	/* Tell the host where our FIFO lives. */
	device_printf(sc->dev,
	    "fifo_probe: writing MEM_START + MEM_SIZE\n");
	vmsvga_write_reg(sc, SVGA_REG_MEM_START, (uint32_t)sc->gfifo_pbase);
	vmsvga_write_reg(sc, SVGA_REG_MEM_SIZE,  (uint32_t)size);
	readback = vmsvga_read_reg(sc, SVGA_REG_MEM_START);
	device_printf(sc->dev,
	    "fifo_probe: MEM_START readback = 0x%08x (wrote 0x%08x)\n",
	    readback, (uint32_t)sc->gfifo_pbase);
	if (readback != (uint32_t)sc->gfifo_pbase) {
		device_printf(sc->dev,
		    "fifo_probe: host rejected guest-allocated FIFO\n");
		return (ENOTSUP);
	}

	/* Initialise the FIFO header in our own buffer. */
	header[SVGA_FIFO_MIN]      = SVGA_FIFO_NUM_REGS * 4;
	header[SVGA_FIFO_MAX]      = size;
	header[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_NUM_REGS * 4;
	header[SVGA_FIFO_STOP]     = SVGA_FIFO_NUM_REGS * 4;
	memcpy((void *)sc->gfifo_vbase, header, sizeof(header));

	vmsvga_write_reg(sc, SVGA_REG_CONFIG_DONE, 1);
	sc->gfifo_ready = true;

	/*
	 * Emit a single SVGA_CMD_UPDATE for the entire current scanout
	 * directly into the guest FIFO buffer.  Bump NEXT_CMD by 5
	 * dwords, write SYNC, read BUSY -- if the host actually
	 * consumes this and runs the command, the FIFO works.
	 */
	{
		uint32_t *ring = (uint32_t *)sc->gfifo_vbase;
		uint32_t next  = SVGA_FIFO_NUM_REGS;
		ring[next + 0] = SVGA_CMD_UPDATE;
		ring[next + 1] = 0;
		ring[next + 2] = 0;
		ring[next + 3] = sc->cur_width  ? sc->cur_width  : 1;
		ring[next + 4] = sc->cur_height ? sc->cur_height : 1;
		ring[SVGA_FIFO_NEXT_CMD] = (next + 5) * 4;
		vmsvga_write_reg(sc, SVGA_REG_SYNC, 1);
		(void)vmsvga_read_reg(sc, SVGA_REG_BUSY);
	}

	device_printf(sc->dev,
	    "fifo_probe: ACCEPTED.  FIFO header in guest RAM, "
	    "CONFIG_DONE=1, one UPDATE emitted (%ux%u)\n",
	    sc->cur_width, sc->cur_height);
	return (0);
}

static int
vmsvga_sysctl_diag(SYSCTL_HANDLER_ARGS)
{
	struct vmsvga_softc *sc = arg1;
	int val = 0, err;

	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (val != 1)
		return (EINVAL);

	device_printf(sc->dev,
	    "diag: reg_res bushandle=0x%jx virtual=%p type=%s\n",
	    (uintmax_t)rman_get_bushandle(sc->reg_res),
	    rman_get_virtual(sc->reg_res),
	    sc->reg_type == SYS_RES_IOPORT ? "ioport" : "memory");
	device_printf(sc->dev,
	    "diag: fb_res  bushandle=0x%jx virtual=%p start=0x%jx "
	    "size=%ju\n",
	    (uintmax_t)rman_get_bushandle(sc->fb_res),
	    rman_get_virtual(sc->fb_res),
	    (uintmax_t)rman_get_start(sc->fb_res),
	    (uintmax_t)rman_get_size(sc->fb_res));
	if (sc->fifo_res != NULL) {
		device_printf(sc->dev,
		    "diag: fifo_res bushandle=0x%jx virtual=%p size=%ju\n",
		    (uintmax_t)rman_get_bushandle(sc->fifo_res),
		    rman_get_virtual(sc->fifo_res),
		    (uintmax_t)rman_get_size(sc->fifo_res));
	}
	device_printf(sc->dev,
	    "diag: SVGA_REG_FB_START=0x%08x FB_OFFSET=0x%08x "
	    "FB_SIZE=%u BYTES_PER_LINE=%u\n",
	    vmsvga_read_reg(sc, SVGA_REG_FB_START),
	    vmsvga_read_reg(sc, SVGA_REG_FB_OFFSET),
	    vmsvga_read_reg(sc, SVGA_REG_FB_SIZE),
	    vmsvga_read_reg(sc, SVGA_REG_BYTES_PER_LINE));
	device_printf(sc->dev,
	    "diag: SVGA_REG_VRAM_SIZE=%u MEM_START=0x%08x MEM_SIZE=%u "
	    "ENABLE=%u BUSY=%u\n",
	    vmsvga_read_reg(sc, SVGA_REG_VRAM_SIZE),
	    vmsvga_read_reg(sc, SVGA_REG_MEM_START),
	    vmsvga_read_reg(sc, SVGA_REG_MEM_SIZE),
	    vmsvga_read_reg(sc, SVGA_REG_ENABLE),
	    vmsvga_read_reg(sc, SVGA_REG_BUSY));
	return (0);
}

/*
 * NOTE: test_update was removed in Phase B v3.  Direct FB BAR
 * writes via bus_space_write_4 panic the guest on VBox arm64
 * even though diag() shows the BAR is kva-mapped with a sane
 * bushandle (0xffff0000aa600000) and the host reports
 * SVGA_REG_FB_START=0x88000000 / FB_SIZE=3MB / ENABLE=1.
 *
 * Hypothesis: VBox arm64's SVGA-II emulation does not back the
 * VRAM BAR with writable guest memory.  Reads "work" only because
 * any read returns 0; writes trigger a hypervisor decode that
 * cannot resolve to anything and takes the guest down.
 *
 * Path forward (Phase C): allocate dumb buffers in guest RAM and
 * tell the host where they live via SVGA_CMD_DEFINE_SCREEN /
 * Screen Object 2 (caps bit 0x800000 IS set on this host).  This
 * mirrors how modern vmwgfx works and sidesteps the FB BAR
 * entirely.
 */

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

	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "fifo_pa",
	    CTLFLAG_RD, &sc->fifo_pa, 0,
	    "FIFO physical base (SVGA_REG_MEM_START)");
	SYSCTL_ADD_OPAQUE(ctx, children, OID_AUTO, "fifo_size",
	    CTLFLAG_RD, &sc->fifo_size, sizeof(sc->fifo_size), "LU",
	    "FIFO size in bytes (SVGA_REG_MEM_SIZE)");

	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "cur_width",
	    CTLFLAG_RD, &sc->cur_width, 0, "Currently programmed width");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "cur_height",
	    CTLFLAG_RD, &sc->cur_height, 0, "Currently programmed height");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "cur_bpp",
	    CTLFLAG_RD, &sc->cur_bpp, 0, "Currently programmed bpp");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "cur_bytes_per_line",
	    CTLFLAG_RD, &sc->cur_bytes_per_line, 0, "Stride in bytes");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "cur_enable",
	    CTLFLAG_RD, &sc->cur_enable, 0, "Scanout enabled");

	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "want_width",
	    CTLFLAG_RW, &sc->want_width, 0,
	    "Requested width for next apply_mode");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "want_height",
	    CTLFLAG_RW, &sc->want_height, 0,
	    "Requested height for next apply_mode");
	SYSCTL_ADD_UINT(ctx, children, OID_AUTO, "want_bpp",
	    CTLFLAG_RW, &sc->want_bpp, 0,
	    "Requested bpp for next apply_mode");

	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "apply_mode",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    vmsvga_sysctl_apply_mode, "I",
	    "Write 1 to apply want_width x want_height @ want_bpp");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "diag",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    vmsvga_sysctl_diag, "I",
	    "Write 1 to dump BAR mappings + live SVGA reg state");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "fifo_probe",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    vmsvga_sysctl_fifo_probe, "I",
	    "Phase D probe: allocate guest RAM, write physaddr to "
	    "SVGA_REG_MEM_START, init FIFO header, set CONFIG_DONE=1, "
	    "emit one SVGA_CMD_UPDATE.  Gated -- panic risk if host "
	    "rejects guest-allocated FIFO.");
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

	TAILQ_INIT(&sc->bos);
	mtx_init(&sc->bos_mtx, "vmsvga_bos", NULL, MTX_DEF);
	sc->next_resource_id = 1;

	sc->want_width  = 1024;
	sc->want_height = 768;
	sc->want_bpp    = 32;

	(void)vmsvga_fifo_init(sc);	/* not fatal -- some hosts gate */

	vmsvga_sysctl_setup(sc);

	/*
	 * Phase C: register the DRM device.
	 *
	 * Critical detail: use drm_get_platform_dev (not _pci_dev) even
	 * though we attach under vgapci -- drm_get_pci_dev panicked the
	 * guest immediately, likely because it expects dev->dev to be
	 * a direct PCI device, not a vgapci child.  drm_get_platform_dev
	 * just trusts dev->dev and lets drm core fill in the rest.
	 */
	{
		struct drm_device *ddev;
		int derr;

		ddev = malloc(sizeof(*ddev), M_DEVBUF, M_WAITOK | M_ZERO);
		derr = drm_get_platform_dev(dev, ddev, &vmsvga_drm_driver);
		if (derr == 0) {
			sc->drm_dev = ddev;
			device_printf(dev,
			    "drm: registered /dev/dri/card%d\n",
			    ddev->primary ? ddev->primary->index : 0);
		} else {
			free(ddev, M_DEVBUF);
			device_printf(dev,
			    "drm: drm_get_platform_dev failed: %d "
			    "(KERNCONF must include 'device drm2')\n",
			    derr);
		}
	}

	device_printf(dev,
	    "SVGA-II id=0x%08x caps=0x%08x max=%ux%u host_bpp=%u "
	    "vram=%u bytes (reg BAR=%s, fb_pa=0x%jx, fifo=%s)\n",
	    sc->svga_id, sc->capabilities,
	    sc->max_width, sc->max_height, sc->host_bpp,
	    sc->vram_size,
	    sc->reg_type == SYS_RES_IOPORT ? "ioport" : "memory",
	    (uintmax_t)sc->fb_pa,
	    sc->fifo_ready ? "ready" : "unavailable");

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
	if (sc->drm_dev != NULL) {
		drm_put_dev(sc->drm_dev);
		sc->drm_dev = NULL;
	}
	mtx_destroy(&sc->bos_mtx);
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
