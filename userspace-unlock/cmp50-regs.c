/* cmp50-regs — read (and optionally poke) the CMP 50HX speed-lock registers
 * from Linux userspace.
 *
 * Two ways in, because a Secure Boot host runs the kernel in integrity
 * lockdown and that blocks the obvious one:
 *
 *   resource0   /sys/bus/pci/devices/<bdf>/resource0 — mmap is refused by
 *               lockdown ("direct PCI access is restricted").
 *   uio         bind the card to uio_pci_generic, then /dev/uioN. The uio
 *               driver hands out BARs to userspace without going through the
 *               pci-sysfs mmap path, so it works under lockdown and needs no
 *               unsigned module.
 *
 * The registers are the same ones the UEFI unlock pokes; reading them is
 * harmless. --direct-probe tests whether plain host writes stick, which
 * would make the whole SEC2/booter chain unnecessary.
 *
 *   sudo ./cmp50-regs --uio 0 --uio 1        # dump, no writes
 *   sudo ./cmp50-regs --uio 0 --direct-probe # try the three-write unlock
 *   sudo ./cmp50-regs --bdf 0000:05:00.0     # old path (needs no lockdown)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* TU102 (CMP 50HX) control surfaces, from efi-unlock/unlock50x_v1.c */
#define REG_FEAT_OVR_PLM      0x00409650u
#define REG_FEAT_OVR_SM_SPD   0x00409664u
#define REG_FEAT_OVR_SM_SPD_1 0x0040966Cu
#define REG_PFB_MMU_WPR2_LO   0x001FA824u
#define REG_PFB_MMU_WPR2_HI   0x001FA828u
#define REG_GFW_BOOT_OK       0x00118234u

#define VAL_PLM_OPEN      0xFFFFFFFFu
#define VAL_SS0_UNLOCKED  0x88888888u
#define VAL_SS1_UNLOCKED  0x00000008u

#define BAR0_MIN (16u << 20)   /* the offsets above all live in BAR0 */

#define MAX_CARDS 8

struct card {
    char label[64];
    volatile uint8_t *bar0;
    size_t bar0_len;
};

/* Map BAR0 through /sys/.../resource0. Fails with EPERM under lockdown. */
static int open_resource0(struct card *c, const char *bdf)
{
    char path[128];
    struct stat st;
    int fd;

    snprintf(c->label, sizeof(c->label), "%s (resource0)", bdf);
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    c->bar0_len = (size_t)st.st_size;
    c->bar0 = mmap(NULL, c->bar0_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return c->bar0 == MAP_FAILED ? -1 : 0;
}

/* Map BAR0 through /dev/uioN. The size comes from map0 (BAR0) in sysfs. */
static int open_uio(struct card *c, const char *dev)
{
    char path[160];
    long size = 0;
    FILE *f;
    int fd, n;
    const char *name;

    name = strrchr(dev, '/');
    name = name ? name + 1 : dev;              /* uio0 */
    n = atoi(name + 3);                        /* "0" from "uio0" */

    snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map0/size", n);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s: %s: %s\n", dev, path, strerror(errno));
        return -1;
    }
    if (fscanf(f, "%lx", &size) != 1 || size <= 0) {
        fclose(f);
        fprintf(stderr, "%s: cannot read map0 size\n", dev);
        return -1;
    }
    fclose(f);

    snprintf(c->label, sizeof(c->label), "%s (BAR0 %ld KiB)", dev, size / 1024);
    c->bar0_len = (size_t)size;
    fd = open(dev, O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;
    c->bar0 = mmap(NULL, c->bar0_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return c->bar0 == MAP_FAILED ? -1 : 0;
}

static void card_close(struct card *c)
{
    if (c->bar0 && c->bar0 != MAP_FAILED)
        munmap((void *)c->bar0, c->bar0_len);
    c->bar0 = NULL;
}

static uint32_t rd(const struct card *c, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(c->bar0 + off);
    __sync_synchronize();
    return v;
}

static void wr(const struct card *c, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(c->bar0 + off) = val;
    __sync_synchronize();
}

static int is_unlocked(const struct card *c)
{
    return rd(c, REG_FEAT_OVR_SM_SPD) == VAL_SS0_UNLOCKED &&
           rd(c, REG_FEAT_OVR_SM_SPD_1) == VAL_SS1_UNLOCKED;
}

static void dump(const struct card *c)
{
    uint32_t plm = rd(c, REG_FEAT_OVR_PLM);
    uint32_t ss0 = rd(c, REG_FEAT_OVR_SM_SPD);
    uint32_t ss1 = rd(c, REG_FEAT_OVR_SM_SPD_1);

    printf("%s\n", c->label);
    printf("  PLM  [0x%06x] = 0x%08x%s\n", REG_FEAT_OVR_PLM, plm,
           plm == VAL_PLM_OPEN ? "  (OPEN)" : "");
    printf("  SS0  [0x%06x] = 0x%08x%s\n", REG_FEAT_OVR_SM_SPD, ss0,
           ss0 == VAL_SS0_UNLOCKED ? "  (UNLOCKED)" : "");
    printf("  SS1  [0x%06x] = 0x%08x%s\n", REG_FEAT_OVR_SM_SPD_1, ss1,
           ss1 == VAL_SS1_UNLOCKED ? "  (UNLOCKED)" : "");
    printf("  WPR2 lo/hi        = 0x%08x / 0x%08x\n",
           rd(c, REG_PFB_MMU_WPR2_LO), rd(c, REG_PFB_MMU_WPR2_HI));
    printf("  GFW_BOOT_OK       = 0x%08x%s\n", rd(c, REG_GFW_BOOT_OK),
           rd(c, REG_GFW_BOOT_OK) == 0xFF ? "  (GFW booted)" : "");
}

/* Same sequence as direct_write_probe() in the EFI unlock. Returns 1 when the
 * card took the writes (unlocked without any booter), 0 when write-locked. */
static int direct_probe(struct card *c)
{
    uint32_t v;

    printf("-- direct FEAT_OVR write probe on %s --\n", c->label);

    wr(c, REG_FEAT_OVR_PLM, VAL_PLM_OPEN);
    v = rd(c, REG_FEAT_OVR_PLM);
    printf("  PLM = 0x%08x after writing 0x%08x%s\n", v, VAL_PLM_OPEN,
           v == VAL_PLM_OPEN ? "  <- accepted" : "  <- ignored (write-locked)");

    wr(c, REG_FEAT_OVR_SM_SPD, VAL_SS0_UNLOCKED);
    wr(c, REG_FEAT_OVR_SM_SPD_1, VAL_SS1_UNLOCKED);
    printf("  SS0 = 0x%08x, SS1 = 0x%08x after writing 0x%08x/0x%08x\n",
           rd(c, REG_FEAT_OVR_SM_SPD), rd(c, REG_FEAT_OVR_SM_SPD_1),
           VAL_SS0_UNLOCKED, VAL_SS1_UNLOCKED);

    if (is_unlocked(c)) {
        printf("  *** UNLOCKED by direct MMIO writes ***\n");
        return 1;
    }

    /* A locked register answers with a fixed pattern no matter what was
     * written, which is what tells "ignored" from "wired". */
    printf("  diagnostics:\n");
    wr(c, REG_FEAT_OVR_PLM, 0x00000000);
    printf("    PLM = 0x%08x after 0x00000000\n", rd(c, REG_FEAT_OVR_PLM));
    wr(c, REG_FEAT_OVR_PLM, 0xFFFFFFFE);
    printf("    PLM = 0x%08x after 0xFFFFFFFE\n", rd(c, REG_FEAT_OVR_PLM));
    wr(c, REG_FEAT_OVR_SM_SPD, 0x11111111);
    printf("    SS0 = 0x%08x after 0x11111111\n", rd(c, REG_FEAT_OVR_SM_SPD));
    wr(c, REG_FEAT_OVR_SM_SPD_1, 0x00000000);
    printf("    SS1 = 0x%08x after 0x00000000\n", rd(c, REG_FEAT_OVR_SM_SPD_1));
    return 0;
}

static void usage(void)
{
    printf("usage: cmp50-regs [--bdf BDF | --uio N | --dev PATH]... [--direct-probe]\n"
           "  --bdf BDF   map /sys/bus/pci/devices/BDF/resource0 (needs lockdown off)\n"
           "  --uio N     map /dev/uioN map0 (uio_pci_generic; works under lockdown)\n"
           "  --dev PATH  any mmap-able BAR0 file (/dev/uioN or resource0)\n"
           "  --direct-probe  attempt the three-write unlock\n"
           "  no target: --uio 0 and --uio 1\n");
}

int main(int argc, char **argv)
{
    struct card cards[MAX_CARDS];
    char what[MAX_CARDS][24];       /* "bdf", "uio", "dev" */
    char arg[MAX_CARDS][64];
    int n = 0, probe = 0, rc = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--direct-probe") == 0) {
            probe = 1;
            continue;
        }
        if ((strcmp(argv[i], "--bdf") == 0 || strcmp(argv[i], "--uio") == 0 ||
             strcmp(argv[i], "--dev") == 0) && i + 1 < argc) {
            if (n < MAX_CARDS) {
                snprintf(what[n], sizeof(what[n]), "%s", argv[i] + 2);
                snprintf(arg[n], sizeof(arg[n]), "%s", argv[++i]);
                n++;
            } else {
                i++;
            }
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        usage();
        return 2;
    }
    if (n == 0) {
        snprintf(what[0], sizeof(what[0]), "uio");
        snprintf(arg[0], sizeof(arg[0]), "0");
        snprintf(what[1], sizeof(what[1]), "uio");
        snprintf(arg[1], sizeof(arg[1]), "1");
        n = 2;
    }

    for (i = 0; i < n; i++) {
        struct card *c = &cards[i];
        int st;

        memset(c, 0, sizeof(*c));
        if (strcmp(what[i], "bdf") == 0) {
            st = open_resource0(c, arg[i]);
        } else if (strcmp(what[i], "uio") == 0) {
            char dev[32];
            snprintf(dev, sizeof(dev), "/dev/uio%s", arg[i]);
            st = open_uio(c, dev);
        } else {
            st = open_uio(c, arg[i]);
        }
        if (st != 0) {
            fprintf(stderr, "%s %s: cannot map BAR0: %s\n", what[i], arg[i],
                    strerror(errno));
            rc = 1;
            continue;
        }
        if (c->bar0_len < BAR0_MIN)
            fprintf(stderr, "warning: %s is %zu bytes, offsets go up to 0x%x\n",
                    c->label, c->bar0_len, REG_FEAT_OVR_PLM);
        dump(c);
        if (probe)
            direct_probe(c);
        card_close(c);
        printf("\n");
    }
    return rc;
}
