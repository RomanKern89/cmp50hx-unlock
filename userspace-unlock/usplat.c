/* usplat — validate the platform assumptions the userspace CMP 50HX unlock
 * needs, and prove the vfio-pci access path end to end.
 *
 * Why vfio and not uio_pci_generic: under Secure Boot the kernel runs in
 * integrity lockdown, so the pci-sysfs config file refuses writes (that is
 * what sets PCI_COMMAND_MASTER). uio_pci_generic never calls pci_set_master()
 * itself and nvidia's remove path clears the bit with pci_disable_device(), so
 * a uio-bound card cannot DMA. vfio-pci calls pci_set_master() when the
 * device is opened, and vfio hands out BAR0 without the pci-sysfs mmap path
 * lockdown blocks. With this host's IOMMU off, vfio's no-IOMMU mode means the
 * addresses the firmware sees ARE physical addresses, which is exactly what
 * the booter's WPR meta needs.
 *
 *   1. /proc/self/pagemap exposes PFNs to root, so the DMA buffers can be
 *      described by their real addresses.
 *   2. MAP_HUGETLB|MAP_HUGE_2MB yields physically contiguous 2 MiB pages,
 *      so a radix-3 table can describe a buffer from one known base.
 *   3. vfio no-IOMMU + BAR0 mmap works and sets bus master.
 *
 *   sudo ./usplat /dev/uio0        # old uio path, read-only
 *   sudo ./usplat --vfio BDF       # vfio path: master + BAR0 + regs
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define HPAGE ((size_t)2 * 1024 * 1024)

static int pagemap_fd;

static uint64_t pfn_of(const void *p)
{
    uint64_t e = 0;
    off_t off = ((uintptr_t)p >> 12) * 8;

    if (pread(pagemap_fd, &e, sizeof(e), off) != sizeof(e))
        return 0;
    if (!(e & (1ull << 63)))
        return 0;
    return e & ((1ull << 55) - 1);
}

static void test_arena(void)
{
    unsigned char *a;
    uint64_t p0, p1, p512;

    a = mmap(NULL, 3 * HPAGE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT),
             -1, 0);
    if (a == MAP_FAILED) {
        printf("hugepage mmap: %s\n", strerror(errno));
        return;
    }
    memset(a, 0x5a, 3 * HPAGE);

    p0   = pfn_of(a);
    p1   = pfn_of(a + 4096);
    p512 = pfn_of(a + 512 * 4096);

    printf("hugepage mmap OK at %p\n", (void *)a);
    printf("  pfn[base]=0x%llx pfn[+4K]=0x%llx (%s) pfn[+2M]=0x%llx (%s)\n",
           (unsigned long long)p0, (unsigned long long)p1,
           p1 == p0 + 1 ? "contiguous" : "NOT contiguous",
           (unsigned long long)p512,
           p512 == p0 + 512 ? "2 MiB contiguous" : "splintered");
    printf("  phys[base]=0x%llx%s\n", (unsigned long long)(p0 << 12),
           p0 ? "" : "  <- pagemap hides PFNs, DMA plan dead");
    munmap(a, 3 * HPAGE);
}

static uint32_t cfg_command(const char *bdf)
{
    char path[160];
    uint32_t v = 0;
    int fd;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (pread(fd, &v, 4, 4) != 4)
        v = 0;
    close(fd);
    return v;
}

/* Open the BDF through vfio no-IOMMU mode. Tries every group device until one
 * answers for this BDF, because the group numbering is not ours to predict. */
static int vfio_open(const char *bdf, int *dev_fd_out, int *group_fd_out)
{
    int container, group = -1, dev = -1;
    char path[64];
    int i;

    container = open("/dev/vfio/vfio", O_RDWR);
    if (container < 0) {
        printf("open /dev/vfio/vfio: %s\n", strerror(errno));
        return -1;
    }
    if (ioctl(container, VFIO_SET_IOMMU, VFIO_NOIOMMU_IOMMU) < 0) {
        printf("VFIO_SET_IOMMU(NOIOMMU): %s\n", strerror(errno));
        close(container);
        return -1;
    }
    for (i = 0; i < 32 && dev < 0; i++) {
        int fd;

        snprintf(path, sizeof(path), "/dev/vfio/noiommu-%d", i);
        fd = open(path, O_RDWR);
        if (fd < 0)
            continue;
        if (ioctl(fd, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
            printf("%s: SET_CONTAINER: %s\n", path, strerror(errno));
            close(fd);
            continue;
        }
        dev = ioctl(fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
        if (dev < 0) {
            /* Wrong group for this BDF: undo, keep looking. */
            close(fd);
            continue;
        }
        group = fd;
        printf("vfio: %s holds %s (dev fd %d)\n", path, bdf, dev);
    }
    if (dev < 0) {
        printf("vfio: no no-IOMMU group holds %s\n", bdf);
        close(container);
        return -1;
    }
    *dev_fd_out = dev;
    *group_fd_out = group;
    return container;
}

static void test_vfio(const char *bdf)
{
    struct vfio_region_info ri = { 0 };
    volatile uint8_t *bar;
    uint32_t before, after;
    int dev, group, container;
    static const uint32_t regs[][2] = {
        { 0x00409650, 0 }, { 0x00409664, 0 }, { 0x0040966c, 0 },
        { 0x001fa824, 0 }, { 0x001fa828, 0 }, { 0x00118234, 0 },
    };
    const char *names[] = { "PLM", "SS0", "SS1", "WPR2_LO", "WPR2_HI", "GFW" };
    int i;

    before = cfg_command(bdf);
    printf("%s: command before vfio open = 0x%08x (master=%d)\n",
           bdf, before, !!(before & 4));

    container = vfio_open(bdf, &dev, &group);
    if (container < 0)
        return;

    after = cfg_command(bdf);
    printf("%s: command after  vfio open = 0x%08x (master=%d)%s\n",
           bdf, after, !!(after & 4),
           (after & 4) ? "  <- usable" : "  <- still no bus master");

    ri.argsz = sizeof(ri);
    ri.index = VFIO_PCI_BAR0_REGION_INDEX;
    if (ioctl(dev, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) {
        printf("GET_REGION_INFO: %s\n", strerror(errno));
        goto out;
    }
    printf("BAR0 region: size=0x%llx offset=0x%llx flags=0x%x\n",
           (unsigned long long)ri.size, (unsigned long long)ri.offset, ri.flags);

    bar = mmap(NULL, (size_t)ri.size, PROT_READ | PROT_WRITE, MAP_SHARED,
               dev, (off_t)ri.offset);
    if (bar == MAP_FAILED) {
        printf("BAR0 mmap: %s\n", strerror(errno));
        goto out;
    }
    for (i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++) {
        uint32_t v = *(volatile uint32_t *)(bar + regs[i][0]);

        printf("  %-8s [0x%06x] = 0x%08x\n", names[i], regs[i][0], v);
    }
    munmap((void *)bar, (size_t)ri.size);
out:
    close(dev);
    close(group);
    close(container);
}

int main(int argc, char **argv)
{
    int i;

    pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        perror("/proc/self/pagemap");
        return 1;
    }
    printf("uid=%d\n", (int)geteuid());
    if (argc > 1 && strcmp(argv[1], "--vfio") == 0) {
        test_arena();
        for (i = 2; i < argc; i++)
            test_vfio(argv[i]);
        return 0;
    }
    test_arena();
    for (i = 1; i < argc; i++)
        printf("(uio path retired; use --vfio BDF)\n"), (void)argv[i];
    return 0;
}
