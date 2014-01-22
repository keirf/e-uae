
#include "sysconfig.h"
#include "sysdeps.h"
#include <stdint.h>
#include <dlfcn.h>
#include "err.h"
#include "libdisk.h"
#include <arpa/inet.h>

#include <libdisk/disk.h>

struct disk;

static struct drive {
    struct disk *disk;
    struct track_raw *track_raw;
    unsigned int saved_track;
} drive[4];

static struct disklib {
    int warned;
    unsigned int ref;
    void *handle;
    struct disk *(*disk_open)(const char *name, int read_only);
    void (*disk_close)(struct disk *);
    struct track_raw *(*track_alloc_raw_buffer)(struct disk *d);
    void (*track_free_raw_buffer)(struct track_raw *);
    void (*track_purge_raw_buffer)(struct track_raw *);
    void (*track_read_raw)(struct track_raw *, unsigned int tracknr);
    int (*track_write_raw)(
        struct track_raw *, unsigned int tracknr, enum track_type);
} disklib;

#define DISKLIB_NAME    "libdisk.so.0"

static int load_libdisk(void)
{
    if (disklib.ref++)
        return 1;

    if ((disklib.handle = dlopen(DISKLIB_NAME, RTLD_LAZY)) == NULL)
        goto fail_no_handle;

#define GETSYM(sym)                             \
    disklib.sym = dlsym(disklib.handle, #sym);  \
    if (dlerror() != 0) goto fail;
    GETSYM(disk_open);
    GETSYM(disk_close);
    GETSYM(track_alloc_raw_buffer);
    GETSYM(track_free_raw_buffer);
    GETSYM(track_purge_raw_buffer);
    GETSYM(track_read_raw);
    GETSYM(track_write_raw);
#undef GETSYM
    return 1;

fail:
    dlclose(disklib.handle);
fail_no_handle:
    warnx("Unable to open " DISKLIB_NAME);
    --disklib.ref;
    if (!disklib.warned) {
        disklib.warned = 1;
        gui_message ("This disk image needs the libdisk plugin\n"
                     "which is available from\n"
                     "https://github.org/keirf/Amiga-Disk-Utilities\n");
    }
    return 0;
}

static void put_libdisk(void)
{
    if (--disklib.ref)
        return;
    dlclose(disklib.handle);
}

int libdisk_open(const char *name, unsigned int drv)
{
    const char *p = strrchr(name, '.');

    if (!p || strcmp(p, ".dsk"))
        return 0;
    if (!load_libdisk())
        return 0;
    if (drive[drv].disk)
        disklib.disk_close(drive[drv].disk);
    drive[drv].disk = disklib.disk_open(name, 1);
    if (!drive[drv].disk) {
        put_libdisk();
        return 0;
    }
    drive[drv].track_raw = disklib.track_alloc_raw_buffer(drive[drv].disk);
    if (!drive[drv].track_raw) {
        disklib.disk_close(drive[drv].disk);
        put_libdisk();
        drive[drv].disk = NULL;
        return 0;
    }
    return 1;
}

void libdisk_close(unsigned int drv)
{
    if (drive[drv].disk) {
        disklib.track_free_raw_buffer(drive[drv].track_raw);
        disklib.disk_close(drive[drv].disk);
        put_libdisk();
    }
    drive[drv].disk = NULL;
}

static void getrev(unsigned int drv, uae_u16 *mfmbuf, uae_u16 *tracktiming,
                   unsigned int *tracklength)
{
    struct drive *d = &drive[drv];
    unsigned int i;

    disklib.track_read_raw(d->track_raw, d->saved_track);
    *tracklength = d->track_raw->bitlen;

    memcpy(mfmbuf, d->track_raw->bits, (d->track_raw->bitlen+7)/8);
    for (i = 0; i < (((d->track_raw->bitlen+7)/8)+1)/2; i++)
        mfmbuf[i] = ntohs(mfmbuf[i]);

    memcpy(tracktiming, d->track_raw->speed, 2*((d->track_raw->bitlen+7)/8));
}

int libdisk_loadtrack(
    uae_u16 *mfmbuf, uae_u16 *tracktiming, unsigned int drv,
    unsigned int track, unsigned int *tracklength, int *multirev,
    unsigned int *gapoffset)
{
    drive[drv].saved_track = track;

    *multirev = 0;
    *gapoffset = -1;

    getrev(drv, mfmbuf, tracktiming, tracklength);
    return 1;
}

int libdisk_loadrevolution(
    uae_u16 *mfmbuf, unsigned int drv, uae_u16 *tracktiming,
    unsigned int *tracklength)
{
    getrev(drv, mfmbuf, tracktiming, tracklength);
    return 1;
}
