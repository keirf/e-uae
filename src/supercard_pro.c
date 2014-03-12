
#include "sysconfig.h"
#include "sysdeps.h"
#include <stdint.h>
#include <dlfcn.h>
#include "err.h"
#include "supercard_pro.h"
#include <arpa/inet.h>

#define MAX_REVS 5

enum pll_mode {
    PLL_fixed_clock, /* Fixed clock, snap phase to flux transitions. */
    PLL_variable_clock, /* Variable clock, snap phase to flux transitions. */
    PLL_authentic /* Variable clock, do not snap phase to flux transition. */
};

static struct drive {
    int fd;

    /* Current track number. */
    unsigned int track;

    /* Raw track data. */
    uint16_t *dat;
    unsigned int datsz;

    unsigned int revs;       /* stored disk revolutions */
    unsigned int dat_idx;    /* current index into dat[] */
    unsigned int index_pos;  /* next index offset */
    unsigned int nr_index;

    unsigned int index_off[MAX_REVS]; /* data offsets of each index */

    /* Accumulated read latency in nanosecs. */
    uint64_t latency;

    /* Flux-based streams: Authentic emulation of FDC PLL behaviour? */
    enum pll_mode pll_mode;

    /* Flux-based streams. */
    int flux;                /* Nanoseconds to next flux reversal */
    int clock, clock_centre; /* Clock base value in nanoseconds */
    unsigned int clocked_zeros;
} drive[4];

#define CLOCK_CENTRE  2000   /* 2000ns = 2us */
#define CLOCK_MAX_ADJ 10     /* +/- 10% adjustment */
#define CLOCK_MIN(_c) (((_c) * (100 - CLOCK_MAX_ADJ)) / 100)
#define CLOCK_MAX(_c) (((_c) * (100 + CLOCK_MAX_ADJ)) / 100)

#define SCK_NS_PER_TICK (25u)

#define min(x,y) ({                             \
    const typeof(x) _x = (x);                   \
    const typeof(y) _y = (y);                   \
    (void) (&_x == &_y);                        \
    _x < _y ? _x : _y; })

#define max(x,y) ({                             \
    const typeof(x) _x = (x);                   \
    const typeof(y) _y = (y);                   \
    (void) (&_x == &_y);                        \
    _x > _y ? _x : _y; })

static void *memalloc(size_t size)
{
    void *p = malloc(size?:1);
    if (p == NULL)
        err(1, NULL);
    memset(p, 0, size);
    return p;
}

static void memfree(void *p)
{
    free(p);
}

static void read_exact(int fd, void *buf, size_t count)
{
    ssize_t done;
    char *_buf = buf;

    while (count > 0) {
        done = read(fd, _buf, count);
        if (done < 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
                continue;
            err(1, NULL);
        }
        if (done == 0) {
            memset(_buf, 0, count);
            done = count;
        }
        count -= done;
        _buf += done;
    }
}

static void write_exact(int fd, const void *buf, size_t count)
{
    ssize_t done;
    const char *_buf = buf;

    while (count > 0) {
        done = write(fd, _buf, count);
        if (done < 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
                continue;
            err(1, NULL);
        }
        count -= done;
        _buf += done;
    }
}

int scp_open(const char *name, unsigned int drv)
{
    struct drive *d = &drive[drv];
    struct stat sbuf;
    struct scp_stream *scss;
    char header[0x10];
    char *p;

    p = strrchr(name, '.');
    if (!p || strcmp(p, ".scp"))
        return 0;

    if (stat(name, &sbuf) < 0)
        return 0;

    scp_close(drv);

    if ((d->fd = open(name, O_RDONLY)) == -1) {
        warn("%s", name);
        return 0;
    }

    read_exact(d->fd, header, sizeof(header));

    if (memcmp(header, "SCP", 3) != 0) {
        warnx("%s is not a SCP file!", name);
        close(d->fd);
        return 0;
    }

    if (header[5] == 0) {
        warnx("%s has an invalid revolution count (%u)!", name, header[5]);
        close(d->fd);
        return 0;
    }

    if (header[9] != 0 && header[9] != 16) {
        warnx("%s has unsupported bit cell time width (%u)", name, header[9]);
        close(d->fd);
        return 0;
    }

    d->revs = header[5];
    if (d->revs > MAX_REVS)
        d->revs = MAX_REVS;

    return 1;
}

void scp_close(unsigned int drv)
{
    struct drive *d = &drive[drv];
    if (!d->revs)
        return;
    close(d->fd);
    memfree(d->dat);
    memset(d, 0, sizeof(*d));
}

int scp_loadtrack(
    uae_u16 *mfmbuf, uae_u16 *tracktiming, unsigned int drv,
    unsigned int track, unsigned int *tracklength, int *multirev,
    unsigned int *gapoffset)
{
    struct drive *d = &drive[drv];
    uint8_t trk_header[4];
    uint32_t longwords[3];
    unsigned int rev, trkoffset[MAX_REVS];
    uint32_t hdr_offset, tdh_offset;

    *multirev = (d->revs != 1);
    *gapoffset = -1;

    memfree(d->dat);
    d->dat = NULL;
    d->datsz = 0;
    
    hdr_offset = 0x10 + track*sizeof(uint32_t);

    if (lseek(d->fd, hdr_offset, SEEK_SET) != hdr_offset)
        return 0;

    read_exact(d->fd, longwords, sizeof(uint32_t));
    tdh_offset = le32toh(longwords[0]);

    if (lseek(d->fd, tdh_offset, SEEK_SET) != tdh_offset)
        return 0;

    read_exact(d->fd, trk_header, sizeof(trk_header));
    if (memcmp(trk_header, "TRK", 3) != 0)
        return 0;

    if (trk_header[3] != track)
        return 0;

    for (rev = 0 ; rev < d->revs ; rev++) {
        read_exact(d->fd, longwords, sizeof(longwords));
        trkoffset[rev] = tdh_offset + le32toh(longwords[2]);
        d->index_off[rev] = le32toh(longwords[1]);
        d->datsz += d->index_off[rev];
    }

    d->dat = memalloc(d->datsz * sizeof(d->dat[0]));
    d->datsz = 0;

    for (rev = 0 ; rev < d->revs ; rev++) {
        if (lseek(d->fd, trkoffset[rev], SEEK_SET) != trkoffset[rev])
            return -1;
        read_exact(d->fd, &d->dat[d->datsz],
                   d->index_off[rev] * sizeof(d->dat[0]));
        d->datsz += d->index_off[rev];
        d->index_off[rev] = d->datsz;
    }

    d->track = track;
    d->pll_mode = PLL_authentic;
    d->dat_idx = 0;
    d->index_pos = d->index_off[0];
    d->clock = d->clock_centre = CLOCK_CENTRE;
    d->nr_index = 0;
    d->flux = 0;
    d->clocked_zeros = 0;

    scp_loadrevolution(mfmbuf, drv, tracktiming, tracklength);
    return 1;
}

static int scp_next_flux(struct drive *d)
{
    uint32_t val = 0, flux, t;

    for (;;) {
        if (d->dat_idx >= d->index_pos) {
            uint32_t rev = d->nr_index++ % d->revs;
            d->index_pos = d->index_off[rev];
            d->dat_idx = rev ? d->index_off[rev-1] : 0;
            return -1;
        }

        t = be16toh(d->dat[d->dat_idx++]);

        if (t == 0) { /* overflow */
            val += 0x10000;
            continue;
        }

        val += t;
        break;
    }

    flux = val * SCK_NS_PER_TICK;
    return (int)flux;
}

static int flux_next_bit(struct drive *d)
{
    int new_flux;

    while (d->flux < (d->clock/2)) {
        if ((new_flux = scp_next_flux(d)) == -1) {
            d->flux = 0;
            d->clocked_zeros = 0;
            d->clock = d->clock_centre;
            return -1;
        }
        d->flux += new_flux;
        d->clocked_zeros = 0;
    }

    d->latency += d->clock;
    d->flux -= d->clock;

    if (d->flux >= (d->clock/2)) {
        d->clocked_zeros++;
        return 0;
    }

    if (d->pll_mode != PLL_fixed_clock) {
        /* PLL: Adjust clock frequency according to phase mismatch. */
        if ((d->clocked_zeros >= 1) && (d->clocked_zeros <= 3)) {
            /* In sync: adjust base clock by 10% of phase mismatch. */
            int diff = d->flux / (int)(d->clocked_zeros + 1);
            d->clock += diff / 10;
        } else {
            /* Out of sync: adjust base clock towards centre. */
            d->clock += (d->clock_centre - d->clock) / 10;
        }

        /* Clamp the clock's adjustment range. */
        d->clock = max(CLOCK_MIN(d->clock_centre),
                          min(CLOCK_MAX(d->clock_centre), d->clock));
    } else {
        d->clock = d->clock_centre;
    }

    /* Authentic PLL: Do not snap the timing window to each flux transition. */
    new_flux = (d->pll_mode == PLL_authentic) ? d->flux / 2 : 0;
    d->latency += d->flux - new_flux;
    d->flux = new_flux;

    return 1;
}

void scp_loadrevolution(
    uae_u16 *mfmbuf, unsigned int drv, uae_u16 *tracktiming,
    unsigned int *tracklength)
{
    struct drive *d = &drive[drv];
    unsigned int i;
    int b;

    d->latency = 0;
    for (i = 0; (b = flux_next_bit(d)) != -1; i++) {
        if ((i & 15) == 0)
            mfmbuf[i>>4] = 0;
        if (b)
            mfmbuf[i>>4] |= 0x8000u >> (i&15);
        if ((i & 7) == 7) {
            tracktiming[i>>3] = d->latency/16u;
            d->latency = 0;
        }
    }

    if (i & 7)
        tracktiming[i>>3] = d->latency/(unsigned int)(2*(1+(i&7)));

    *tracklength = i;
}
