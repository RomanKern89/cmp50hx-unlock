/* cmp50-unlock-us — unlock CMP 50HX (TU102, 10de:1e09) speed selectors from
 * Linux userspace, before the stock nvidia driver binds the card.
 *
 * This is a port of efi-unlock/unlock50x_v1.c (u50_unlock_card) to Linux. The
 * hardware sequence is unchanged — kill the POST-time GFW, boot the 50HX FWSEC
 * on GSP so WPR2 comes up at the FRTS carve, load the TU102 SEC2 booter through
 * its port window with the V67 payload published as the signature, let the
 * booter's privileged write run, then hand the card back after an FLR. What
 * changes is the platform layer, and it is dictated by Secure Boot:
 *
 *   lockdown=integrity refuses /sys/.../resource0 mmap ("direct PCI access is
 *   restricted") and refuses pci-sysfs config writes, so PCI_COMMAND_MASTER
 *   cannot be set the obvious ways. uio_pci_generic maps BAR0 but never calls
 *   pci_set_master() and nvidia's remove path clears the bit, so a uio card
 *   cannot DMA and the booter never reads its WPR meta.
 *
 *   vfio-pci in no-IOMMU mode is the way through: vfio hands out BAR0 without
 *   the pci-sysfs path, and PCI_COMMAND is writable through the VFIO config
 *   region (a different code path than the one lockdown blocks). With no IOMMU
 *   on the host there is no translation, so the addresses the firmware reads
 *   are plain physical addresses.
 *
 *   A userspace process has no EFI allocator, so the DMA buffers live in 2 MiB
 *   hugepages and their physical addresses come from /proc/self/pagemap (PFNs
 *   are only visible to CAP_SYS_ADMIN, which this needs anyway). The booter
 *   must read them from above 4 GiB: below 4 GiB the reads are gated on a
 *   locked card and the booter exits 0x91.
 *
 * Run it with both cards unbound from nvidia (an initramfs unit is the
 * intended place, exactly like the reference userspace-unlock project loads
 * its kernel module before nvidia). Interactively it will unbind nvidia, take
 * the card, and bind nvidia back at the end.
 *
 *   sudo ./cmp50-unlock-us --blobs ../efi-unlock/blobs
 *   sudo ./cmp50-unlock-us --bdf 0000:05:00.0 --stage fwsec
 *
 * A card that stops responding mid-run stays that way until a power cycle;
 * that is the same risk the EFI unlock carries.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * CMP 50HX / TU102 register map (BAR0 offsets, from the EFI unlock)     *
 * ------------------------------------------------------------------ */
#define REG_FEAT_OVR_PLM        0x00409650u
#define REG_FEAT_OVR_SM_SPD     0x00409664u
#define REG_FEAT_OVR_SM_SPD_1   0x0040966cu
/* Readout of the applied SM issue rates: nine 3-bit fields, 0 = full speed. */
#define REG_FEAT_READOUT        0x00409668u
#define REG_PFB_MMU_WPR2_LO     0x001fa824u
#define REG_PFB_MMU_WPR2_HI     0x001fa828u
#define REG_GFW_BOOT_OK         0x00118234u
#define REG_GFW_PLM             0x00118128u
#define REG_ACR_SCRATCH         0x00001438u

#define VAL_PLM_OPEN            0xffffffffu
#define VAL_SS0_UNLOCKED        0x88888888u
#define VAL_SS1_UNLOCKED        0x00000008u

#define NV_PSEC_BASE            0x00840000u
#define NV_PSEC_FBIF_BASE       0x00840600u
#define NV_FALCON2_SEC_BASE     0x00841000u
#define SEC2_MAILBOX0           (NV_PSEC_BASE + 0x040)
#define SEC2_MAILBOX1           (NV_PSEC_BASE + 0x044)
#define SEC2_IRQSTAT            (NV_PSEC_BASE + 0x008)
#define SEC2_DEBUGINFO          (NV_PSEC_BASE + 0x094)
#define SEC2_CPUCTL             (NV_PSEC_BASE + 0x100)
#define SEC2_BOOTVEC            (NV_PSEC_BASE + 0x104)
#define SEC2_DMACTL             (NV_PSEC_BASE + 0x10c)
#define SEC2_DMATRFBASE         (NV_PSEC_BASE + 0x110)
#define SEC2_DMATRFCMD          (NV_PSEC_BASE + 0x118)
#define SEC2_ENGINE             (NV_PSEC_BASE + 0x3c0)
#define SEC2_FBIF_CTL           (NV_PSEC_FBIF_BASE + 0x24)
#define SEC2_RM                 (NV_PSEC_BASE + 0x084)
#define SEC2_IMEMT0             (NV_PSEC_BASE + 0x188)
#define SEC2_IMEMC0             (NV_PSEC_BASE + 0x180)
#define SEC2_IMEMD0             (NV_PSEC_BASE + 0x184)
#define SEC2_DMEMC0             (NV_PSEC_BASE + 0x1c0)
#define SEC2_DMEMD0             (NV_PSEC_BASE + 0x1c4)
#define SEC2_BCR_CTRL           (NV_FALCON2_SEC_BASE + 0x668)
#define SEC2_HWCFG2             (NV_PSEC_BASE + 0x0f4)
#define SEC2_CPUCTL_ALIAS       (NV_PSEC_BASE + 0x130)
#define SEC2_CSBERRSTAT         (NV_PSEC_BASE + 0x244)
#define SEC2_FHSTATE            (NV_PSEC_BASE + 0x05c)

#define GSP_BASE                0x00110000u
#define GSP_ENGINE              (GSP_BASE + 0x3c0)
#define GSP_MAILBOX0            (GSP_BASE + 0x040)
#define GSP_MAILBOX1            (GSP_BASE + 0x044)
#define GSP_CPUCTL              (GSP_BASE + 0x100)
#define GSP_BOOTVEC             (GSP_BASE + 0x104)
#define GSP_HWCFG               (GSP_BASE + 0x108)
#define GSP_DMACTL              (GSP_BASE + 0x10c)
#define GSP_HWCFG2              (GSP_BASE + 0x0f4)
#define GSP_FBIF_TRANSCFG0      (GSP_BASE + 0x600)
#define GSP_FBIF_TRANSCFG4      (GSP_BASE + 0x610)
#define GSP_FBIF_CTL            (GSP_BASE + 0x624)
#define GSP_RM                  (GSP_BASE + 0x084)
#define GSP_IRQMSET             (GSP_BASE + 0x080)
#define GSP_IMEMC0              (GSP_BASE + 0x180)
#define GSP_IMEMD0              (GSP_BASE + 0x184)
#define GSP_IMEMT0              (GSP_BASE + 0x188)
#define GSP_DMEMC0              (GSP_BASE + 0x1c0)
#define GSP_DMEMD0              (GSP_BASE + 0x1c4)
#define GSP_BCR                 (0x00111000u + 0x668)
#define GSP_FALCON2_BASE        0x00111000u

#define NV_PTIMER_TIME_0        0x00009400u
#define NV_PTIMER_TIME_1        0x00009410u

#define CPUCTL_STARTCPU         0x2u
#define CPUCTL_HALTED           0x10u

/* ------------------------------------------------------------------ *
 * Firmware geometry (TU102 SEC2 booter + 50HX FWSEC), from the EFI    *
 * ------------------------------------------------------------------ */
#define V67_SIZE                0x0000fa00u
#define BOOTER_UCODE_SIZE       0x0000e700u
#define BOOTER_SIG_PATCH_LOC    0x00008700u
#define GSP_RM_BOOT_SIZE        0x00006000u
#define WPR_META_SIZE           256u

#define FW50_CODE_SIZE          0x00009a00u
#define FW50_DATA_OFF           0x00009a00u
#define FW50_DMEM_SIZE          0x000003f0u
#define FW50_BLOB_SIZE          0x00009df0u
#define FW50_MAPPER_OFF         0x00000360u
#define FW50_CMDIN_OFF          0x000003b0u
#define FW50_NS_SIZE            0x00000400u
#define FW50_SEC_BASE           0x00000400u
#define FW50_SEC_SIZE           0x00009600u
#define FW50_FRTS_OFFSET        0x27fe00000ull
#define FW50_WPR2_LO_UP         0x027fe000u
#define FWSEC_CMD_FRTS          0x15u

#define GSP_BL_TU102_SIZE       0x00000300u
#define GSP_BL_TU102_CODE_SIZE  0x00000200u
#define GSP_BL_TU102_START_TAG  0x000000fdu
#define BL_DESC_SIZE            0x54u
#define BL_DESC_CTXDMA          0x20u
#define BL_DESC_CODEDMA_LO      0x24u
#define BL_DESC_CODEDMA_HI      0x28u
#define BL_DESC_NSOFF           0x2cu
#define BL_DESC_NSSIZE          0x30u
#define BL_DESC_SECOFF          0x34u
#define BL_DESC_SECSIZE         0x38u
#define BL_DESC_ENTRY           0x3cu
#define BL_DESC_DATADMA_LO      0x40u
#define BL_DESC_DATADMA_HI      0x44u
#define BL_DESC_DATASIZE        0x48u

#define DUMMY_FW_SIZE           0x1bfadc8ull   /* real gsp_tu10x .fwimage size */

#define GSP_FW_WPR_META_MAGIC   0xdc3aae21371a60b3ull
#define GSP_FW_WPR_META_REVISION 1u

struct wpr_meta {
    uint64_t magic;
    uint64_t revision;
    uint64_t sysmemAddrOfRadix3Elf;
    uint64_t sizeOfRadix3Elf;
    uint64_t sysmemAddrOfBootloader;
    uint64_t sizeOfBootloader;
    uint64_t bootloaderCodeOffset;
    uint64_t bootloaderDataOffset;
    uint64_t bootloaderManifestOffset;
    uint64_t sysmemAddrOfSignature;
    uint64_t sizeOfSignature;
    uint64_t gspFwRsvdStart;
    uint64_t nonWprHeapOffset;
    uint64_t nonWprHeapSize;
    uint64_t gspFwWprStart;
    uint64_t gspFwHeapOffset;
    uint64_t gspFwHeapSize;
    uint64_t gspFwOffset;
    uint64_t bootBinOffset;
    uint64_t frtsOffset;
    uint64_t frtsSize;
    uint64_t gspFwWprEnd;
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;
    uint64_t bootCount;
    uint64_t partitionRpcAddr;
    uint16_t partitionRpcRequestOffset;
    uint16_t partitionRpcReplyOffset;
    uint32_t elfCodeOffset;
    uint32_t elfDataOffset;
    uint32_t elfCodeSize;
    uint32_t elfDataSize;
    uint32_t lsUcodeVersion;
    uint8_t  gspFwHeapVfPartitionCount;
    uint8_t  flags;
    uint8_t  padding[2];
    uint32_t pmuReservedSize;
    uint64_t verified;
};

/* ------------------------------------------------------------------ *
 * Logging                                                             *
 * ------------------------------------------------------------------ */
static void logmsg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ *
 * 2 MiB hugepage arena: buffers whose physical address we can state    *
 * ------------------------------------------------------------------ */
#define HPAGE_SHIFT 21
#define HPAGE_SIZE  (1u << HPAGE_SHIFT)

struct arena {
    uint8_t *base;
    size_t len;
    size_t used;
    unsigned pages;
    uint64_t *phys;      /* physical base of each 2 MiB page */
    int pagemap_fd;
};

static uint64_t pagemap_pfn(int fd, const void *p)
{
    uint64_t e = 0;
    off_t off = ((uintptr_t)p >> 12) * 8;

    if (pread(fd, &e, sizeof(e), off) != (ssize_t)sizeof(e))
        return 0;
    if (!(e & (1ull << 63)))
        return 0;
    return e & ((1ull << 55) - 1);
}

/* Reserve 2 MiB hugepages on demand: a boot service cannot be expected to set
 * nr_hugepages before the binary runs, and leaving it to the caller is exactly
 * the kind of coupling that makes a deployment fail on the first boot. */
static void ensure_hugepages(unsigned pages)
{
    char buf[32];
    long cur = 0;
    int fd;

    fd = open("/proc/sys/vm/nr_hugepages", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0) {
            buf[n] = '\0';
            cur = strtol(buf, NULL, 10);
        }
        close(fd);
    }
    if (cur >= (long)pages)
        return;
    fd = open("/proc/sys/vm/nr_hugepages", O_WRONLY);
    if (fd < 0) {
        logmsg("hugepages: cannot reserve %u pages: %s", pages, strerror(errno));
        return;
    }
    snprintf(buf, sizeof(buf), "%u", pages);
    if (write(fd, buf, strlen(buf)) < 0)
        logmsg("hugepages: reserve failed: %s", strerror(errno));
    else
        logmsg("hugepages: reserved %u x 2 MiB", pages);
    close(fd);
}

static int arena_init(struct arena *a, unsigned pages)
{
    unsigned i;

    memset(a, 0, sizeof(*a));
    ensure_hugepages(pages);
    a->pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (a->pagemap_fd < 0) {
        logmsg("arena: /proc/self/pagemap: %s", strerror(errno));
        return -1;
    }
    a->len = (size_t)pages << HPAGE_SHIFT;
    a->base = mmap(NULL, a->len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
                       (HPAGE_SHIFT << MAP_HUGE_SHIFT),
                   -1, 0);
    if (a->base == MAP_FAILED && errno == ENOMEM) {
        /* The pool can take a moment to settle right after being resized. */
        usleep(200000);
        a->base = mmap(NULL, a->len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
                           (HPAGE_SHIFT << MAP_HUGE_SHIFT),
                       -1, 0);
    }
    if (a->base == MAP_FAILED) {
        logmsg("arena: hugepage mmap (%u pages): %s", pages, strerror(errno));
        a->base = NULL;
        return -1;
    }
    /* Touch every page first: pagemap only answers for populated PTEs. */
    memset(a->base, 0, a->len);

    a->pages = pages;
    a->phys = calloc(pages, sizeof(*a->phys));
    for (i = 0; i < pages; i++) {
        uint64_t pfn = pagemap_pfn(a->pagemap_fd, a->base + ((size_t)i << HPAGE_SHIFT));

        if (!pfn) {
            logmsg("arena: page %u has no visible PFN; root with CAP_SYS_ADMIN is required", i);
            munmap(a->base, a->len);
            free(a->phys);
            close(a->pagemap_fd);
            a->base = NULL;
            a->phys = NULL;
            a->pagemap_fd = -1;
            return -1;
        }
        a->phys[i] = pfn << 12;
    }
    logmsg("arena: %u x 2 MiB hugepages, physical 0x%llx .. 0x%llx",
           pages, (unsigned long long)a->phys[0],
           (unsigned long long)(a->phys[pages - 1] + HPAGE_SIZE));
    if (a->phys[0] < 0x100000000ull)
        logmsg("arena: WARNING base is below 4 GiB; the booter gates sysmem reads there");
    return 0;
}

static void *arena_alloc(struct arena *a, size_t size, size_t align)
{
    size_t off = (a->used + (align - 1)) & ~(align - 1);

    if (off + size > a->len) {
        logmsg("arena: out of memory (%zu + %zu > %zu)", a->used, size, a->len);
        return NULL;
    }
    a->used = off + size;
    return a->base + off;
}

static uint64_t arena_phys(const struct arena *a, const void *p)
{
    size_t off = (const uint8_t *)p - a->base;
    unsigned page = (unsigned)(off >> HPAGE_SHIFT);

    return a->phys[page] + (off & (HPAGE_SIZE - 1));
}

/* The EFI build used wbinvd; per-line clflush is the userspace equivalent. */
static void cache_flush(const void *p, size_t n)
{
    const uint8_t *c = p;
    size_t i;

    for (i = 0; i < n; i += 64)
        __asm__ volatile("clflush %0" : : "m"(*(const uint8_t *)(c + i)) : "memory");
    __sync_synchronize();
}

/* ------------------------------------------------------------------ *
 * vfio-pci platform layer                                             *
 * ------------------------------------------------------------------ */
struct gpu {
    char bdf[32];
    int container;
    int group;
    int devfd;
    off_t cfg_off;
    volatile uint8_t *bar;
    size_t barlen;
    int bound;
};

static uint32_t rd(struct gpu *g, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(g->bar + off);

    __sync_synchronize();
    return v;
}

static void wr(struct gpu *g, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(g->bar + off) = v;
    __sync_synchronize();
}

static uint32_t sysfs_dword(const char *bdf, off_t off)
{
    char path[160];
    uint32_t v = 0;
    int fd;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.64s/config", bdf);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (pread(fd, &v, 4, off) != 4)
        v = 0;
    close(fd);
    return v;
}

static uint16_t gpu_command(struct gpu *g)
{
    return (uint16_t)(sysfs_dword(g->bdf, 4) & 0xffff);
}

/* PCI_COMMAND_MASTER through the VFIO config region: the pci-sysfs file is
 * refused under lockdown, this path is not. */
static int gpu_set_master(struct gpu *g)
{
    uint16_t cmd;
    off_t at = g->cfg_off + 4;

    if (pread(g->devfd, &cmd, 2, at) != 2)
        return -1;
    if (!(cmd & 4)) {
        uint16_t want = (uint16_t)(cmd | 6);    /* MEMORY | BUS_MASTER */

        if (pwrite(g->devfd, &want, 2, at) != 2)
            return -1;
    }
    cmd = gpu_command(g);
    return (cmd & 4) ? 0 : -1;
}

static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, value, strlen(value));
    close(fd);
    return n == (ssize_t)strlen(value) ? 0 : -1;
}

static int gpu_open(struct gpu *g, const char *bdf)
{
    struct vfio_region_info ri = { 0 };
    char path[160];
    int i;

    memset(g, 0, sizeof(*g));
    g->container = g->group = g->devfd = -1;
    snprintf(g->bdf, sizeof(g->bdf), "%.31s", bdf);

    g->container = open("/dev/vfio/vfio", O_RDWR);
    if (g->container < 0) {
        logmsg("%s: open /dev/vfio/vfio: %s", bdf, strerror(errno));
        return -1;
    }
    /* Group first, then the IOMMU type: this kernel rejects SET_IOMMU on an
     * empty container. Keep scanning after a failed GET_DEVICE_FD — with more
     * than one no-IOMMU group the first one that accepts a container is not
     * necessarily the one holding this device. */
    {
        int iommu_set = 0;

        for (i = 0; i < 64 && g->devfd < 0; i++) {
            int fd;

            snprintf(path, sizeof(path), "/dev/vfio/noiommu-%d", i);
            fd = open(path, O_RDWR);
            if (fd < 0)
                continue;
            if (ioctl(fd, VFIO_GROUP_SET_CONTAINER, &g->container) < 0) {
                close(fd);
                continue;
            }
            if (!iommu_set) {
                if (ioctl(g->container, VFIO_SET_IOMMU, VFIO_NOIOMMU_IOMMU) < 0)
                    logmsg("%s: VFIO_SET_IOMMU: %s", bdf, strerror(errno));
                iommu_set = 1;
            }
            g->devfd = ioctl(fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
            if (g->devfd < 0) {
                close(fd);
                continue;
            }
            g->group = fd;
        }
    }
    if (g->group < 0) {
        logmsg("%s: no vfio no-IOMMU group holds this device (is vfio-pci bound?)", bdf);
        return -1;
    }

    ri.argsz = sizeof(ri);
    ri.index = VFIO_PCI_CONFIG_REGION_INDEX;
    if (ioctl(g->devfd, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) {
        logmsg("%s: config region: %s", bdf, strerror(errno));
        return -1;
    }
    g->cfg_off = (off_t)ri.offset;

    ri.argsz = sizeof(ri);
    ri.index = VFIO_PCI_BAR0_REGION_INDEX;
    if (ioctl(g->devfd, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) {
        logmsg("%s: BAR0 region: %s", bdf, strerror(errno));
        return -1;
    }
    g->barlen = (size_t)ri.size;
    g->bar = mmap(NULL, g->barlen, PROT_READ | PROT_WRITE, MAP_SHARED,
                  g->devfd, (off_t)ri.offset);
    if (g->bar == MAP_FAILED) {
        logmsg("%s: BAR0 mmap: %s", bdf, strerror(errno));
        g->bar = NULL;
        return -1;
    }
    if (gpu_set_master(g) != 0) {
        logmsg("%s: cannot enable bus master (cmd=0x%04x)", bdf, gpu_command(g));
        return -1;
    }
    g->bound = 1;
    logmsg("%s: BAR0 0x%zx mapped, command=0x%04x (MEM|BUS_MASTER)", bdf, g->barlen,
           gpu_command(g));
    return 0;
}

static void gpu_close(struct gpu *g)
{
    if (g->bar)
        munmap((void *)g->bar, g->barlen);
    if (g->devfd >= 0)
        close(g->devfd);
    if (g->group >= 0)
        close(g->group);
    if (g->container >= 0)
        close(g->container);
    g->bar = NULL;
    g->devfd = g->group = g->container = -1;
}

/* FLR through VFIO. Returns 0 when the card came back coherent. */
static int gpu_flr(struct gpu *g)
{
    if (!g->bound)
        return -1;
    if (ioctl(g->devfd, VFIO_DEVICE_RESET, 0) < 0) {
        logmsg("%s: VFIO_DEVICE_RESET: %s", g->bdf, strerror(errno));
        return -1;
    }
    usleep(200000);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Driver handoff (nvidia out, vfio in; and back)                       *
 * ------------------------------------------------------------------ */
static int driver_of(const char *bdf, char *out, size_t n)
{
    char path[160];
    char target[256];
    ssize_t len;
    const char *base;

    out[0] = '\0';
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.64s/driver", bdf);
    len = readlink(path, target, sizeof(target) - 1);
    if (len <= 0)
        return -1;
    target[len] = '\0';
    base = strrchr(target, '/');
    snprintf(out, n, "%.*s", (int)(n - 1), base ? base + 1 : target);
    return 0;
}

static int unbind_from(const char *bdf, const char *driver)
{
    char cur[64];
    char path[192];

    if (driver_of(bdf, cur, sizeof(cur)) != 0)
        return 0;                       /* already unbound */
    if (strcmp(cur, driver) != 0)
        return 0;
    snprintf(path, sizeof(path), "/sys/bus/pci/drivers/%s/unbind", driver);
    return write_file(path, bdf);
}

static int bind_to(const char *bdf, const char *driver)
{
    char path[192];

    snprintf(path, sizeof(path), "/sys/bus/pci/drivers/%s/bind", driver);
    return write_file(path, bdf);
}

static void set_override(const char *bdf, const char *value)
{
    char path[192];

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.64s/driver_override", bdf);
    write_file(path, value);
}

/* vfio has to be present before the first bind; on a cold boot nothing has
 * pulled it in yet. The no-IOMMU mode is what lets a card without an IOMMU
 * group be bound at all. */
static int ensure_vfio(void)
{
    if (system("modprobe -q vfio") != 0 || system("modprobe -q vfio_pci") != 0) {
        logmsg("cannot load the vfio modules");
        return -1;
    }
    if (access("/sys/module/vfio/parameters/enable_unsafe_noiommu_mode", W_OK) == 0)
        write_file("/sys/module/vfio/parameters/enable_unsafe_noiommu_mode", "1\n");
    return 0;
}

static int gpu_claim(const char *bdf)
{
    char cur[64];
    char path[192];
    char saved[64];

    saved[0] = '\0';
    if (driver_of(bdf, saved, sizeof(saved)) != 0)
        saved[0] = '\0';

    /* Runtime PM must stay off: if the device is allowed to enter D3hot when
     * vfio releases it, the fuse-override latches that hold the speed
     * selectors are lost before the next driver gets to read them. */
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.64s/power/control", bdf);
    if (write_file(path, "on") == 0)
        logmsg("%s: power/control=on (keeps the card in D0 across the handoff)", bdf);
    else
        logmsg("%s: could not force power/control=on: %s", bdf, strerror(errno));

    if (ensure_vfio() != 0)
        return -1;

    if (saved[0] && strcmp(saved, "vfio-pci") != 0) {
        logmsg("%s: detaching %s", bdf, saved);
        unbind_from(bdf, saved);
        usleep(300000);
    }
    set_override(bdf, "vfio-pci");
    bind_to(bdf, "vfio-pci");
    if (driver_of(bdf, cur, sizeof(cur)) != 0 || strcmp(cur, "vfio-pci") != 0) {
        logmsg("%s: cannot bind vfio-pci (driver is now %s)", bdf,
               cur[0] ? cur : "none");
        set_override(bdf, "\n");
        if (saved[0] && strcmp(saved, "vfio-pci") != 0 && strcmp(saved, cur) != 0) {
            logmsg("%s: restoring %s", bdf, saved);
            bind_to(bdf, saved);
        }
        return -1;
    }
    usleep(300000);
    return 0;
}

enum { HANDOFF_KEEP = 0, HANDOFF_NVIDIA = 1, HANDOFF_NONE = 2 };

/* Detach vfio and either leave the card driverless (initramfs: nvidia binds it
 * later on its own) or bind the already-loaded nvidia module back. */
static void gpu_release(struct gpu *g, int mode)
{
    unbind_from(g->bdf, "vfio-pci");
    set_override(g->bdf, "\n");
    usleep(200000);
    if (mode == HANDOFF_NVIDIA) {
        if (bind_to(g->bdf, "nvidia") != 0 && system("modprobe -q nvidia") == 0) {
            usleep(500000);
            bind_to(g->bdf, "nvidia");
        }
        usleep(200000);
    }
    {
        char cur[64];

        if (driver_of(g->bdf, cur, sizeof(cur)) == 0)
            logmsg("%s: handed off to %s", g->bdf, cur);
        else
            logmsg("%s: handed off unbound", g->bdf);
    }
}

/* ------------------------------------------------------------------ *
 * Blob loading                                                        *
 * ------------------------------------------------------------------ */
/* Load the first `expect` bytes of a blob. The binaries in the EFI tree are
 * sometimes the full driver image while the unlock only consumes a prefix
 * (gsp_rm_boot_dbg.bin is 128 KiB, GSP_RM_BOOT_SIZE is 0x6000). */
static int load_blob(const char *dir, const char *name, uint8_t *dst, size_t expect)
{
    char path[512];
    struct stat st;
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        logmsg("blob %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        logmsg("blob %s: stat failed", path);
        close(fd);
        return -1;
    }
    if ((size_t)st.st_size < expect) {
        logmsg("blob %s: %lld bytes, need %zu", path, (long long)st.st_size, expect);
        close(fd);
        return -1;
    }
    if ((size_t)st.st_size != expect)
        logmsg("blob %s: taking the first %zu of %lld bytes", name, expect,
               (long long)st.st_size);
    n = read(fd, dst, expect);
    close(fd);
    if (n != (ssize_t)expect) {
        logmsg("blob %s: short read", path);
        return -1;
    }
    return 0;
}

/* Validate every input before the card is touched: a missing or short blob
 * must cost a log line, not a half-applied unlock. */
static int preflight_blobs(const char *dir)
{
    static const struct {
        const char *name;
        size_t min;
    } need[] = {
        { "v67_payload.bin", V67_SIZE },
        { "booter_ucode_prod.bin", BOOTER_UCODE_SIZE },
        { "gsp_rm_boot_dbg.bin", GSP_RM_BOOT_SIZE },
        { "fwsec_50hx_prod.bin", FW50_BLOB_SIZE },
        { "bl_gsp_tu102.bin", GSP_BL_TU102_SIZE },
    };
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        char path[512];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s", dir, need[i].name);
        if (stat(path, &st) != 0) {
            logmsg("preflight: %s: %s", path, strerror(errno));
            bad = 1;
        } else if ((size_t)st.st_size < need[i].min) {
            logmsg("preflight: %s is %lld bytes, need %zu", path, (long long)st.st_size,
                   need[i].min);
            bad = 1;
        }
    }
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------ *
 * Card state helpers                                                  *
 * ------------------------------------------------------------------ */
static int is_unlocked(struct gpu *g)
{
    return rd(g, REG_FEAT_OVR_SM_SPD) == VAL_SS0_UNLOCKED &&
           rd(g, REG_FEAT_OVR_SM_SPD_1) == VAL_SS1_UNLOCKED;
}

static void dump_state(struct gpu *g, const char *tag)
{
    logmsg("%s: PLM=0x%08x SS0=0x%08x SS1=0x%08x RATE=0x%08x GFW=0x%08x WPR2=%08x:%08x",
           tag, rd(g, REG_FEAT_OVR_PLM), rd(g, REG_FEAT_OVR_SM_SPD),
           rd(g, REG_FEAT_OVR_SM_SPD_1), rd(g, REG_FEAT_READOUT),
           rd(g, REG_GFW_BOOT_OK), rd(g, REG_PFB_MMU_WPR2_HI),
           rd(g, REG_PFB_MMU_WPR2_LO));
}

/* The privileged chain lands the override words, but the applied rates change
 * only when the FECS aperture is open (PLM). Write the selectors from the host
 * too and report what the hardware actually reads back: if RATE keeps the
 * clamped pattern, the aperture is still gated and the step that opens it did
 * not take effect. */
static void host_selectors(struct gpu *g)
{
    uint32_t plm = rd(g, REG_FEAT_OVR_PLM);
    uint32_t rate_before = rd(g, REG_FEAT_READOUT);

    wr(g, REG_FEAT_OVR_SM_SPD, VAL_SS0_UNLOCKED);
    wr(g, REG_FEAT_OVR_SM_SPD_1, VAL_SS1_UNLOCKED);
    logmsg("host: PLM 0x%08x, SS0->0x%08x SS1->0x%08x, RATE 0x%08x -> 0x%08x",
           plm, rd(g, REG_FEAT_OVR_SM_SPD), rd(g, REG_FEAT_OVR_SM_SPD_1),
           rate_before, rd(g, REG_FEAT_READOUT));
    if (plm != VAL_PLM_OPEN)
        logmsg("host: PLM is not open (0x%08x): selectors are staged but gated", plm);
}

struct geom {
    uint64_t fb_size;
    uint64_t frts_offset;
    uint32_t wpr2_lo_up;
    uint32_t wpr2_hi_up;
};

/* The VBIOS latches the real framebuffer geometry at 0x100ce0 as
 * mag<<(scale+20); bit 30 means the part is 15/16 of that. This is the only
 * reliable source for a card whose framebuffer was reworked (the CMP 50HX
 * ships as 10 GiB, 20 GiB cards are common modifications), and getting it
 * wrong puts every WPR region in the middle of the framebuffer — the booter
 * then rejects the meta with 0x91. */
#define REG_TU102_FB_RANGE     0x00100ce0u
#define REG_VGA_WORKSPACE_BASE 0x00625f04u

static int fb_from_range(struct gpu *g, uint64_t *out)
{
    uint32_t range = rd(g, REG_TU102_FB_RANGE);
    uint32_t mag, scale;
    uint64_t fb;

    logmsg("geometry: FB range register = 0x%08x", range);
    if (range == 0xffffffffu || (range & 0xffff0000u) == 0xbadf0000u)
        return -1;
    mag = (range >> 4) & 0x3fu;
    scale = range & 0xfu;
    if (!mag || scale > 12)
        return -1;
    fb = (uint64_t)mag << (scale + 20);
    if (range & (1u << 30))
        fb = fb / 16 * 15;
    if (fb < 256ull << 20 || fb > 256ull << 30)
        return -1;
    *out = fb;
    return 0;
}

static void detect_fb_size(struct gpu *g, struct geom *geo, int force)
{
    uint32_t lo = rd(g, REG_PFB_MMU_WPR2_LO) & 0xfffffff0u;
    uint32_t hi = rd(g, REG_PFB_MMU_WPR2_HI) & 0xfffffff0u;
    uint64_t fb = 0;
    int have = 0;

    if (force == 20) {
        fb = 0x500000000ull;
        have = 1;
    } else if (force == 10) {
        fb = 0x280000000ull;
        have = 1;
    } else if (fb_from_range(g, &fb) == 0) {
        have = 1;
    }

    if (!have) {
        /* Fall back to the WPR2 the VBIOS POST latched: a 20 GiB part leaves
         * an 0xE00 window just below 0x06000000. */
        if (hi - lo == 0xe00u && lo >= 0x04000000u && lo < 0x06000000u) {
            geo->fb_size = 0x500000000ull;
            geo->frts_offset = (uint64_t)lo << 8;
            geo->wpr2_lo_up = lo;
            geo->wpr2_hi_up = hi;
            logmsg("geometry: 20 GiB from latched WPR2 (LO=0x%08x)", lo);
            return;
        }
        fb = 0x280000000ull;
    }

    geo->fb_size = fb;
    geo->frts_offset = (fb - 0x100000ull) & ~0x1ffffull;
    geo->frts_offset -= 0x100000ull;            /* 1 MiB FRTS at the top */
    geo->wpr2_lo_up = (uint32_t)(geo->frts_offset >> 8);
    geo->wpr2_hi_up = (uint32_t)((geo->frts_offset + 0xe0000ull) >> 8);
    logmsg("geometry: fb=0x%llx (%llu MiB) from %s, frts=0x%llx, vga_reg=0x%08x%s",
           (unsigned long long)geo->fb_size,
           (unsigned long long)(geo->fb_size >> 20),
           force == 20 ? "--fb 20g" : force == 10 ? "--fb 10g" : "FB range register",
           (unsigned long long)geo->frts_offset, rd(g, REG_VGA_WORKSPACE_BASE),
           (rd(g, REG_VGA_WORKSPACE_BASE) & 8) ? " (bit3 set, ignored like the EFI path)" : "");
}

static void set_gpu_time(struct gpu *g)
{
    struct timespec ts;
    uint64_t ns;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ns = 1787011200000000000ull;
    } else {
        ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    wr(g, NV_PTIMER_TIME_1, (uint32_t)(ns >> 32));
    wr(g, NV_PTIMER_TIME_0, (uint32_t)ns);
    logmsg("time: GPU PTIMER seeded (ns>>32=0x%08x)", (uint32_t)(ns >> 32));
}

/* ------------------------------------------------------------------ *
 * Falcon primitives                                                   *
 * ------------------------------------------------------------------ */
/* ENGINE._RESET pulse: kills the GFW the VBIOS booted at POST, which is what
 * holds SEC2 in privileged lockdown (0xbadf5620). */
static void gsp_engine_reset(struct gpu *g)
{
    int i;

    wr(g, GSP_ENGINE, 1);
    for (i = 0; i < 16; i++)
        (void)rd(g, GSP_ENGINE);
    wr(g, GSP_ENGINE, 0);
    for (i = 0; i < 16; i++)
        (void)rd(g, GSP_ENGINE);
}

/* After an engine reset the Falcon scrubs IMEM/DMEM; DMA issued while the
 * scrubber runs gets erased (the "DEAD5EC1 everywhere" failure). */
static int falcon_wait_scrub(struct gpu *g, uint32_t dmactl, uint32_t hwcfg2)
{
    int i;

    for (i = 0; i < 30000; i++) {
        if ((rd(g, dmactl) & 0x6) == 0 && !(rd(g, hwcfg2) & (1u << 12)))
            return 0;
        usleep(100);
    }
    return -1;
}

static void falcon_wait_reset_ready(struct gpu *g, uint32_t hwcfg2)
{
    int i;

    for (i = 0; i < 10000; i++) {
        if (rd(g, hwcfg2) & (1u << 31))
            return;
        usleep(100);
    }
}

/* Port load into SEC2 IMEM, one 256 B tag per block, secure flag on demand. */
static void sec2_imem_write(struct gpu *g, uint32_t dst, int sec, const uint32_t *src,
                            uint32_t size_bytes, uint32_t virt_addr)
{
    uint32_t words = size_bytes >> 2;
    uint32_t tag = virt_addr >> 8;
    uint32_t i;

    wr(g, SEC2_IMEMC0, (dst & 0xfffffcu) | (1u << 24) | (sec ? (1u << 28) : 0));
    for (i = 0; i < words; i++) {
        if ((i & 63u) == 0) {
            wr(g, SEC2_IMEMT0, tag & 0xffffu);
            tag++;
        }
        wr(g, SEC2_IMEMD0, src[i]);
    }
}

static void sec2_dmem_write(struct gpu *g, uint32_t dst, const uint32_t *src,
                            uint32_t size_bytes)
{
    uint32_t words = size_bytes >> 2;
    uint32_t i;

    wr(g, SEC2_DMEMC0, (dst & 0xfffffcu) | (1u << 24));
    for (i = 0; i < words; i++)
        wr(g, SEC2_DMEMD0, src[i]);
}

static void gsp_dmem_write(struct gpu *g, uint32_t dst, const uint32_t *src,
                           uint32_t size_bytes)
{
    uint32_t words = size_bytes >> 2;
    uint32_t i;

    wr(g, GSP_DMEMC0, (dst & 0xfffffcu) | (1u << 24));
    for (i = 0; i < words; i++)
        wr(g, GSP_DMEMD0, src[i]);
}

/* ------------------------------------------------------------------ *
 * Optional real GSP firmware image (the reference implementation puts  *
 * the driver's .fwimage into the radix data pages, not zeros)          *
 * ------------------------------------------------------------------ */
struct fw_image {
    uint8_t *map;
    size_t map_len;
    const uint8_t *data;
    size_t size;
};

static int fw_image_open(struct fw_image *fw, const char *path)
{
    struct stat st;
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *sh;
    const char *names;
    uint16_t i;
    int fd;

    memset(fw, 0, sizeof(*fw));
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        logmsg("gsp fw %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(Elf64_Ehdr)) {
        close(fd);
        return -1;
    }
    fw->map_len = (size_t)st.st_size;
    fw->map = mmap(NULL, fw->map_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (fw->map == MAP_FAILED) {
        fw->map = NULL;
        return -1;
    }
    eh = (const Elf64_Ehdr *)fw->map;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_shoff == 0 ||
        eh->e_shnum == 0 || eh->e_shstrndx >= eh->e_shnum) {
        logmsg("gsp fw %s: not an ELF container", path);
        return -1;
    }
    sh = (const Elf64_Shdr *)(fw->map + eh->e_shoff);
    names = (const char *)(fw->map + sh[eh->e_shstrndx].sh_offset);
    for (i = 0; i < eh->e_shnum; i++) {
        const char *name = names + sh[i].sh_name;

        if (strcmp(name, ".fwimage") != 0)
            continue;
        if (sh[i].sh_offset + sh[i].sh_size > fw->map_len) {
            logmsg("gsp fw %s: .fwimage outside the file", path);
            return -1;
        }
        fw->data = fw->map + sh[i].sh_offset;
        fw->size = (size_t)sh[i].sh_size;
        logmsg("gsp fw: %s .fwimage %zu bytes", path, fw->size);
        return 0;
    }
    logmsg("gsp fw %s: no .fwimage section", path);
    return -1;
}

/* ------------------------------------------------------------------ *
 * [7] WPR meta and radix-3 ELF table                                  *
 * ------------------------------------------------------------------ */
static void build_wpr_meta(struct wpr_meta *m, uint64_t elf_phys, uint64_t elf_size,
                           uint64_t sig_phys, uint64_t fb_size, uint64_t bl_phys,
                           uint64_t bl_size)
{
    const uint64_t MB = 0x100000ull;
    uint64_t wpr_end;

    memset(m, 0, sizeof(*m));
    m->magic = GSP_FW_WPR_META_MAGIC;
    m->revision = GSP_FW_WPR_META_REVISION;

    m->sysmemAddrOfRadix3Elf = elf_phys;
    m->sizeOfRadix3Elf = elf_size;
    m->sysmemAddrOfSignature = sig_phys;
    m->sizeOfSignature = V67_SIZE;

    m->sysmemAddrOfBootloader = bl_phys;
    m->sizeOfBootloader = bl_size;
    m->bootloaderCodeOffset = 0x1800;
    m->bootloaderDataOffset = 0x800;
    m->bootloaderManifestOffset = 0x0;

    m->fbSize = fb_size;
    m->vgaWorkspaceOffset = fb_size - MB;           /* CMP has no display fuse */
    m->vgaWorkspaceSize = fb_size - m->vgaWorkspaceOffset;
    wpr_end = m->vgaWorkspaceOffset & ~0x1ffffull;
    m->gspFwWprEnd = wpr_end;

    m->frtsSize = MB;
    m->frtsOffset = wpr_end - m->frtsSize;
    m->bootBinOffset = (m->frtsOffset - bl_size) & ~0xfffull;
    m->gspFwOffset = (m->bootBinOffset - elf_size) & ~0xffffull;

    /* Heap sizes are the live-driver values per framebuffer size; recomputing
     * them after alignment inflates the layout and the booter exits 0x91. */
    m->gspFwHeapSize = (fb_size == 0x280000000ull) ? 0x7f00000ull
                     : (fb_size == 0x500000000ull) ? 0xfe00000ull
                                                   : 0x6900000ull;
    m->gspFwHeapOffset = (m->gspFwOffset - m->gspFwHeapSize) & ~(MB - 1);
    m->gspFwWprStart = m->gspFwHeapOffset - MB;
    m->nonWprHeapSize = MB;
    m->nonWprHeapOffset = m->gspFwWprStart - MB;
    m->gspFwRsvdStart = m->nonWprHeapOffset;

    m->bootCount = 0;
    m->verified = 0;
    m->pmuReservedSize = 0;
    m->gspFwHeapVfPartitionCount = 0;
    m->flags = 0x1;                                  /* CLOCK_BOOST */
}

/* Radix-3 table over an all-zero dummy ELF: root -> L1 -> L2 -> data pages.
 * The EFI build allocated a second 28.9 MB zero buffer and copied it in; the
 * data is already zero here, so the region is simply left zeroed. */
static int build_radix(struct gpu *g, struct arena *a, size_t data_size,
                       const uint8_t *data, void **root_out, uint64_t *table_phys_out)
{
    uint64_t n_data = (data_size + 0xfffull) >> 12;
    uint64_t n_l2 = ((n_data - 1) >> 9) + 1;
    uint64_t pt_size = (2 + n_l2) << 12;
    uint64_t data_off = pt_size;
    size_t total = (size_t)(data_off + data_size);
    uint8_t *tab;
    uint64_t tab_phys;
    uint64_t i;

    (void)g;

    tab = arena_alloc(a, total, HPAGE_SIZE);
    if (!tab)
        return -1;
    memset(tab, 0, total);
    if (data)
        memcpy(tab + data_off, data, data_size);
    tab_phys = arena_phys(a, tab);

    for (i = 0; i < n_data; i++)
        *(uint64_t *)(tab + 0x2000 + i * 8) = arena_phys(a, tab + data_off + (i << 12));
    for (i = 0; i < n_l2; i++)
        *(uint64_t *)(tab + 0x1000 + i * 8) = tab_phys + 0x2000 + (i << 12);
    *(uint64_t *)tab = tab_phys + 0x1000;

    cache_flush(tab, (size_t)pt_size);
    logmsg("radix-3: %llu data pages, %llu L2 tables, table@0x%llx data@0x%llx",
           (unsigned long long)n_data, (unsigned long long)n_l2,
           (unsigned long long)tab_phys, (unsigned long long)(tab_phys + data_off));
    *root_out = tab;
    *table_phys_out = tab_phys;
    return 0;
}

/* ------------------------------------------------------------------ *
 * [8b] FWSEC on GSP -> FRTS -> WPR2 up (TU102 WITH_LOADER boot)       *
 * ------------------------------------------------------------------ */
static int fwsec_boot(struct gpu *g, struct geom *geo, uint8_t *blob, uint64_t blob_phys,
                      const uint8_t *bl, uint32_t bl_size)
{
    uint8_t *dmem = blob + FW50_DATA_OFF;
    uint32_t *mapper = (uint32_t *)(dmem + FW50_MAPPER_OFF);
    uint32_t *c = (uint32_t *)(dmem + FW50_CMDIN_OFF);
    uint32_t bl_bootvec = GSP_BL_TU102_START_TAG << 8;
    uint8_t desc[BL_DESC_SIZE];
    uint32_t *d = (uint32_t *)desc;
    uint32_t imem_size_blk, imem_dst_blk, bl_dst;
    uint64_t data_phys = blob_phys + FW50_DATA_OFF;
    uint32_t code_lo = (uint32_t)(blob_phys & 0xffffffffu);
    uint32_t code_hi = (uint32_t)(blob_phys >> 32);
    int i;

    (void)bl_size;

    logmsg("fwsec: reset GSP (killing GFW), BCR->FALCON, RM=chipId0");
    wr(g, GSP_IRQMSET, 0);
    falcon_wait_reset_ready(g, GSP_HWCFG2);
    wr(g, GSP_ENGINE, 1);
    for (i = 0; i < 16; i++)
        (void)rd(g, GSP_ENGINE);
    wr(g, GSP_ENGINE, 0);
    for (i = 0; i < 16; i++)
        (void)rd(g, GSP_ENGINE);
    if (falcon_wait_scrub(g, GSP_DMACTL, GSP_HWCFG2) != 0)
        logmsg("fwsec: scrub wait timed out (continuing)");
    usleep(50000);
    wr(g, GSP_BCR, 0);
    for (i = 0; i < 16; i++)
        (void)rd(g, GSP_BCR);
    wr(g, GSP_RM, 0x162000a1u);
    for (i = 0; i < 16; i++)
        (void)rd(g, GSP_RM);
    if ((rd(g, GSP_CPUCTL) & 0xbadf0000u) == 0xbadf0000u) {
        logmsg("fwsec: GSP still locked (0x%08x)", rd(g, GSP_CPUCTL));
        return -1;
    }

    /* Interface data: FRTS command with this card's carve. */
    c[0] = 1; c[1] = 24;            /* readVbiosDesc */
    c[2] = 0; c[3] = 0;
    c[4] = 0; c[5] = 2;
    c[6] = 1; c[7] = 20;            /* frtsRegionDesc */
    c[8] = (uint32_t)(geo->frts_offset >> 12);
    c[9] = 0x100;
    c[10] = 2;
    mapper[11] = FWSEC_CMD_FRTS;
    cache_flush(dmem, FW50_DMEM_SIZE);
    logmsg("fwsec: blob@0x%llx frts=0x%llx (4K=0x%x)", (unsigned long long)blob_phys,
           (unsigned long long)geo->frts_offset, c[8]);

    /* Host DMA into the Falcon needs a coherent, physical aperture. */
    {
        uint32_t data = rd(g, GSP_FBIF_CTL);

        wr(g, GSP_FBIF_CTL, data | (1u << 7));
    }
    wr(g, GSP_DMACTL, 0);
    wr(g, GSP_FBIF_TRANSCFG0, 0x15);
    wr(g, GSP_FBIF_TRANSCFG4, 0x15);

    /* BL DMEM descriptor at DMEM 0 and the generic BL at the top of IMEM. */
    memset(desc, 0, sizeof(desc));
    d[BL_DESC_CTXDMA / 4] = 4;
    d[BL_DESC_CODEDMA_LO / 4] = code_lo;
    d[BL_DESC_CODEDMA_HI / 4] = code_hi;
    d[BL_DESC_NSOFF / 4] = 0;
    d[BL_DESC_NSSIZE / 4] = FW50_NS_SIZE;
    d[BL_DESC_SECOFF / 4] = FW50_SEC_BASE;
    d[BL_DESC_SECSIZE / 4] = FW50_SEC_SIZE;
    d[BL_DESC_ENTRY / 4] = 0;
    d[BL_DESC_DATADMA_LO / 4] = (uint32_t)(data_phys & 0xffffffffu);
    d[BL_DESC_DATADMA_HI / 4] = (uint32_t)(data_phys >> 32);
    d[BL_DESC_DATASIZE / 4] = FW50_DMEM_SIZE;

    gsp_dmem_write(g, 0, d, BL_DESC_SIZE);
    cache_flush(desc, sizeof(desc));

    imem_size_blk = rd(g, GSP_HWCFG) & 0x1ff;
    imem_dst_blk = imem_size_blk - (GSP_BL_TU102_CODE_SIZE / 256);
    bl_dst = imem_dst_blk << 8;
    logmsg("fwsec: BL image at IMEM 0x%x (hwcfg blocks=%u), BOOTVEC=0x%x", bl_dst,
           imem_size_blk, GSP_BL_TU102_START_TAG << 8);

    wr(g, GSP_IMEMC0, bl_dst | (1u << 24));
    {
        uint32_t tag = GSP_BL_TU102_START_TAG;

        for (i = 0; i < (int)(GSP_BL_TU102_CODE_SIZE / 4); i++) {
            if ((i & 63) == 0) {
                wr(g, GSP_IMEMT0, tag);
                tag++;
            }
            wr(g, GSP_IMEMD0, *(const uint32_t *)(bl + i * 4));
        }
    }
    wr(g, GSP_IMEMC0, 0);

    wr(g, GSP_BOOTVEC, bl_bootvec);
    wr(g, GSP_FALCON2_BASE + 0x400, 0x10b0ff00);
    wr(g, GSP_CPUCTL, CPUCTL_STARTCPU);
    logmsg("fwsec: STARTCPU, polling WPR2 (up to 5 s)");

    for (i = 0; i < 5000; i++) {
        uint32_t lo = rd(g, REG_PFB_MMU_WPR2_LO);
        uint32_t hi = rd(g, REG_PFB_MMU_WPR2_HI);

        if ((lo >> 4) == (uint32_t)(geo->frts_offset >> 12) && (hi >> 4) != 0) {
            logmsg("fwsec: WPR2 up lo=0x%08x hi=0x%08x after %d ms", lo, hi, i);
            return 0;
        }
        if ((i % 1000) == 0)
            logmsg("fwsec: t=%dms wpr2=%08x:%08x gspcpu=0x%08x mbox0=0x%08x", i, hi, lo,
                   rd(g, GSP_CPUCTL), rd(g, GSP_MAILBOX0));
        usleep(1000);
    }
    logmsg("fwsec: WPR2 never came up (lo=0x%08x hi=0x%08x gspcpu=0x%08x mbox0=0x%08x)",
           rd(g, REG_PFB_MMU_WPR2_LO), rd(g, REG_PFB_MMU_WPR2_HI), rd(g, GSP_CPUCTL),
           rd(g, GSP_MAILBOX0));
    return -1;
}

/* Falcon reset, mirroring the reference implementation's gu_falcon_reset():
 * poll RESET_READY, pulse ENGINE._RESET, wait for the scrubber, switch a
 * RISC-V core back to Falcon if needed, then restore RM = PMC_BOOT_0. Doing
 * this first matters: the load below must not race the scrubber, and a card
 * that has seen a driver this power cycle left IMEM/DMEM dirty. */
static int sec2_reset_full(struct gpu *g)
{
    uint32_t hwcfg2 = 0, hwcfg = 0, bcr, chip;
    int i;

    /* RESET_READY may never assert on this part; the reference tolerates it. */
    for (i = 0; i < 100; i++) {
        hwcfg2 = rd(g, SEC2_HWCFG2);
        if (hwcfg2 & (1u << 31))
            break;
        usleep(10);
    }
    {
        uint32_t engine = rd(g, SEC2_ENGINE);

        wr(g, SEC2_ENGINE, engine | 1);
        for (i = 0; i < 10; i++)
            hwcfg2 = rd(g, SEC2_ENGINE);
        for (i = 0; i < 100; i++) {
            if (rd(g, SEC2_ENGINE) & 1)
                break;
            usleep(10);
        }
        wr(g, SEC2_ENGINE, rd(g, SEC2_ENGINE) & ~1u);
        for (i = 0; i < 10; i++)
            hwcfg2 = rd(g, SEC2_ENGINE);
        for (i = 0; i < 100; i++) {
            if (!(rd(g, SEC2_ENGINE) & 1))
                break;
            usleep(10);
        }
    }
    if (falcon_wait_scrub(g, SEC2_DMACTL, SEC2_HWCFG2) != 0) {
        logmsg("reset: scrub did not finish");
        return -1;
    }
    hwcfg = rd(g, SEC2_HWCFG2);
    (void)hwcfg;

    /* FALCON2 core select: a RISC-V core must be switched back to Falcon. */
    bcr = rd(g, SEC2_BCR_CTRL);
    if (bcr & (1u << 4)) {
        wr(g, SEC2_BCR_CTRL, 0);
        for (i = 0; i < 1000; i++) {
            if ((rd(g, SEC2_BCR_CTRL) & 0x11) == 1)
                break;
            usleep(10);
        }
    }
    chip = rd(g, 0);
    wr(g, SEC2_RM, chip);
    logmsg("reset: bcr=0x%08x->0x%08x rm=0x%08x hwcfg2=0x%08x", bcr, rd(g, SEC2_BCR_CTRL),
           chip, rd(g, SEC2_HWCFG2));
    return 0;
}

/* Wait until the Falcon reports not-halted. */
static int falcon_wait_running(struct gpu *g)
{
    int i;

    for (i = 0; i < 10000; i++) {
        if (!(rd(g, SEC2_CPUCTL) & CPUCTL_HALTED))
            return 0;
        usleep(10);
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * [9] DIRECT_SEC2 booter load with the V67 signature                   *
 * ------------------------------------------------------------------ */
static int booter_attempt(struct gpu *g, uint8_t *img, uint64_t meta_phys)
{
    uint32_t cpu, m0, m1, ctl;

    if (sec2_reset_full(g) != 0)
        return -1;
    if (falcon_wait_running(g) != 0)
        logmsg("booter: CPUCTL stays halted after reset (0x%08x), loading anyway",
               rd(g, SEC2_CPUCTL));

    /* REQUIRE_CTX=false, as kflcnDisableCtxReq leaves it. */
    {
        uint32_t fbif = rd(g, SEC2_FBIF_CTL);

        wr(g, SEC2_FBIF_CTL, fbif | (1u << 7));
    }
    ctl = rd(g, SEC2_DMACTL);
    wr(g, SEC2_DMACTL, ctl & ~1u);

    /* NS code [0..0x100) at 0, secure code [0x100..0x8500) at 0x100, DMEM at
     * 0x8500. The prod signature already sits at image+0x8700 == DMEM+0x200. */
    sec2_imem_write(g, 0x000, 0, (const uint32_t *)(img + 0x0000), 0x100, 0x0000);
    sec2_imem_write(g, 0x100, 1, (const uint32_t *)(img + 0x0100), 0x8400, 0x0100);
    sec2_dmem_write(g, 0x000, (const uint32_t *)(img + 0x8500), 0x6200);
    cache_flush(img, 0xe700);

    {
        uint32_t rb_ns, rb_sec, rb_dmem;

        wr(g, SEC2_IMEMC0, 0);
        rb_ns = rd(g, SEC2_IMEMD0);
        wr(g, SEC2_IMEMC0, 0x100u | (1u << 28));
        rb_sec = rd(g, SEC2_IMEMD0);
        wr(g, SEC2_DMEMC0, 0x200);
        rb_dmem = rd(g, SEC2_DMEMD0);
        logmsg("booter: readback IMEM[0]=0x%08x(exp 0x%08x) IMEM_S[0x100]=0x%08x "
               "DMEM[0x200]=0x%08x(exp sig 0x%08x)",
               rb_ns, *(const uint32_t *)img, rb_sec, rb_dmem,
               *(const uint32_t *)(img + 0x8700));
    }

    wr(g, SEC2_BOOTVEC, 0);
    wr(g, SEC2_MAILBOX0, (uint32_t)(meta_phys & 0xffffffffu));
    wr(g, SEC2_MAILBOX1, (uint32_t)(meta_phys >> 32));
    ctl = rd(g, SEC2_CPUCTL);
    /* CPUCTL bit 6 selects the alias register on newer Falcon revisions. */
    wr(g, (ctl & (1u << 6)) ? SEC2_CPUCTL_ALIAS : SEC2_CPUCTL, CPUCTL_STARTCPU);
    logmsg("booter: started (meta@0x%llx, cpuctl=0x%08x), holding 3 s", 
           (unsigned long long)meta_phys, ctl);
    sleep(3);

    cpu = rd(g, SEC2_CPUCTL);
    usleep(200000);
    m0 = rd(g, SEC2_MAILBOX0);
    m1 = rd(g, SEC2_MAILBOX1);
    logmsg("booter: cpu=0x%08x mbox=0x%08x:0x%08x dbg=0x%08x irq=0x%08x csberr=0x%08x "
           "fhstate=0x%08x", cpu, m0, m1, rd(g, SEC2_DEBUGINFO), rd(g, SEC2_IRQSTAT),
           rd(g, SEC2_CSBERRSTAT), rd(g, SEC2_FHSTATE));
    dump_state(g, "booter: result");

    return m0 == 0 ? 0 : -1;
}

/* The SEC2 core is a RISC-V with an execution trace ring; when the booter
 * aborts it is the only record of where it stopped. */
static void sec2_trace_dump(struct gpu *g)
{
    /* The RISC-V trace ring belongs to the SEC2 Falcon2 window (0x841000),
     * not the GSP one. */
    const uint32_t ring = NV_FALCON2_SEC_BASE + 0x400;
    uint32_t rd_idx = rd(g, ring + 0x004) & 0xff;
    uint32_t wt_idx = rd(g, ring + 0x008) & 0xff;
    int t;

    logmsg("trace: ctl=0x%08x rd=%u wt=%u bcr=0x%08x", rd(g, ring), rd_idx, wt_idx,
           rd(g, SEC2_BCR_CTRL));
    for (t = 0; t < 12; t++) {
        uint32_t idx = (wt_idx + 0x100 - t) & 0xff;
        uint32_t lo, hi;

        wr(g, ring + 0x004, idx);
        lo = rd(g, ring + 0x00c);
        hi = rd(g, ring + 0x010);
        logmsg("trace[-%d] pc=0x%08x%08x", t, hi, lo);
    }
    {
        uint32_t d10, d4c, d50;

        wr(g, SEC2_DMEMC0, 0x10);
        d10 = rd(g, SEC2_DMEMD0);
        wr(g, SEC2_DMEMC0, 0xff4c);
        d4c = rd(g, SEC2_DMEMD0);
        wr(g, SEC2_DMEMC0, 0xff50);
        d50 = rd(g, SEC2_DMEMD0);
        logmsg("trace: dmem[0x10]=0x%08x [0xff4c]=0x%08x [0xff50]=0x%08x", d10, d4c, d50);
    }
}

/* ------------------------------------------------------------------ *
 * [10] sanitize: leave SEC2 in a cold state so nvidia can take over    *
 * ------------------------------------------------------------------ */
static void sec2_sanitize(struct gpu *g)
{
    int i;

    for (i = 0; i < 100000; i++) {
        if (rd(g, SEC2_CPUCTL) & CPUCTL_HALTED)
            break;
    }
    wr(g, SEC2_ENGINE, 1);
    for (i = 0; i < 16; i++)
        (void)rd(g, SEC2_ENGINE);
    wr(g, SEC2_ENGINE, 0);
    for (i = 0; i < 16; i++)
        (void)rd(g, SEC2_ENGINE);
    usleep(100000);
    wr(g, SEC2_MAILBOX0, 0);
    wr(g, SEC2_MAILBOX1, 0);
    logmsg("sanitize: SEC2 cpu=0x%08x mbox0=0x%08x", rd(g, SEC2_CPUCTL), rd(g, SEC2_MAILBOX0));
}

/* ------------------------------------------------------------------ *
 * Per-card unlock                                                     *
 * ------------------------------------------------------------------ */
struct opts {
    const char *blobs;
    int max_stage;
    int do_flr;
    int handoff;            /* HANDOFF_* */
    int allow_low;
    int force_fb;           /* 0 = auto, 10 or 20 GiB */
    const char *gsp_fw;     /* optional real .fwimage container */
    int hold;               /* seconds to keep the card open after the verdict */
};

enum { STAGE_PROBE = 1, STAGE_FWSEC = 2, STAGE_BOOTER = 3, STAGE_UNLOCK = 4,
       STAGE_FLR = 5, STAGE_HANDOFF = 6 };

static int unlock_card(const char *bdf, const struct opts *o)
{
    struct arena arena;
    struct gpu g;
    struct geom geo;
    struct wpr_meta *meta;
    uint8_t *v67, *ucode, *bl, *fwsec_blob, *sec2_bl;
    uint64_t v67_phys, ucode_phys, bl_phys, fwsec_phys, meta_phys;
    void *radix;
    uint64_t radix_phys = 0;
    int rc = -1;
    static const uint8_t booter_sig_prod[16] = {
        0x32, 0x60, 0x43, 0x67, 0x98, 0x89, 0x7b, 0x06,
        0xe8, 0x5f, 0x1a, 0xa8, 0x71, 0x43, 0x5d, 0xcb
    };

    logmsg("=== %s ===", bdf);

    /* Room for the radix table + its 28.9 MB data, the booter images and the
     * metadata: 24 hugepages is 48 MiB, comfortably above the ~29.4 MB peak. */
    if (arena_init(&arena, 24) != 0)
        return -1;
    if (arena.phys[0] < 0x100000000ull && !o->allow_low) {
        logmsg("%s: arena below 4 GiB; rerun with --allow-low to try anyway", bdf);
        goto out_arena;
    }

    /* Identify the device before claiming it: a mistyped --bdf must not yank a
     * NIC or another GPU out of its driver. */
    {
        uint32_t id = sysfs_dword(bdf, 0);

        if (id == 0) {
            logmsg("%s: cannot read the PCI vendor/device id", bdf);
            goto out_arena;
        }
        if ((id & 0xffff) != 0x10de || (id >> 16) != 0x1e09) {
            logmsg("%s: not a CMP 50HX (%04x:%04x), nothing claimed", bdf, id & 0xffff,
                   id >> 16);
            goto out_arena;
        }
    }

    if (gpu_claim(bdf) != 0)
        goto out_arena;
    if (gpu_open(&g, bdf) != 0)
        goto out_gpu;
    dump_state(&g, "entry");


    /* [2] geometry from the latched WPR2, [3] chip id */
    detect_fb_size(&g, &geo, o->force_fb);
    logmsg("BOOT0=0x%08x (TU10x arch id is 0x16)", rd(&g, 0));

    if (is_unlocked(&g)) {
        logmsg("%s: already unlocked", bdf);
        rc = 0;
        goto out_gpu;
    }

    /* [5] GFW must have finished its POST-time boot before we kill it */
    if ((rd(&g, REG_GFW_BOOT_OK) & 0xff) != 0xff) {
        int i;

        logmsg("GFW not ready (0x%08x), waiting", rd(&g, REG_GFW_BOOT_OK));
        for (i = 0; i < 200; i++) {
            usleep(50000);
            if ((rd(&g, REG_GFW_BOOT_OK) & 0xff) == 0xff)
                break;
        }
    }
    logmsg("GFW_BOOT_OK=0x%08x", rd(&g, REG_GFW_BOOT_OK));

    if (o->max_stage <= STAGE_PROBE) {
        logmsg("%s: probe complete (BAR0 mapped, bus master enabled; no GPU register "
               "written)", bdf);
        rc = 2;
        goto out_gpu;
    }
    set_gpu_time(&g);

    /* [6] every buffer the firmware reads lives in the hugepage arena */
    v67 = arena_alloc(&arena, V67_SIZE, HPAGE_SIZE);
    ucode = arena_alloc(&arena, BOOTER_UCODE_SIZE, HPAGE_SIZE);
    bl = arena_alloc(&arena, GSP_RM_BOOT_SIZE, HPAGE_SIZE);
    fwsec_blob = arena_alloc(&arena, FW50_BLOB_SIZE, HPAGE_SIZE);
    sec2_bl = arena_alloc(&arena, GSP_BL_TU102_SIZE, HPAGE_SIZE);
    meta = arena_alloc(&arena, 512, HPAGE_SIZE);
    if (!v67 || !ucode || !bl || !fwsec_blob || !sec2_bl || !meta)
        goto out_gpu;

    if (load_blob(o->blobs, "v67_payload.bin", v67, V67_SIZE) ||
        load_blob(o->blobs, "booter_ucode_prod.bin", ucode, BOOTER_UCODE_SIZE) ||
        load_blob(o->blobs, "gsp_rm_boot_dbg.bin", bl, GSP_RM_BOOT_SIZE) ||
        load_blob(o->blobs, "fwsec_50hx_prod.bin", fwsec_blob, FW50_BLOB_SIZE) ||
        load_blob(o->blobs, "bl_gsp_tu102.bin", sec2_bl, GSP_BL_TU102_SIZE))
        goto out_gpu;

    /* The booter verifies its own signature from DMEM+0x200; the prod bytes
     * belong in the image at PATCH_LOC before anything is loaded. */
    memcpy(ucode + BOOTER_SIG_PATCH_LOC, booter_sig_prod, sizeof(booter_sig_prod));

    v67_phys = arena_phys(&arena, v67);
    ucode_phys = arena_phys(&arena, ucode);
    bl_phys = arena_phys(&arena, bl);
    fwsec_phys = arena_phys(&arena, fwsec_blob);
    meta_phys = arena_phys(&arena, meta);
    cache_flush(v67, V67_SIZE);
    cache_flush(ucode, BOOTER_UCODE_SIZE);
    cache_flush(bl, GSP_RM_BOOT_SIZE);
    logmsg("layout: v67@0x%llx ucode@0x%llx bl@0x%llx fwsec@0x%llx meta@0x%llx",
           (unsigned long long)v67_phys, (unsigned long long)ucode_phys,
           (unsigned long long)bl_phys, (unsigned long long)fwsec_phys,
           (unsigned long long)meta_phys);

    /* [7] radix-3 table over the (zero or real) GSP image + WPR meta */
    {
        struct fw_image fwimg;
        const uint8_t *elf = NULL;
        size_t elf_size = DUMMY_FW_SIZE;

        if (o->gsp_fw && fw_image_open(&fwimg, o->gsp_fw) == 0) {
            elf = fwimg.data;
            elf_size = fwimg.size;
        }
        if (build_radix(&g, &arena, elf_size, elf, &radix, &radix_phys) != 0)
            goto out_gpu;
        build_wpr_meta(meta, radix_phys, elf_size, v67_phys, geo.fb_size,
                       bl_phys, GSP_RM_BOOT_SIZE);
    }
    cache_flush(meta, sizeof(struct wpr_meta));
    logmsg("meta: fb=0x%llx frts=0x%llx heap=0x%llx+0x%llx fw=0x%llx",
           (unsigned long long)meta->fbSize, (unsigned long long)meta->frtsOffset,
           (unsigned long long)meta->gspFwHeapOffset,
           (unsigned long long)meta->gspFwHeapSize,
           (unsigned long long)meta->gspFwOffset);

    /* [7.6] snapshot before any engine reset */
    logmsg("WPR2 pre-kill: %08x:%08x", rd(&g, REG_PFB_MMU_WPR2_HI),
           rd(&g, REG_PFB_MMU_WPR2_LO));

    /* [8] kill the POST-time GFW; SEC2 becomes reachable */
    gsp_engine_reset(&g);
    usleep(200000);
    {
        uint32_t cpuctl = rd(&g, SEC2_CPUCTL);

        logmsg("SEC2 post-kill: CPUCTL=0x%08x DMATRFCMD=0x%08x FBIF_CTL=0x%08x BCR=0x%08x",
               cpuctl, rd(&g, SEC2_DMATRFCMD), rd(&g, SEC2_FBIF_CTL), rd(&g, SEC2_BCR_CTRL));
        if ((cpuctl & 0xbadf0000u) == 0xbadf0000u) {
            int i;

            logmsg("SEC2 locked, trying BCR CORE_SELECT=FALCON");
            wr(&g, SEC2_BCR_CTRL, 0);
            for (i = 0; i < 16; i++)
                (void)rd(&g, SEC2_BCR_CTRL);
            usleep(10000);
            cpuctl = rd(&g, SEC2_CPUCTL);
        }
        if ((cpuctl & 0xbadf0000u) == 0xbadf0000u) {
            logmsg("SEC2 still locked (0x%08x); stopping", cpuctl);
            goto out_gpu;
        }
    }

    /* [8b] FWSEC: run it only on a cold card (WPR2 down). A WPR2 that is up at
     * an unexpected carve means a driver already used this card since power-on;
     * launching the booter against a meta that does not match the live WPR is
     * the documented 0x91 failure, so refuse instead. */
    {
        uint32_t lo = rd(&g, REG_PFB_MMU_WPR2_LO) & 0xfffffff0u;
        uint32_t hi = rd(&g, REG_PFB_MMU_WPR2_HI) & 0xfffffff0u;

        if ((hi >> 4) == 0) {
            if (fwsec_boot(&g, &geo, fwsec_blob, fwsec_phys, sec2_bl,
                           GSP_BL_TU102_SIZE) != 0)
                logmsg("fwsec: failed; booter will be attempted anyway (control)");
        } else if ((lo & 0xfffffff0u) == geo.wpr2_lo_up &&
                   (hi & 0xfffffff0u) == geo.wpr2_hi_up) {
            logmsg("fwsec: WPR2 already up at the expected carve, skipping");
        } else {
            logmsg("%s: WPR2 is up at an unexpected carve 0x%08x:0x%08x (want "
                   "0x%08x:0x%08x) — the card is not cold; power-cycle it",
                   bdf, hi, lo, geo.wpr2_hi_up, geo.wpr2_lo_up);
            rc = 1;
            goto out_gpu;
        }
    }
    logmsg("WPR2 after fwsec: %08x:%08x", rd(&g, REG_PFB_MMU_WPR2_HI),
           rd(&g, REG_PFB_MMU_WPR2_LO));
    {
        uint32_t scratch = rd(&g, REG_ACR_SCRATCH);

        logmsg("ACR scratch=0x%08x (%s)", scratch, (scratch >> 16) ? "FWSEC reports an error" : "clean");
    }

    if (o->max_stage <= STAGE_FWSEC) {
        rc = 2;
        goto out_gpu;
    }

    /* [9] booter: exactly one launch. The first start after the reset is the
     * only one that runs — a second start on the same SEC2 block reads a
     * corpse, so there is no retry to be had here. */
    if (booter_attempt(&g, ucode, meta_phys) != 0) {
        logmsg("booter: launch did not complete");
        sec2_trace_dump(&g);
    }
    host_selectors(&g);

    /* [10] verdict */
    sec2_sanitize(&g);
    dump_state(&g, "final");
    if (is_unlocked(&g)) {
        logmsg("%s: *** UNLOCKED (SS0=0x88888888 SS1=0x8) ***", bdf);
        rc = 0;
    } else {
        uint32_t plm = rd(&g, REG_FEAT_OVR_PLM);

        logmsg("%s: not unlocked: PLM=0x%08x %s", bdf, plm,
               plm == VAL_PLM_OPEN ? "(open, selectors gated)"
               : plm == 0xffffff8fu ? "(staged, selectors gated)" : "(closed)");
        rc = 1;
    }

    if (o->max_stage <= STAGE_UNLOCK || rc != 0)
        goto out_gpu;

    /* Watch the selectors while the device is still held open, then close the
     * vfio fd halfway through: the BAR mapping stays valid, so the reads still
     * work and show whether the release is what clears the latches. */
    if (o->hold > 0) {
        int i;

        for (i = 0; i < o->hold * 4; i++) {
            if (i == o->hold * 2) {
                close(g.devfd);
                g.devfd = -1;
                g.bound = 0;        /* no device fd => no FLR possible any more */
                logmsg("hold: vfio fd closed, BAR0 still readable (FLR no longer "
                       "possible)");
            }
            logmsg("hold %d.%02ds: chip=0x%08x PLM=0x%08x SS0=0x%08x SS1=0x%08x", i / 4,
                   (i % 4) * 25, rd(&g, 0), rd(&g, REG_FEAT_OVR_PLM),
                   rd(&g, REG_FEAT_OVR_SM_SPD), rd(&g, REG_FEAT_OVR_SM_SPD_1));
            usleep(250000);
        }
    }

    /* [11] FLR: the driver that binds next expects a cold, WPR2-down card.
     * The selectors live in fuse-override and survive the reset; WPR2 must
     * come back disabled or nvidia's kgspBootGspRm refuses the card. */
    if (o->do_flr) {
        uint32_t chip = rd(&g, 0);

        if (gpu_flr(&g) != 0) {
            logmsg("flr: failed; keeping the card as is");
        } else {
            uint32_t lo = rd(&g, REG_PFB_MMU_WPR2_LO);
            uint32_t hi = rd(&g, REG_PFB_MMU_WPR2_HI);
            uint16_t cmd = gpu_command(&g);

            logmsg("flr: chip=0x%08x (was 0x%08x) cmd=0x%04x wpr2=%08x:%08x", rd(&g, 0),
                   chip, cmd, hi, lo);
            dump_state(&g, "post-flr");
            if (hi >> 4)
                logmsg("flr: WPR2 still enabled (hi=0x%08x); nvidia may reject the card", hi);
            if (!is_unlocked(&g))
                logmsg("flr: selectors did not survive the reset");
        }
    }

    if (o->max_stage <= STAGE_FLR)
        goto out_gpu;

out_gpu:
    gpu_close(&g);
    if (o->handoff != HANDOFF_KEEP)
        gpu_release(&g, o->handoff);
out_arena:
    if (arena.phys)
        free(arena.phys);
    if (arena.base)
        munmap(arena.base, arena.len);
    if (arena.pagemap_fd >= 0)
        close(arena.pagemap_fd);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Card enumeration and command line                                   *
 * ------------------------------------------------------------------ */
static int find_cards(char bdfs[][32], int max)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    int n = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        char path[384];
        uint32_t id = 0;
        int fd;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.64s/config", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        if (pread(fd, &id, 4, 0) == 4 && (id & 0xffff) == 0x10de && (id >> 16) == 0x1e09 &&
            n < max)
            snprintf(bdfs[n++], 32, "%.31s", e->d_name);
        close(fd);
    }
    closedir(d);
    return n;
}

static void usage(void)
{
    printf("usage: cmp50-unlock-us [options] [--bdf BDF]...\n"
           "  --blobs DIR      firmware directory (default ../efi-unlock/blobs)\n"
           "  --bdf BDF        unlock this card only (default: every 10de:1e09)\n"
           "  --stage NAME     stop after: probe | fwsec | booter | unlock | flr | handoff\n"
           "                   (default handoff)\n"
           "  --no-flr         skip the post-unlock FLR\n"
           "  --handoff MODE   nvidia (default: bind the loaded driver back),\n"
           "                   none (leave the card unbound, for initramfs),\n"
           "                   keep (stay on vfio-pci for debugging)\n"
           "  --no-handoff     same as --handoff keep\n"
           "  --allow-low      permit buffers below 4 GiB (booter usually fails)\n"
           "  --fb auto|10g|20g  framebuffer size for the WPR layout (default auto:\n"
           "                   the FB range register at 0x100ce0)\n"
           "  --gsp-fw FILE    put the driver's real .fwimage into the radix pages\n"
           "                   instead of a zero-filled dummy image\n"
           "  --hold SECONDS   keep the card open after the verdict and re-read\n"
           "                   the selectors, to see whether they persist\n"
           "  --verbose        accepted for symmetry; output is always detailed\n");
}

int main(int argc, char **argv)
{
    struct opts o = { .blobs = "../efi-unlock/blobs", .max_stage = STAGE_HANDOFF,
                      .do_flr = 1, .handoff = HANDOFF_NVIDIA, .allow_low = 0 };
    char cards[8][32];
    int ncards = 0, i, rc = 0, ok = 0, failed = 0, stopped = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bdf") == 0 && i + 1 < argc) {
            if (ncards < 8)
                snprintf(cards[ncards++], 32, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--blobs") == 0 && i + 1 < argc) {
            o.blobs = argv[++i];
        } else if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            const char *s = argv[++i];

            if (!strcmp(s, "probe")) o.max_stage = STAGE_PROBE;
            else if (!strcmp(s, "fwsec")) o.max_stage = STAGE_FWSEC;
            else if (!strcmp(s, "booter")) o.max_stage = STAGE_BOOTER;
            else if (!strcmp(s, "unlock")) o.max_stage = STAGE_UNLOCK;
            else if (!strcmp(s, "flr")) o.max_stage = STAGE_FLR;
            else if (!strcmp(s, "handoff")) o.max_stage = STAGE_HANDOFF;
            else { usage(); return 2; }
        } else if (strcmp(argv[i], "--no-flr") == 0) {
            o.do_flr = 0;
        } else if (strcmp(argv[i], "--no-handoff") == 0) {
            o.handoff = HANDOFF_KEEP;
        } else if (strcmp(argv[i], "--handoff") == 0 && i + 1 < argc) {
            const char *s = argv[++i];

            if (!strcmp(s, "nvidia")) o.handoff = HANDOFF_NVIDIA;
            else if (!strcmp(s, "none")) o.handoff = HANDOFF_NONE;
            else if (!strcmp(s, "keep")) o.handoff = HANDOFF_KEEP;
            else { usage(); return 2; }
        } else if (strcmp(argv[i], "--allow-low") == 0) {
            o.allow_low = 1;
        } else if (strcmp(argv[i], "--fb") == 0 && i + 1 < argc) {
            const char *s = argv[++i];

            if (!strcmp(s, "10g")) o.force_fb = 10;
            else if (!strcmp(s, "20g")) o.force_fb = 20;
            else o.force_fb = 0;
        } else if (strcmp(argv[i], "--gsp-fw") == 0 && i + 1 < argc) {
            o.gsp_fw = argv[++i];
        } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            o.hold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            /* always verbose */
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    if (geteuid() != 0) {
        logmsg("must run as root (pagemap PFNs, vfio, driver bind)");
        return 1;
    }
    if (preflight_blobs(o.blobs) != 0) {
        logmsg("firmware blobs are incomplete; nothing was touched");
        return 1;
    }
    if (ncards == 0)
        ncards = find_cards(cards, 8);
    if (ncards == 0) {
        logmsg("no CMP 50HX (10de:1e09) found");
        return 1;
    }
    logmsg("%d card(s) to unlock", ncards);

    for (i = 0; i < ncards; i++) {
        int r = unlock_card(cards[i], &o);

        if (r == 0)
            ok++;
        else if (r == 2)
            stopped++;
        else {
            failed++;
            rc = 1;
        }
    }
    logmsg("done: %d unlocked, %d failed%s", ok, failed,
           stopped ? ", stopped at the requested stage" : "");
    return rc;
}
