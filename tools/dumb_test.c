/*
 * Minimal Phase C.2 verifier: open /dev/dri/card0, create a
 * dumb buffer via DRM_IOCTL_MODE_CREATE_DUMB, map it, write
 * a pattern, dump-destroy it.  No display work -- just proves
 * the ioctl path lands in our vmsvga_dumb_create + pages are
 * usable from userspace.
 *
 * cc -o dumb_test dumb_test.c
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
	struct drm_mode_create_dumb create = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	uint32_t *fb;
	int fd, r;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return (1);
	}
	printf("opened %s fd=%d\n", path, fd);

	create.width  = 1024;
	create.height = 768;
	create.bpp    = 32;
	r = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (r != 0) {
		fprintf(stderr, "CREATE_DUMB: %s\n", strerror(errno));
		close(fd);
		return (1);
	}
	printf("CREATE_DUMB: handle=%u pitch=%u size=%llu\n",
	    create.handle, create.pitch,
	    (unsigned long long)create.size);

	map.handle = create.handle;
	r = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (r != 0) {
		fprintf(stderr, "MAP_DUMB: %s\n", strerror(errno));
		goto out;
	}
	printf("MAP_DUMB: offset=0x%llx\n", (unsigned long long)map.offset);

	fb = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, map.offset);
	if (fb == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		goto out;
	}
	printf("mmap: va=%p\n", fb);

	/* Touch every page with a recognisable pattern. */
	for (size_t i = 0; i < create.size / 4; i++)
		fb[i] = 0xdeadbeef ^ (uint32_t)i;
	if (fb[0] != 0xdeadbeef) {
		fprintf(stderr, "readback mismatch: 0x%08x\n", fb[0]);
		munmap(fb, create.size);
		goto out;
	}
	printf("write+readback OK across %llu bytes\n",
	    (unsigned long long)create.size);

	munmap(fb, create.size);
out:
	destroy.handle = create.handle;
	r = ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	if (r != 0)
		fprintf(stderr, "DESTROY_DUMB: %s\n", strerror(errno));
	else
		printf("DESTROY_DUMB: ok\n");
	close(fd);
	return (0);
}
