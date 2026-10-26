/*
 * SWEET internal UFS root device.
 *
 * The Qualcomm UFS host (UFSHCI 2.1) is left enabled with the link up by the
 * bootloader, which loaded our boot image from it. This driver reuses that
 * state: it points the transfer request list at its own memory (verifying the
 * host took it before ringing any doorbell) and issues polled SCSI commands,
 * one at a time, to LUN 0. It finds one GPT partition by name (boot-arg
 * ufsroot=, default "userdata") and exposes it as the BSD disk ufsroot0
 * (rufsroot0), with 512-byte logical sectors over the device's 4 KiB blocks.
 * Root on it with rd=ufsroot0.
 *
 * All DMA goes through one physically contiguous allocation (descriptors and
 * a 1 MiB bounce buffer); the host is not cache coherent, so caches are
 * cleaned/invalidated around each command.
 *
 * Writes are refused unless boot-arg ufsrw=1 is set. Every write is checked
 * against the partition bounds again right before it is issued, so nothing
 * outside the named partition can be written.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/kauth.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include "UmiSmpMode.h"
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <machine/atomic.h>
#include "UmiLowDma.h"
#include "I16UmiSmmu.h"
#include "gpt4096.c"
#include "UmiRootVolume.h"

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
/* Actual managed physical window, exported by arm_vm_init. */
extern unsigned long gPhysBase, gPhysSize;
/* osfmk/arm/cpu_data.h, not exported to the BSD layer */
extern int get_preemption_level(void);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;
extern unsigned int ml_get_cpu_count(void);
static void sweet_log_start(void);
extern void sweet_sample_start(void);
extern void sweet_keys_init(void);
extern void sweet_touch_init(void);
extern void sweet_usb_probe(void);

#define REG_CAP         0x00
#define REG_IS          0x20
#define REG_IE          0x24
#define REG_HCS         0x30
#define REG_HCE         0x34
#define REG_UTRLBA      0x50
#define REG_UTRLBAU     0x54
#define REG_UTRLDBR     0x58
#define REG_UTRLCLR     0x5c
#define REG_UTRLRSR     0x60

#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define CAP_64AS        (1u << 24)              /* 64-bit DMA addressing */

#define UFS_BLOCK       4096u
#define BOUNCE_SIZE     (64u << 10)
#define PRDT_CHUNK      (64u << 10)            /* PRDT byte count field: 18 bits */
#define PRDT_MAX        (BOUNCE_SIZE / PRDT_CHUNK)
#define CMD_TIMEOUT_NS  (5ull * NSEC_PER_SEC)
#define CMD_SPIN_NS     (50ull * NSEC_PER_USEC)
#define CMD_SLEEP_NS    (200ull * NSEC_PER_USEC)

struct utrd {
	uint32_t dw0, dw1, dw2, dw3;
	uint32_t ucdba, ucdbau;
	uint32_t resp;          /* [31:16] response UPIU offset, [15:0] length, DWORDs */
	uint32_t prdt;          /* [31:16] PRDT offset (DWORDs), [15:0] entries */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;
	} prdt[PRDT_MAX];
};

/* Layout of the contiguous DMA allocation. */
#define DMA_UTRL        0x0000                  /* 1 KiB aligned */
#define DMA_UCD         0x1000                  /* 128 byte aligned */
#define DMA_BOUNCE      0x4000
#define DMA_SIZE        (DMA_BOUNCE + BOUNCE_SIZE)

static struct {
	vm_offset_t regs;
	vm_offset_t dma;                        /* kernel VA of the DMA allocation */
	uint64_t dma_pa;
	uint8_t lun;
	uint8_t tag;
	uint64_t device_blocks;
	uint64_t first_block, nblocks;          /* partition, in 4 KiB device blocks */
	uint32_t secsize;                       /* logical sector size for the BSD layer */
	bool writable;                          /* boot-arg ufsrw=1 */
	bool cache;                             /* boot-arg ufscache=1 */
	bool wait;                              /* boot-arg ufswait=1 */
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
} su;

/* Explicit diagnostic only. Direct console callbacks avoid a delayed printf
 * tail: serial=1 routes each byte to the framebuffer and opt-in RAM mirror.
 * No UFS writes are added; trace only observes the first eight commands. */
extern void console_write_unbuffered(char c);
static bool ufs_trace_enabled, ufs_trace_active;
static bool ufs_lowdma_enabled;
static bool umi_rootfs_mode;
static uint32_t ufs_trace_commands;

static void ufs_trace_text(const char *s)
{
    while (*s) console_write_unbuffered(*s++);
}

static void ufs_trace_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        console_write_unbuffered(digits[(value >> shift) & 15]);
}

static void ufs_trace_step(const char *step)
{
    if (!ufs_trace_active) return;
    ufs_trace_text("\nUFS TRACE "); ufs_trace_text(step); ufs_trace_text("\n");
}

static void ufs_trace_value(const char *key, uint64_t value)
{
    if (!ufs_trace_active) return;
    ufs_trace_text("UFS TRACE "); ufs_trace_text(key); ufs_trace_text("=0x");
    ufs_trace_hex(value); ufs_trace_text("\n");
}

static LCK_GRP_DECLARE(sweet_ufs_grp, "sweet_ufs");
static LCK_MTX_DECLARE(sweet_ufs_lock, &sweet_ufs_grp);

static inline uint32_t
ufs_rd(uint32_t off)
{
	return *(volatile uint32_t *)(su.regs + off);
}

static inline void
ufs_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(su.regs + off) = v;
}

static void
dma_sync(uint32_t off, uint32_t len)
{
    if (ufs_lowdma_enabled) {
        assert(off <= DMA_SIZE && len <= DMA_SIZE - off);
        ufs_trace_step("low DMA NC: before DSB (no physical cache helper)");
        __builtin_arm_dsb(0xf);
        ufs_trace_step("low DMA NC: after DSB");
        return;
    }
    ufs_trace_value("flush PA", su.dma_pa + off);
    ufs_trace_value("flush bytes", len);
    ufs_trace_step("before flush_dcache64");
    flush_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
    ufs_trace_step("after flush_dcache64");
}

static void
dma_clean(uint32_t off, uint32_t len)
{
    if (ufs_lowdma_enabled) {
        assert(off <= DMA_SIZE && len <= DMA_SIZE - off);
        ufs_trace_step("low DMA NC: before DSB (no physical cache helper)");
        __builtin_arm_dsb(0xf);
        ufs_trace_step("low DMA NC: after DSB");
        return;
    }
    ufs_trace_value("clean PA", su.dma_pa + off);
    ufs_trace_value("clean bytes", len);
    ufs_trace_step("before clean_dcache64");
    clean_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
    ufs_trace_step("after clean_dcache64");
}

static int umi_smmu_inspect(void);
static int umi_smmu_takeover(void);
/*
 * One SCSI command in slot 0, data (if any) to/from the bounce buffer
 * (to the device when write is set). Returns 0 or an errno.
 */
static int
ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len, bool write)
{
    if (ufs_lowdma_enabled && write && (!umi_rootfs_mode || !su.writable)) return EPERM;
    ufs_trace_active = ufs_trace_enabled && (++ufs_trace_commands <= 8);
    ufs_trace_step("SCSI enter");
    ufs_trace_value("command count", ufs_trace_commands);
    ufs_trace_value("opcode", cdb[0]);
    ufs_trace_value("cdb bytes", cdb_len);
    ufs_trace_value("transfer bytes", len);
    ufs_trace_value("dma VA", su.dma);
    ufs_trace_value("dma PA", su.dma_pa);
	struct utrd *utrd = (struct utrd *)(su.dma + DMA_UTRL);
	struct ucd *ucd = (struct ucd *)(su.dma + DMA_UCD);
	uint64_t ucd_pa = su.dma_pa + DMA_UCD, bounce_pa = su.dma_pa + DMA_BOUNCE;

	assert(len <= BOUNCE_SIZE);
    ufs_trace_step("before UTRD bzero");
    bzero(utrd, sizeof(*utrd));
    ufs_trace_step("after UTRD bzero; before UCD bzero");
    bzero(ucd, sizeof(*ucd));
    ufs_trace_step("after UCD bzero; encoding request");

	ucd->cmd[0] = 0x01;                     /* COMMAND UPIU */
	ucd->cmd[1] = len ? (write ? 0x20 : 0x40) : 0x00;      /* W: data out, R: data in */
	ucd->cmd[2] = su.lun;
	ucd->cmd[3] = ++su.tag;
	ucd->cmd[12] = (uint8_t)(len >> 24);
	ucd->cmd[13] = (uint8_t)(len >> 16);
	ucd->cmd[14] = (uint8_t)(len >> 8);
	ucd->cmd[15] = (uint8_t)len;
	memcpy(&ucd->cmd[16], cdb, cdb_len);

	/* DD: 1 = memory to device, 2 = device to memory. */
	utrd->dw0 = (1u << 28) | ((len ? (write ? 1u : 2u) : 0u) << 25) | (1u << 24);
	utrd->dw2 = 0x0f;                       /* OCS invalid until updated */
	utrd->ucdba = (uint32_t)ucd_pa;
	utrd->ucdbau = (uint32_t)(ucd_pa >> 32);
	utrd->resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uint32_t n = 0;
		for (uint32_t done = 0; done < len; done += PRDT_CHUNK, n++) {
			uint32_t chunk = MIN(PRDT_CHUNK, len - done);
			ucd->prdt[n].addr_lo = (uint32_t)(bounce_pa + done);
			ucd->prdt[n].addr_hi = (uint32_t)((bounce_pa + done) >> 32);
			ucd->prdt[n].dbc = chunk - 1;
		}
		utrd->prdt = (1024u / 4) << 16 | n;
	}

    ufs_trace_step("descriptor encoding complete");
    ufs_trace_value("UPIU LUN", ucd->cmd[2]);
    ufs_trace_value("UPIU tasktag", ucd->cmd[3]);
    ufs_trace_value("UTRD dw0", utrd->dw0);
    ufs_trace_value("UTRD UCD address", ((uint64_t)utrd->ucdbau << 32) | utrd->ucdba);
    ufs_trace_value("UTRD response layout", utrd->resp);
    ufs_trace_value("UTRD PRDT layout", utrd->prdt);
    if (su.cache) {
        dma_clean(DMA_UTRL, sizeof(*utrd));
		dma_clean(DMA_UCD, 16 + cdb_len);
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
		if (len) {
			dma_clean(DMA_UCD + offsetof(struct ucd, prdt),
			    (utrd->prdt & 0xffff) * sizeof(ucd->prdt[0]));
		}
	} else {
		dma_sync(DMA_UTRL, sizeof(*utrd));
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len) {
		if (su.cache && write) {
			dma_clean(DMA_BOUNCE, len);
		} else {
			dma_sync(DMA_BOUNCE, len);
		}
	}
    ufs_trace_step("before pre-doorbell DSB");
    __builtin_arm_dsb(0xf);
    ufs_trace_step("after pre-doorbell DSB");
    if (ufs_trace_active) {
        ufs_trace_step("before CAP read"); ufs_trace_value("CAP", ufs_rd(REG_CAP));
        ufs_trace_step("before HCE read"); ufs_trace_value("HCE", ufs_rd(REG_HCE));
        ufs_trace_step("before HCS read"); ufs_trace_value("HCS", ufs_rd(REG_HCS));
        ufs_trace_step("before IS read"); ufs_trace_value("IS", ufs_rd(REG_IS));
        ufs_trace_step("before UTRLBA read"); ufs_trace_value("UTRLBA", ufs_rd(REG_UTRLBA));
        ufs_trace_step("before UTRLBAU read"); ufs_trace_value("UTRLBAU", ufs_rd(REG_UTRLBAU));
        ufs_trace_step("before UTRLRSR read"); ufs_trace_value("UTRLRSR", ufs_rd(REG_UTRLRSR));
    }
    ufs_trace_step("before UTRLDBR write1");
    ufs_wr(REG_UTRLDBR, 1);
    ufs_trace_step("after UTRLDBR write; before first DBR read");
	/* SWEET: bounded busy poll. Timer based waits do not wake reliably on
	 * this board, which makes the transfer look like an infinite hang. */
    uint32_t spin;
    for (spin = 0; spin < 50000000 && (ufs_rd(REG_UTRLDBR) & 1); spin++) {
        if (ufs_trace_active && (spin == 0 || spin == 1048576 || spin == 16777216 || spin == 33554432 || spin == 49999999))
            ufs_trace_value("busy poll iteration", spin);
    }
    ufs_trace_value("poll loop exit iteration", spin);
    ufs_trace_step("before final DBR read");
	if (ufs_rd(REG_UTRLDBR) & 1) {
		ufs_trace_step("timeout: before UTRLCLR write");
		ufs_wr(REG_UTRLCLR, ~1u);
		ufs_trace_step("timeout: after UTRLCLR write");
		printf("SWEET UFS: command 0x%02x timed out\n", cdb[0]);
		return ETIMEDOUT;
	}
    ufs_trace_step("doorbell clear; before completion DSB");
    __builtin_arm_dsb(0xf);
    ufs_trace_step("after completion DSB; before IS read/write ack");
    if (ufs_trace_active) { ufs_trace_value("completion IS", ufs_rd(REG_IS)); ufs_trace_value("completion HCS", ufs_rd(REG_HCS)); }
    ufs_wr(REG_IS, ufs_rd(REG_IS));         /* acknowledge completion status */
    ufs_trace_step("after IS acknowledgement; invalidating response");

	dma_sync(DMA_UTRL, sizeof(*utrd));
	if (su.cache) {
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
	} else {
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len && !(su.cache && write)) {
		dma_sync(DMA_BOUNCE, len);
	}

    ufs_trace_step("response cache sync complete");
    ufs_trace_value("UTRD OCS", utrd->dw2);
    ufs_trace_value("response header first8", ((uint64_t)ucd->resp[0] << 56) | ((uint64_t)ucd->resp[1] << 48) | ((uint64_t)ucd->resp[2] << 40) | ((uint64_t)ucd->resp[3] << 32) | ((uint64_t)ucd->resp[4] << 24) | ((uint64_t)ucd->resp[5] << 16) | ((uint64_t)ucd->resp[6] << 8) | ucd->resp[7]);
    uint32_t ocs = utrd->dw2 & 0xff;
	if (ocs || ucd->resp[0] != 0x21 || ucd->resp[6] || ucd->resp[7]) {
		printf("SWEET UFS: command 0x%02x failed: ocs 0x%x upiu 0x%x response 0x%x status 0x%x\n",
		    cdb[0], ocs, ucd->resp[0], ucd->resp[6], ucd->resp[7]);
		return EIO;
	}
	return 0;
}

/* READ(10) of count device blocks at lba into the bounce buffer. */
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
	if (lba > UINT32_MAX || count > BOUNCE_SIZE / UFS_BLOCK) {
		return EINVAL;
	}
	uint8_t cdb[10] = {
		0x28, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);
}

/*
 * WRITE(10) of count device blocks from the bounce buffer to lba. The range
 * must lie inside the partition: this is the only place data goes out.
 */
static int
ufs_write_blocks(uint64_t lba, uint32_t count)
{
	if (!su.writable || !su.nblocks || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK ||
	    lba < su.first_block || lba - su.first_block >= su.nblocks ||
	    count > su.nblocks - (lba - su.first_block) || lba + count - 1 > UINT32_MAX) {
		printf("SWEET UFS: refusing write of %u blocks at 0x%llx\n", count, lba);
		return EPERM;
	}
	uint8_t cdb[10] = {
		0x2a, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);
}

static int
ufs_sync_cache(void)
{
	static const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */
	return ufs_scsi(cdb, sizeof(cdb), 0, false);
}

static uint32_t umi_be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int ufs_read_capacity(void)
{
    uint8_t cdb[10]={0x25};
    su.device_blocks=0;
    int error=ufs_scsi(cdb,sizeof(cdb),8,false);
    if(error) return error;
    const uint8_t *b=(const uint8_t *)(su.dma+DMA_BOUNCE);
    uint32_t last=umi_be32(b), block=umi_be32(b+4);
    printf("UMI CAPACITY last=0x%x block=%u\n",last,block);
    /* READ CAPACITY(16) is deliberately unsupported; no truncation. */
    if(last==UINT32_MAX || block!=UFS_BLOCK || last<3) return ENXIO;
    su.device_blocks=(uint64_t)last+1;
    return 0;
}
static int ufs_check_head(void)
{
    static uint8_t saved[UFS_BLOCK];
    const uint8_t *b=(const uint8_t *)(su.dma+DMA_BOUNCE);
    for(unsigned i=0;i<3;i++) {
        int error=ufs_read_blocks(su.first_block,1);
        if(error) return error;
        if(i==0) memcpy(saved,b,UFS_BLOCK);
        else if(memcmp(saved,b,UFS_BLOCK)) return EIO;
        printf("UMI HEAD read=%u crc32=0x%08x signature=0x%02x%02x\n",i,gpt4096_crc32(b,UFS_BLOCK),b[1024],b[1025]);
    }
    return 0;
}

static int ufs_validate_root_image(void)
{
    if(!umi_rootfs_mode || !ufs_lowdma_enabled || su.nblocks<UMI_ROOT_BLOCKS) return EPERM;
    const uint8_t *b=(const uint8_t *)(su.dma+DMA_BOUNCE);
    int error=ufs_read_blocks(su.first_block,1);
    if(error) return error;
    if(!umi_root_header_valid(b+1024,512)) {
        printf("UMI ROOT refused: primary HFSX identity mismatch; disk writes disabled\n");
        return ENXIO;
    }
    error=ufs_read_blocks(su.first_block+UMI_ROOT_BLOCKS-1,1);
    if(error) return error;
    if(!umi_root_header_valid(b+3072,512)) {
        printf("UMI ROOT refused: alternate HFSX identity mismatch; disk writes disabled\n");
        return ENXIO;
    }
    /* Expose only this qualified 16GiB volume, never the unformatted partition tail. */
    su.nblocks=UMI_ROOT_BLOCKS;
    printf("UMI ROOT HFSX primary+alternate identity verified; bounded to16GiB\n");
    return 0;
}

static int
ufs_find_partition(const char *want)
{
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	struct gpt4096_plan plan;
	struct gpt4096_partition part;
	enum gpt4096_status status;
	int error;

	/* Fail closed until independent READ CAPACITY is available. Never derive
	 * device_blocks from the untrusted GPT backup-LBA field. */
	su.first_block = 0;
	su.nblocks = 0;
	su.writable = false;
	if (!su.device_blocks) {
		printf("SWEET UFS: independent LUN capacity unavailable\n");
		return ENXIO;
	}
	error = ufs_read_blocks(1, 1);
	if (error) {
		return error;
	}
	status = gpt4096_header(b, UFS_BLOCK, su.device_blocks, BOUNCE_SIZE, &plan);
	if (status != GPT4096_OK) {
		printf("SWEET UFS: invalid primary GPT (%u)\n", (unsigned)status);
		return ENXIO;
	}
	/* Transport is READ(10), although the pure GPT helper supports uint64 LBAs. */
	if (plan.entry_lba > UINT32_MAX || !plan.entry_blocks ||
	    plan.entry_blocks - 1 > UINT32_MAX - plan.entry_lba) {
		return ENXIO;
	}
	/* Header-derived plan is on the stack; this read may reuse DMA_BOUNCE. */
	error = ufs_read_blocks(plan.entry_lba, (uint32_t)plan.entry_blocks);
	if (error) {
		return error;
	}
	status = gpt4096_find(&plan, b, (size_t)plan.entry_blocks * UFS_BLOCK,
	    want, &part);
	if (status != GPT4096_OK) {
		printf("SWEET UFS: GPT partition validation failed (%u)\n", (unsigned)status);
		return status == GPT4096_NOT_FOUND ? ENOENT : ENXIO;
	}
	if (part.last_lba > UINT32_MAX) {
		return ENXIO;
	}
	printf("UMI GPT CRC verified entries=%u index=%u first=0x%llx last=0x%llx\n",plan.entry_count,part.entry_index,part.first_lba,part.last_lba);
	su.first_block = part.first_lba;
	su.nblocks = part.block_count;
	return 0;
}

static int
ufs_take_over_host(void)
{
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		printf("SWEET UFS: host not ready (hce 0x%x hcs 0x%x)\n", hce, hcs);
		return ENXIO;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		printf("SWEET UFS: requests outstanding from the bootloader\n");
		return EBUSY;
	}
	/* With RAM above 4 GiB (mkadt.py --highmem) the DMA memory can be there too. */
	if (((su.dma_pa + DMA_SIZE - 1) >> 32) && !(ufs_rd(REG_CAP) & CAP_64AS)) {
		printf("SWEET UFS: DMA memory at 0x%llx but the host has no 64-bit addressing\n",
		    su.dma_pa);
		return ENXIO;
	}
	int smmu_error=umi_smmu_takeover();
	if(smmu_error) return smmu_error;
	ufs_wr(REG_IE, 0);                      /* polled operation */
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_wr(REG_UTRLRSR, 0);
	uint64_t list = su.dma_pa + DMA_UTRL;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	/* Never ring a doorbell unless the host uses our list. */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		printf("SWEET UFS: host did not take our request list\n");
		return EIO;
	}
	return 0;
}

static int
sweet_ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!su.ready) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !su.writable) ? EROFS : 0;
}

static int
sweet_ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void
sweet_ufs_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * su.secsize;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = su.nblocks * UFS_BLOCK;

	bool rd = buf_flags(bp) & B_READ;

	buf_setresid(bp, count);
	if (!rd && !su.writable) {
		error = EROFS;
		goto done;
	}
	if (off > part_bytes || count > part_bytes - off) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}

	lck_mtx_lock(&sweet_ufs_lock);
	for (uint32_t done = 0; done < count;) {
		uint64_t pos = off + done;
		uint64_t lba = pos / UFS_BLOCK;
		uint32_t skip = (uint32_t)(pos % UFS_BLOCK);
		uint32_t want = MIN(count - done, BOUNCE_SIZE - skip);
		uint32_t blocks = (skip + want + UFS_BLOCK - 1) / UFS_BLOCK;
		void *bounce = (void *)(su.dma + DMA_BOUNCE);
		if (rd) {
			error = ufs_read_blocks(su.first_block + lba, blocks);
			if (!error) {
				memcpy(vaddr + done, (const char *)bounce + skip, want);
			}
		} else {
			/* Partial device blocks at either end: read, merge, write back. */
			if (skip || (want % UFS_BLOCK)) {
				error = ufs_read_blocks(su.first_block + lba, blocks);
			}
			if (!error) {
				memcpy((char *)bounce + skip, vaddr + done, want);
				error = ufs_write_blocks(su.first_block + lba, blocks);
			}
		}
		if (error) {
			break;
		}
		done += want;
		buf_setresid(bp, count - done);
	}
	lck_mtx_unlock(&sweet_ufs_lock);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
sweet_ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)data;
	uint64_t *o = (uint64_t *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = su.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) {
			return ENODEV;
		}
		if (*f < DEV_BSIZE || *f > UFS_BLOCK || (*f & (*f - 1))) {
			return EINVAL;
		}
		su.secsize = *f;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = UFS_BLOCK;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = su.nblocks * UFS_BLOCK / su.secsize;
		return 0;
	case DKIOCISWRITABLE:
		*f = su.writable;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE: {
		if (!su.writable) {
			return 0;
		}
		lck_mtx_lock(&sweet_ufs_lock);
		int error = ufs_sync_cache();
		lck_mtx_unlock(&sweet_ufs_lock);
		return error;
	}
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = BOUNCE_SIZE / su.secsize;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = su.writable ? BOUNCE_SIZE / su.secsize : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
sweet_ufs_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return sweet_ufs_ioctl(dev, cmd, data, flag, p, 0);
}

static int
sweet_ufs_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return sweet_ufs_ioctl(dev, cmd, data, flag, p, 1);
}

static int
sweet_ufs_psize(__unused dev_t dev)
{
	return su.ready ? (int)MIN(su.nblocks * UFS_BLOCK / su.secsize, INT_MAX) : -1;
}

static int
sweet_ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(sweet_ufs_strategy, NULL, dev, B_READ, minphys, uio, (int)su.secsize);
}

static int
sweet_ufs_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (!su.writable) {
		return EROFS;
	}
	return physio(sweet_ufs_strategy, NULL, dev, B_WRITE, minphys, uio, (int)su.secsize);
}

static const struct bdevsw sweet_ufs_bdevsw = {
	.d_open     = sweet_ufs_open,
	.d_close    = sweet_ufs_close,
	.d_strategy = sweet_ufs_strategy,
	.d_ioctl    = sweet_ufs_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = sweet_ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw sweet_ufs_cdevsw = {
	.d_open       = sweet_ufs_open,
	.d_close      = sweet_ufs_close,
	.d_read       = sweet_ufs_read,
	.d_write      = sweet_ufs_write,
	.d_ioctl      = sweet_ufs_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/* Fail closed before any MMIO mapping or controller access. The loader has
 * independently checked and reserved these EFI Conventional pages. ADT alone
 * cannot establish firmware ownership; the kernel validates its fixed contract. */
static int
ufs_lowdma_configure(DTEntry node, const uintptr_t *reg, unsigned reg_size,
    uint64_t pool[4])
{
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("ufs-lowdma", &enabled, sizeof(enabled))) return 0;
    if (enabled != 1) return EINVAL;
    uint32_t mode=0, cpus=UINT32_MAX, rw=UINT32_MAX, rootrw=UINT32_MAX, log=UINT32_MAX, trace=0;
    PE_parse_boot_argn("umi-rootfs",&mode,sizeof(mode));
    PE_parse_boot_argn("cpus",&cpus,sizeof(cpus));
    PE_parse_boot_argn("ufsrw",&rw,sizeof(rw));
    PE_parse_boot_argn("rootrw",&rootrw,sizeof(rootrw));
    PE_parse_boot_argn("sweetlog",&log,sizeof(log));
    PE_parse_boot_argn("ufstrace",&trace,sizeof(trace));
    if(!umi_cpu_boot_valid(cpus) || !umi_root_mode_valid(mode,cpus,rw,rootrw,log,trace)) return EPERM;
    if(cpus>1 && ml_get_cpu_count()!=cpus)return EPERM;
    printf("UMI GUI CPU requested=%u topology_count=%u\n",cpus,ml_get_cpu_count());
    umi_rootfs_mode=true;
    DTEntry root;
    void const *bytes = NULL;
    unsigned size = 0;
    if (SecureDTLookupEntry(NULL, "/", &root) != kSuccess ||
        SecureDTGetProperty(root, "compatible", &bytes, &size) != kSuccess ||
        !umi_lowdma_is_umi(bytes, size) || reg_size != 2 * sizeof(uintptr_t) ||
        reg[0] != 0x1d84000ULL || reg[1] != 0x3000ULL)
        return EINVAL;
    if (SecureDTGetProperty(node, "dma-pool", &bytes, &size) != kSuccess ||
        size != 4 * sizeof(uint64_t)) return EINVAL;
    /* DeviceTree properties are only 4-byte aligned. Avoid u64 dereferences. */
    memcpy(pool, bytes, 4 * sizeof(uint64_t));
    if (!umi_lowdma_pool_valid(pool, gPhysBase, gPhysSize)) return EINVAL;
    return 0;
}


/* Mi10-specific volatile transport setup, gated independently of disk writes.
 * First qualification uses umi-smmu-probe=1: zero controller/SMMU writes,
 * return before any SCSI submission. Apply requires a separate explicit flag.
 */
static bool umi_smmu_requested, umi_smmu_probe;
static vm_offset_t umi_smmu_regs;
static I16SmmuPlan umi_smmu_plan;
static uint32_t umi_smmu_rd(void *ctx, unsigned off) {
    return *(volatile uint32_t *)((vm_offset_t)ctx + off);
}
static void umi_smmu_wr(void *ctx, unsigned off, uint32_t value) {
    ufs_trace_value("SMMU write offset", off);
    ufs_trace_value("SMMU write value", value);
    *(volatile uint32_t *)((vm_offset_t)ctx + off) = value;
    __builtin_arm_dsb(0xf);
}
static int umi_smmu_inspect(void) {
    uint32_t probe=0, apply=0;
    bool has_probe=PE_parse_boot_argn("umi-smmu-probe", &probe, sizeof(probe));
    bool has_apply=PE_parse_boot_argn("umi-smmu", &apply, sizeof(apply));
    if (!has_probe && !has_apply) return 0;
    if ((has_probe && probe!=1) || (has_apply && apply!=1) || (has_probe && has_apply)) return EINVAL;
    if (!ufs_lowdma_enabled) return EPERM; /* proves umi/CPU/explicit root mode/ownedpool */
    bool saved=ufs_trace_active; ufs_trace_active=true;
    vm_offset_t gcc=ml_io_map(0x177000,4096);
    if (!gcc) {ufs_trace_active=saved; return ENOMEM;}
    uint32_t gdsc=umi_smmu_rd((void *)gcc,4), axi=umi_smmu_rd((void *)gcc,0x10), ahb=umi_smmu_rd((void *)gcc,0x18);
    ufs_trace_value("UMI GCC GDSCR",gdsc);ufs_trace_value("UMI GCC AXI",axi);ufs_trace_value("UMI GCC AHB",ahb);
    if (!(gdsc&0x80000000U) || !(axi&1) || !(ahb&1)) {
        ufs_trace_step("UMI UFS power/clocks refused before SMMU access");ufs_trace_active=saved;return ENXIO;
    }
    umi_smmu_regs=ml_io_map(0x15000000,0xd0000);
    if (!umi_smmu_regs) {ufs_trace_active=saved;return ENOMEM;}
    void *ctx=(void *)umi_smmu_regs;
    for(unsigned off=0x20;off<=0x28;off+=4) {ufs_trace_value("SMMU ID register offset",off);ufs_trace_value("SMMU ID value",umi_smmu_rd(ctx,off));}
    umi_smmu_plan=i16_smmu_plan(su.dma_pa,DMA_SIZE,umi_smmu_rd,ctx);
    I16SmmuPlan *p=&umi_smmu_plan;
    ufs_trace_value("SMMU plan error",p->error);ufs_trace_value("SMMU bank",p->bank);ufs_trace_value("SMMU pages",p->pages);ufs_trace_value("SMMU slots count",p->count);
    if(p->error!=1 && p->count && p->count<=128) for(unsigned i=0;i<p->count;i++) {
        uint32_t smr=umi_smmu_rd(ctx,0x800+4*i);
        if(!(smr&0x80000000U)) continue;
        unsigned mask=(smr>>16)&0x7fff;
        if((((smr^0xe0)&0xffff&~mask)==0) || (((smr^0x4e0)&0xffff&~mask)==0)) {
            ufs_trace_value("SMMU matching slot",i);ufs_trace_value("SMMU SMR",smr);ufs_trace_value("SMMU S2CR",umi_smmu_rd(ctx,0xc00+4*i));
        }
    }
    if (!p->error) {
        ufs_trace_value("SMMU route0 slot",p->slot[0]);ufs_trace_value("SMMU route1 slot",p->slot[1]);
        ufs_trace_value("SMMU reuse0",p->reuse[0]);ufs_trace_value("SMMU reuse1",p->reuse[1]);
        ufs_trace_value("SMMU selected SCTLR",umi_smmu_rd(ctx,(p->pages+p->bank)*4096));
        ufs_trace_value("SMMU selected CBAR",umi_smmu_rd(ctx,0x1000+4*p->bank));
        ufs_trace_value("SMMU selected CBA2R",umi_smmu_rd(ctx,0x1800+4*p->bank));
    }
    ufs_trace_value("UMI HCE before plan",ufs_rd(REG_HCE));ufs_trace_value("UMI HCS before plan",ufs_rd(REG_HCS));
    ufs_trace_value("UMI transfer DBR",ufs_rd(REG_UTRLDBR));ufs_trace_value("UMI task DBR",ufs_rd(0x78));ufs_trace_value("UMI task RSR",ufs_rd(0x80));
    ufs_trace_step("UMI SMMU read-only inspection finished; no SMMU/queue writes");
    ufs_trace_active=saved;
    if (p->error) return ENXIO;
    if (has_probe) return EAGAIN; /* expected diagnostic termination before TUR */
    umi_smmu_requested=true; umi_smmu_probe=false;return 0;
}
static int umi_smmu_takeover(void) {
    if (!umi_smmu_requested || umi_smmu_probe) return 0;
    bool saved=ufs_trace_active;ufs_trace_active=true;
    if (!(ufs_rd(REG_HCE)&1) || (ufs_rd(REG_HCS)&0xf)!=0xf || ufs_rd(REG_UTRLDBR) || ufs_rd(0x78) || ufs_rd(0x80) || (ufs_rd(REG_IS)&0x71880U)) {
        ufs_trace_step("UMI queue ownership refused; no SMMU writes");ufs_trace_active=saved;return EBUSY;
    }
    ufs_wr(REG_IE,0);ufs_wr(REG_UTRLRSR,0);__builtin_arm_dsb(0xf);
    uint32_t spins=0;while((ufs_rd(REG_UTRLRSR)&1) && spins++<1000000) {}
    if(ufs_rd(REG_UTRLRSR)&1) {ufs_trace_active=saved;return ETIMEDOUT;}
    void *ctx=(void *)umi_smmu_regs;
    I16SmmuPlan p=i16_smmu_plan(su.dma_pa,DMA_SIZE,umi_smmu_rd,ctx);
    if(p.error) {ufs_trace_value("SMMU replan refused",p.error);ufs_trace_active=saved;return ENXIO;}
    unsigned result=i16_smmu_apply(&p,umi_smmu_rd,umi_smmu_wr,ctx);
    ufs_trace_value("SMMU dual stream apply result",result);
    ufs_trace_active=saved;return result?EIO:0;
}

static int
sweet_ufs_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	char want[64] = "userdata";
	int error;

	if (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("SWEET UFS: no /sweet-ufs node in the device tree\n");
		return ENXIO;
	}
	printf("UFS: dt");
	PE_parse_boot_argn("ufsroot", want, sizeof(want));
	uint32_t rw = 0;
	PE_parse_boot_argn("ufsrw", &rw, sizeof(rw));
    uint32_t trace = 0;
    PE_parse_boot_argn("ufstrace", &trace, sizeof(trace));
    ufs_trace_enabled = (trace == 1);
    if (ufs_trace_enabled && rw != 0) {
        printf("SWEET UFS: trace diagnostic requires ufsrw=0\n");
        return EPERM;
    }
    uint64_t low_pool[4] = { 0 };
    error = ufs_lowdma_configure(node, reg, size, low_pool);
    if (error) {
        printf("SWEET UFS: low DMA contract rejected, error=%d\n", error);
        return error;
    }
    ufs_lowdma_enabled = (low_pool[3] == DMA_SIZE);
	uint32_t cache = 0, wait = 0;
	PE_parse_boot_argn("ufscache", &cache, sizeof(cache));
	PE_parse_boot_argn("ufswait", &wait, sizeof(wait));
	su.cache = (cache == 1);
	su.wait = (wait == 1);

	su.regs = ml_io_map(reg[0], reg[1]);
	printf(" map");
    if (ufs_lowdma_enabled) {
        printf(" low DMA alloc=0x%llx+0x%llx NC=0x%llx+0x%llx",
            low_pool[0], low_pool[1], low_pool[2], low_pool[3]);
        su.dma = ml_io_map_wcomb(low_pool[2], DMA_SIZE);
        if (!su.dma) return ENOMEM;
        su.dma_pa = low_pool[2];
        bzero((void *)su.dma, DMA_SIZE);
        __builtin_arm_dsb(0xf);
    } else {
	if (kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT | KMA_NOPAGEWAIT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET UFS: no contiguous DMA memory\n");
		return ENOMEM;
	}
	printf(" dma");
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)su.dma);
	if (!pn) {
		return ENOMEM;
	}
	su.dma_pa = ptoa_64(pn);
    }
	printf(" pa=0x%llx", su.dma_pa);
	su.lun = 0;
	su.secsize = DEV_BSIZE;

	error=umi_smmu_inspect();
	if(error) return error;
	lck_mtx_lock(&sweet_ufs_lock);
	error = ufs_take_over_host();
	printf(" to=%d", error);
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention. */
		for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
		}
		printf(" tur=%d", error);
	}
	if (!error) {
		error = ufs_read_capacity();
        if (!error) error = ufs_find_partition(want);
        if (!error) error = ufs_validate_root_image();
        if (!error) error = ufs_check_head();
		printf(" find=%d", error);
	}
	lck_mtx_unlock(&sweet_ufs_lock);
	if (error) {
		return error;
	}

	su.bmajor = bdevsw_add(-1, &sweet_ufs_bdevsw);
	if (su.bmajor < 0) {
		return ENXIO;
	}
	su.cmajor = cdevsw_add_with_bdev(-1, &sweet_ufs_cdevsw, su.bmajor);
	if (su.cmajor < 0) {
		return ENXIO;
	}
	su.bdev = makedev(su.bmajor, 0);
	devfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
	devfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
	/* Only now, with the partition bounds known, can writes be allowed. */
	su.writable = (rw != 0);
	su.ready = true;
	printf("SWEET UFS: \"%s\" on LUN %u: first block 0x%llx, %llu MiB, %s, DMA at 0x%llx\n",
	    want, su.lun, su.first_block, su.nblocks * UFS_BLOCK >> 20,
	    su.writable ? "read-write" : "read-only", su.dma_pa);
	sweet_log_start();
	/* SWEET: the stack sampler is pure software; keep it running so a hung
	 * SpringBoard/backboardd shows where it is stuck (0054). */
	sweet_sample_start();
	/* Mi10 inputs start only after UFS SMMU and bounded root setup. */
	extern void umi_sweet_touch_start(void);
	umi_sweet_touch_start();
	/* SWEET: hardware bring-up hooks (keys, touch, USB) are staged
	 * separately; they hang while their buses are not initialized yet. */
#if 0
	sweet_keys_init();
	sweet_touch_init();
	sweet_usb_probe();
#endif
	return 0;
}

/*
 * Persistent console log ("sweetlog"). Filming the phone screen is the only
 * other way to see the console, so every character that reaches the
 * console (kernel printf and /dev/console writes of daemons, both through
 * _cnputs) is also kept in a 4 MiB ring, and once the partition is known
 * and writable a thread copies new ring blocks to the LAST 4 MiB + 4 KiB of
 * the partition every ufslogsecs seconds (default 10), far behind the HFS
 * image at its start. Device block 0 of that area is a header:
 *   "SWEETLOG" | u32 version 1 | u32 ring size | u64 total bytes written
 *   | u64 flush sequence
 * ring offset of byte n = n % ring size. Read it from a recovery with
 * tools/sweet-log-pull.sh. Persistent writes require explicit sweetlog=1.
 * The in-memory console ring remains available regardless of this setting.
 */
#define SWEET_LOG_SIZE          (4u << 20)
#define SWEET_LOG_BLOCKS        (1 + SWEET_LOG_SIZE / UFS_BLOCK)
#define SWEET_LOG_SECS          10u

static char sweet_log_ring[SWEET_LOG_SIZE];
static uint64_t _Atomic sweet_log_total;
static int sweet_log_chan;
static uint32_t sweet_log_secs = 60;
/* Local umi safety fix: kern_exec can call sweet_log_kick independently of
 * sweet_log_start, so gate every disk entry path, not just initial capture. */
static bool sweet_log_disk_enabled;

static void sweet_log_flush_to_disk(void);
static int sweet_log_thread_started;
void sweet_log_kick(void);

/* Called for all console output, any context: never blocks. */
void sweet_log_append(const char *c, int size);

void
sweet_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = os_atomic_add_orig(&sweet_log_total, (uint64_t)size, relaxed);
	for (int i = 0; i < size; i++) {
		sweet_log_ring[(at + (uint64_t)i) % SWEET_LOG_SIZE] = c[i];
	}
	/* Never do I/O here: the console ticket lock is held while this runs and
	 * a preemption inside a disk write would freeze the kernel on one CPU.
	 * Only wake the flusher thread, and only once it has been started. */
	{
		static uint64_t _Atomic woken;
		uint64_t w = os_atomic_load(&woken, relaxed);
		if (sweet_log_thread_started && at - w >= 262144 &&
		    os_atomic_cmpxchg(&woken, w, at, relaxed)) {
			wakeup(&sweet_log_chan);
		}
	}
}

/*
 * Copy console bytes [*pos, total) (at most max) for live log streaming
 * (sweet_usb.c). A reader that fell more than the ring behind skips ahead.
 * Returns the byte count; *pos advances.
 */
uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);

uint32_t
sweet_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint64_t total = os_atomic_load(&sweet_log_total, relaxed);
	if (total - *pos > SWEET_LOG_SIZE) {
		*pos = total - SWEET_LOG_SIZE;
	}
	uint32_t n = (uint32_t)MIN((uint64_t)max, total - *pos);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = sweet_log_ring[(*pos + i) % SWEET_LOG_SIZE];
	}
	*pos += n;
	return n;
}

uint64_t sweet_log_position(void);

uint64_t
sweet_log_position(void)
{
	return os_atomic_load(&sweet_log_total, relaxed);
}

struct sweet_log_header {
	char magic[8];
	uint32_t version, size;
	uint64_t total, seq;
};

/* Print the tail of a file (boot-arg sweetdumpfile=PATH) to the console. */
static char sweet_dump_buf[4096];

static void
sweet_dump_file(void)
{
	char path[128];
	uint32_t want = 2048;
	vnode_t vp = NULLVP;
	off_t size = 0;
	int error, resid = 0;

	if (!PE_parse_boot_argn("sweetdumpfile", path, sizeof(path)) || !path[0]) {
		return;
	}
	if (!PE_parse_boot_argn("sweetdumplen", &want, sizeof(want)) ||
	    want == 0 || want > sizeof(sweet_dump_buf)) {
		want = 2048;
	}
	error = vnode_lookup(path, 0, &vp, vfs_context_kernel());
	if (error || vp == NULLVP) {
		printf("SWEET DUMP: lookup %s failed (%d)\n", path, error);
		return;
	}
	error = vnode_size(vp, &size, vfs_context_kernel());
	if (error || size <= 0) {
		printf("SWEET DUMP: %s size %lld error %d\n", path, (long long)size, error);
		vnode_put(vp);
		return;
	}
	off_t off = size > (off_t)want ? size - (off_t)want : 0;
	error = vn_rdwr(UIO_READ, vp, (caddr_t)sweet_dump_buf, (int)want, off,
	    UIO_SYSSPACE, IO_NODELOCKED, kauth_cred_get(), &resid, current_proc());
	if (error) {
		printf("SWEET DUMP: read %s failed (%d)\n", path, error);
		vnode_put(vp);
		return;
	}
	printf("SWEET DUMP: %s (size %lld, last %u bytes):\n", path, (long long)size, want);
	{
		char line[100];
		uint32_t n = 0;
		for (uint32_t i = 0; i < want; i++) {
			char c = sweet_dump_buf[i];
			if (c == '\n' || n >= sizeof(line) - 1) {
				line[n] = 0;
				if (n) {
					printf("%s\n", line);
				}
				n = 0;
				continue;
			}
			if (c >= 32 && c < 127) {
				line[n++] = c;
			}
		}
		if (n) {
			line[n] = 0;
			printf("%s\n", line);
		}
	}
	vnode_put(vp);
}

/* Capture the tail of the previous session's persistent log. */
static char sweet_log_prev[512];
static uint32_t sweet_log_prev_len;
static bool sweet_log_prev_valid;

static void
sweet_log_capture(void);

static void
sweet_log_prev_print(void);

static void
sweet_log_capture(void)
{
	uint64_t base = su.first_block + su.nblocks - SWEET_LOG_BLOCKS;
	uint8_t *bounce = (uint8_t *)(su.dma + DMA_BOUNCE);
	struct sweet_log_header *h = (struct sweet_log_header *)bounce;

	if (!sweet_log_disk_enabled || !su.writable || ufs_read_blocks(base, 1)) {
		return;
	}
	if (h->magic[0] != 'S' || h->magic[7] != 'G' || h->size == 0 ||
	    h->size > SWEET_LOG_SIZE || h->total == 0) {
		return;
	}
	uint64_t last = (h->total - 1) / UFS_BLOCK % (h->size / UFS_BLOCK);
	if (ufs_read_blocks(base + 1 + last, 1)) {
		return;
	}
	memcpy(sweet_log_prev, bounce + UFS_BLOCK - 512, sizeof(sweet_log_prev));
	sweet_log_prev_len = sizeof(sweet_log_prev);
	sweet_log_prev_valid = true;
}

static void
sweet_log_prev_print(void)
{
	char line[80];
	uint32_t n = 0;

	if (!sweet_log_prev_valid || sweet_log_prev_len == 0) {
		return;
	}
	printf("SWEET LOG: previous session tail:\n");
	for (uint32_t i = 0; i < sweet_log_prev_len; i++) {
		char c = sweet_log_prev[i];
		if (c == '\n' || n >= sizeof(line) - 1) {
			line[n] = 0;
			if (n) {
				printf("%s\n", line);
			}
			n = 0;
			continue;
		}
		if (c >= 32 && c < 127) {
			line[n++] = c;
		}
	}
	if (n) {
		line[n] = 0;
		printf("%s\n", line);
	}
	sweet_log_prev_valid = false;
}

/* launchd opens StandardOutPath=/dev/console for daemons that run as user
 * 501; the console node is 0600 root, so that open fails with EACCES and
 * posix_spawn fails for every service. Make it world writable.
 */
static void
sweet_fix_console(void)
{
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	int error;

	error = vnode_lookup("/dev/console", 0, &vp, vfs_context_kernel());
	if (error || vp == NULLVP) {
		printf("SWEET CON: lookup /dev/console failed (%d)\n", error);
		return;
	}
	VATTR_INIT(&va);
	VATTR_SET(&va, va_mode, 0666);
	error = vnode_setattr(vp, &va, vfs_context_kernel());
	printf("SWEET CON: chmod /dev/console 0666 -> %d\n", error);
	vnode_put(vp);
}

static uint64_t sweet_log_flushed;
static uint64_t sweet_log_seq;
static int sweet_log_flushing;

/* Write the ring to the tail of the partition. Synchronous and re-entrancy
 * protected; called from the console path every 8 KiB and once at start-up. */
static void
sweet_log_flush_to_disk(void)
{
	uint64_t base, total, flushed, pos;
	void *bounce;
	int error = 0;

	if (!sweet_log_disk_enabled || !su.writable || su.nblocks <= SWEET_LOG_BLOCKS) {
		return;
	}
	if (!os_atomic_cmpxchg(&sweet_log_flushing, 0, 1, relaxed)) {
		return;
	}
	base = su.first_block + su.nblocks - SWEET_LOG_BLOCKS;
	bounce = (void *)(su.dma + DMA_BOUNCE);
	total = os_atomic_load(&sweet_log_total, relaxed);
	flushed = sweet_log_flushed;
	if (total != flushed) {
		pos = total - flushed >= SWEET_LOG_SIZE ? total - SWEET_LOG_SIZE : flushed;
		/* Never block here: this runs inside the console path, while the
		 * driver may print (wanting the console lock) with this lock held.
		 * Blocking would invert the order and deadlock. */
		if (!lck_mtx_try_lock(&sweet_ufs_lock)) {
			os_atomic_store(&sweet_log_flushing, 0, relaxed);
			return;
		}
		while (pos < total && !error) {
			uint32_t off = (uint32_t)(pos % SWEET_LOG_SIZE);
			uint32_t blk = off / UFS_BLOCK;
			uint64_t need = ((uint64_t)off % UFS_BLOCK + (total - pos) + UFS_BLOCK - 1) / UFS_BLOCK;
			uint32_t n = (uint32_t)MIN(MIN(need, (uint64_t)(SWEET_LOG_SIZE / UFS_BLOCK - blk)),
			    (uint64_t)(BOUNCE_SIZE / UFS_BLOCK));
			memcpy(bounce, sweet_log_ring + (uint64_t)blk * UFS_BLOCK, n * UFS_BLOCK);
			error = ufs_write_blocks(base + 1 + blk, n);
			pos += (uint64_t)(blk + n) * UFS_BLOCK - off;
		}
		if (!error) {
			struct sweet_log_header h = {
				.magic = { 'S', 'W', 'E', 'E', 'T', 'L', 'O', 'G' },
				.version = 1, .size = SWEET_LOG_SIZE, .total = total,
				.seq = ++sweet_log_seq,
			};
			bzero(bounce, UFS_BLOCK);
			memcpy(bounce, &h, sizeof(h));
			error = ufs_write_blocks(base, 1);
		}
		if (!error) {
			error = ufs_sync_cache();
		}
		lck_mtx_unlock(&sweet_ufs_lock);
		/* No printing here: this runs with the console lock held (the flush is
		 * triggered from the console output path), and a nested console write
		 * would deadlock on the non-recursive lock. */
		if (!error) {
			sweet_log_flushed = total;
		}
	}
	os_atomic_store(&sweet_log_flushing, 0, relaxed);
}

static void
sweet_log_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		/* A time stamp in the ring, so the persisted log shows how far the
		 * kernel got and when it stopped. Appending here is safe: this is the
		 * ring, not the console, and it takes no lock. */
		char tick[64];
		int n = snprintf(tick, sizeof(tick), "\nSWEET TICK %llu\n",
		    (unsigned long long)mach_absolute_time());
		if (n > 0) {
			sweet_log_append(tick, n);
		}
		sweet_log_flush_to_disk();
		/*
		 * Wake on output, and in any case after sweet_log_secs: an absolute
		 * deadline does not depend on hz being initialised (it is not, at the
		 * point where this thread is first started). The interval is a boot
		 * argument so a slow rate can be compared with a fast one: this write
		 * shares the block driver with HFS+, and a stuck write here freezes
		 * every reader and writer in the system with it.
		 */
		uint64_t deadline;
		clock_interval_to_deadline(sweet_log_secs, NSEC_PER_SEC, &deadline);
		assert_wait_deadline(&sweet_log_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* Started at the first exec (kern_exec.c): by then the scheduler runs and hz
 * is valid, so the sleeping flusher works and the console path stays I/O free. */
void
sweet_log_kick(void)
{
	thread_t thread;

	if (!sweet_log_disk_enabled || sweet_log_thread_started || !su.writable ||
	    su.nblocks <= SWEET_LOG_BLOCKS) {
		return;
	}
	sweet_log_thread_started = 1;
	if (kernel_thread_start(sweet_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	{
		extern void sweet_diag_display(void);
		sweet_diag_display();
	}
}

static void
sweet_log_start(void)
{
	uint32_t on = 0;

	PE_parse_boot_argn("sweetlog", &on, sizeof(on));
	if (on != 1 || !su.writable || su.nblocks < 2 * SWEET_LOG_BLOCKS) {
		return;
	}
	sweet_log_disk_enabled = true;
	{
		uint32_t secs = 60;
		PE_parse_boot_argn("ufslogsecs", &secs, sizeof(secs));
		if (secs >= 1 && secs <= 600) {
			sweet_log_secs = secs;
		}
	}
	sweet_log_capture();
	printf("SWEET LOG: console log to the last %u KiB of the partition (block 0x%llx)\n",
	    SWEET_LOG_BLOCKS * UFS_BLOCK >> 10,
	    su.first_block + su.nblocks - SWEET_LOG_BLOCKS);
	if (sweet_log_prev_valid) {
		sweet_log_prev_print();
	}
	if (0) {
		sweet_dump_file();
	}
	sweet_log_flush_to_disk();
}

/* Root device for rd=ufsroot0 (IOFindBSDRoot). */
int sweet_ufs_rootdev(dev_t *dev);

int
sweet_ufs_rootdev(dev_t *dev)
{
	if (!su.ready) {
		int error = sweet_ufs_init();
		if (error) {
			return error;
		}
	}
	*dev = su.bdev;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
