/*
 * SweetGPU: apollo's Adreno 650 for programs to draw with.
 *
 * bsd/dev/sweet_gpu.c brings the GPU up (boot-arg sweetgpu=21 or 22) and,
 * with sweetgpukeep=1, keeps it on and starts this service. A program that
 * opens it speaks the interface of Linux's msm DRM driver, the one Mesa's
 * freedreno knows: every request is a call of the user client with the
 * request's number (the low byte of the ioctl code) as the selector and the
 * request's structure passed in and back; a buffer's memory is the client
 * memory of the type that is the buffer's handle. This is the ABI of HackiOS's
 * RedwoodGPU (github.com/oosexgang/hackios-xnu-kernel, iokit/Kernel/
 * RedwoodGPU.cpp, APSL 2.0), from which this is ported, so its userland
 * (tools/mesa/redwood-drm, which stands in for libdrm inside Mesa) works
 * against it once it matches "SweetGPU" instead of "RedwoodGPU".
 *
 * What there is: parameters, buffers (made, told their address in the GPU's
 * address space, mapped, given back), submission of command buffers, waiting
 * for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context 0; it is write-combined, so what the program writes goes
 * to memory, where the GPU reads it (as Linux maps them for freedreno).
 *
 * Umi extension: selector 0x7e imports/describes the local SweetSurface ABI,
 * whose state and pixels are separate descriptors. The imported descriptor
 * is prepared and held with its surface until its last fence completes.
 * SweetGPUWaitForSurface() joins those fences before CPU display reads.
 * Unaligned or read-only wrapped memory is rejected for GPU import.
 *
 * Not here yet (HackiOS has them): a cache of given-back buffers, interrupts
 * (waiting polls every 200 us), recovery of a hung core, per-client address
 * spaces (every client's buffers are in the one context, as in HackiOS).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include "SweetSurface.h"
#include <IOKit/system.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/proc.h>

extern "C" {
/* bsd/dev/sweet_gpu.c, "the GPU kept on": called with gGPULock held */
bool sweet_gpu_ready(void);
bool sweet_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t sweet_gpu_address(uint64_t size);
void sweet_gpu_address_free(uint64_t address, uint64_t size);
bool sweet_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void sweet_gpu_mapped(void);
void sweet_gpu_unmap(uint64_t address, uint64_t size);
void sweet_gpu_to_memory(void *start, uint64_t size);
uint32_t sweet_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool sweet_gpu_done(uint32_t fence);
uint32_t sweet_gpu_last(void);
void sweet_gpu_report(const char *why);

void SweetGPUStart(void);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kSweetSurface           = 0x7e,
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
/* Same 40-byte ABI as HackiOS redwood_drm_surface; pixels start at
 * memory+wrapDelta, never at a fabricated PAGE_SIZE shared-state prefix. */
struct sweet_gpu_surface {
	uint32_t surface, handle;
	uint64_t size;
	uint32_t offset, rowBytes, width, height, pixelFormat, planes;
};
static_assert(sizeof(struct sweet_gpu_surface) == 40, "surface ABI");
#define SWEET_GPU_DESCRIBE 0xffffffffu

struct drm_gem_close {
	uint32_t handle, pad;
};

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             4096            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          4096
#define GPU_PATIENCE_MS         5000            /* for a wait without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting: how long between two looks at the fence */

static IOLock *gGPULock;                        /* around everything that is the GPU's (sweet_gpu_...) */
static bool gGPUStopped;                        /* the core stopped answering: nothing is waited for again */

/* A live import owns the surface through this record. A timed-out BO
 * deliberately keeps its descriptor prepared and its record alive: DMA may
 * still write it. Records survive a closed client's fBuffers in that case. */
struct gpu_surface_sync {
    SweetSurface *surface;
    uint32_t fence;
    struct gpu_surface_sync *next;
};
static struct gpu_surface_sync *gSurfaceSync;

static void
gpuSurfaceUnlink(struct gpu_surface_sync *entry)
{
    struct gpu_surface_sync **link = &gSurfaceSync;
    while (*link != NULL && *link != entry) link = &(*link)->next;
    if (*link == entry) *link = entry->next;
    entry->surface->release();
    IODelete(entry, struct gpu_surface_sync, 1);
}

/* Wait for submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (;;) {
		if (sweet_gpu_done(fence)) {
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUStopped) {
			break;
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	/* a wait of a second or more that came to nothing: the core has stopped; said once, and not waited for again */
	if (milliseconds >= 1000 && !gGPUStopped) {
		sweet_gpu_report("no answer");
		gGPUStopped = true;
	}
	return false;
}

/* Called before display/other CPU reads. The caller retains surface.
 * Capture only fence values before gpuWait(), which drops the global lock;
 * a concurrent client close may remove list entries while it waits. */
extern "C" bool
SweetGPUWaitForSurface(SweetSurface *surface, uint64_t milliseconds)
{
    if (surface == NULL || gGPULock == NULL) return true;
    IOLockLock(gGPULock);
    uint32_t fence = 0;
    for (struct gpu_surface_sync *e = gSurfaceSync; e != NULL; e = e->next) {
        if (e->surface == surface && e->fence != 0 &&
            (fence == 0 || (int32_t)(e->fence - fence) > 0)) fence = e->fence;
    }
    bool done = fence == 0 || gpuWait(fence, milliseconds > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : milliseconds);
    if (done && fence != 0) {
        uintptr_t address = surface->kernelAddress();
        if (address == 0) done = false;
        else sweet_gpu_to_memory((void *)address, surface->allocSize);
    }
    IOLockUnlock(gGPULock);
    return done;
}

/* How long from now a time of msm's is, in milliseconds (at most GPU_PATIENCE_MS); 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec *when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t)NSEC_PER_SEC + when->nanoseconds - (int64_t)now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t)left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t)left / NSEC_PER_MSEC;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class SweetGPU : public IOService
{
	OSDeclareDefaultStructors(SweetGPU);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGPU, IOService);

bool
SweetGPU::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "SweetGPUUserClient");
	setName("SweetGPU");
	registerService();
	IOLog("SweetGPU: registered: the GPU is offered to programs\n");
	return true;
}

/* From sweet_gpu.c once the GPU is kept on. */
void
SweetGPUStart(void)
{
	IOService *provider = IOService::getPlatform();
	SweetGPU *gpu;

	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(SweetGPU);
	if (gGPULock == NULL || gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("SweetGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("SweetGPU: the service did not start\n");
	}
	gpu->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *memory;             /* NULL: this handle is free */
    uintptr_t kernel;                     /* base of the GPU-mapped pixels */
    struct gpu_surface_sync *sync;         /* retained IOSurface, if imported */
    bool prepared;
	uint64_t address;                       /* the GPU's for it */
	uint64_t size;
	uint32_t flags;
	uint32_t fence;                         /* the last submission that names it (0: none has) */
};

class SweetGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(SweetGPUUserClient);

	struct gpu_buffer *fBuffers;            /* handle n is fBuffers[n - 1] */
    IOLock *fCallLock;                     /* serialize handles while gpuWait drops gGPULock */
    bool fClosing;
	uint32_t fNextBuffer;                   /* where the search for a free handle starts */
	uint32_t fNextQueue;
	uint32_t fSubmits;
	uint32_t fStrangers;                    /* requests that named a buffer the client does not have */

	struct gpu_buffer *buffer(uint32_t handle);
	void freeBuffer(struct gpu_buffer *b);
	IOReturn gemNew(struct msm_gem_new *request);
	IOReturn gemInfo(struct msm_gem_info *request);
    IOReturn surfaceBuffer(struct sweet_gpu_surface *request);
    IOReturn cpuPrep(const struct msm_gem_cpu_prep *request);
    IOReturn call(uint32_t selector, IOExternalMethodArguments *args);
	IOReturn submit(struct msm_gem_submit *request);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGPUUserClient, IOUserClient);

bool
SweetGPUUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type)) return false;
    fCallLock = IOLockAlloc();
    return fCallLock != NULL;
}

bool
SweetGPUUserClient::start(IOService *provider)
{
	if (gGPULock == NULL || !sweet_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

struct gpu_buffer *
SweetGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("SweetGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * With the lock held. A buffer the core may still be using (it has stopped
 * answering) stays the GPU's for good: its memory may yet be written, and
 * must not be somebody else's by then.
 */
void
SweetGPUUserClient::freeBuffer(struct gpu_buffer *b)
{
	if (b->fence != 0 && !gpuWait(b->fence, 1000)) {
		static uint32_t said;

		if (said++ < 8) {
			IOLog("SweetGPU: the GPU is not done with a buffer of %llu bytes: it stays its own\n", b->size);
		}
	} else {
		sweet_gpu_unmap(b->address, b->size);
		sweet_gpu_address_free(b->address, b->size);
        if (b->prepared) (void)b->memory->complete();
        b->memory->release();
        if (b->sync != NULL) gpuSurfaceUnlink(b->sync);
    }
    /* If the fence timed out, memory, preparation and surface ref deliberately
     * remain live. Do not let free() or a reused handle release in-flight DMA. */
    bzero(b, sizeof(*b));
}

IOReturn
SweetGPUUserClient::clientClose(void)
{
    IOLockLock(fCallLock);
    fClosing = true;
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		IOLockUnlock(gGPULock);
	}
    IOLockUnlock(fCallLock);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
SweetGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
        /* clientClose is normal but not the only destruction route. */
        IOLockLock(gGPULock);
        for (uint32_t i = 0; i < GPU_BUFFERS; ++i)
            if (fBuffers[i].memory != NULL) freeBuffer(&fBuffers[i]);
        IOLockUnlock(gGPULock);
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
    if (fCallLock != NULL) { IOLockFree(fCallLock); fCallLock = NULL; }
	IOUserClient::free();
}

IOReturn
SweetGPUUserClient::gemNew(struct msm_gem_new *request)
{
	uint64_t size = round_page_64(request->size);
	struct gpu_buffer *b = NULL;
	IOBufferMemoryDescriptor *memory;
	bool mapped = true;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* write-combined, as Linux maps freedreno's buffers: the program's writes go to memory, where the GPU reads */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache, (vm_size_t)size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("SweetGPU: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}

	IOLockLock(gGPULock);
	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			request->handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = sweet_gpu_address(size)) == 0) {
		IOLockUnlock(gGPULock);
		IOLog("SweetGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		memory->release();
		return kIOReturnNoMemory;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && sweet_gpu_map(b->address + offset, phys, (request->flags & MSM_BO_GPU_READONLY) != 0);
	}
	sweet_gpu_mapped();
	if (!mapped) {
		sweet_gpu_unmap(b->address, size);
		sweet_gpu_address_free(b->address, size);
		IOLockUnlock(gGPULock);
		IOLog("SweetGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
    b->kernel = (uintptr_t)memory->getBytesNoCopy();
    b->sync = NULL;
    b->prepared = false;
	b->size = size;
	b->flags = request->flags;
	b->fence = 0;
	IOLockUnlock(gGPULock);
	return kIOReturnSuccess;
}

/* Import only complete 4 KiB pages owned by the descriptor. Never round
 * beyond its length and never map the separate state page as image pixels. */
static bool
gpuSurfaceRange(uint64_t length, uint64_t delta, uint64_t alloc, uint64_t *rounded)
{
    if (alloc == 0 || alloc > GPU_BUFFER_MAX || (delta & 4095u) != 0 ||
        delta > length || alloc > length - delta || alloc > UINT64_MAX - 4095u) return false;
    uint64_t size = (alloc + 4095u) & ~4095ull;
    if (size > length - delta) return false;
    *rounded = size;
    return true;
}

IOReturn
SweetGPUUserClient::surfaceBuffer(struct sweet_gpu_surface *request)
{
    SweetSurface *surface = sweet_surface_copy(request->surface);
    IOMemoryDescriptor *memory = NULL;
    struct gpu_surface_sync *sync = NULL;
    struct gpu_buffer *b = NULL;
    uint64_t size = 0, address = 0;
    uintptr_t kernel = 0;
    IOReturn ret = kIOReturnBadArgument;
    bool prepared = false, entered = false;

    if (surface == NULL) return kIOReturnNotFound;
    /* Layout is immutable while the retained surface exists. Describe does
     * not pin memory or create a handle, including unsupported wrap layouts. */
    if (surface->memory == NULL || surface->width == 0 || surface->height == 0 ||
        surface->bytesPerRow == 0 || surface->allocSize == 0 ||
        surface->offset >= surface->allocSize ||
        surface->wrapDelta > surface->memory->getLength() ||
        surface->allocSize > surface->memory->getLength() - surface->wrapDelta) goto out;
    request->size = surface->allocSize;
    request->offset = surface->offset;
    request->rowBytes = surface->bytesPerRow;
    request->width = surface->width;
    request->height = surface->height;
    request->pixelFormat = surface->pixelFormat;
    request->planes = surface->planeCount;
    if (request->handle == SWEET_GPU_DESCRIBE) {
        request->handle = 0;
        ret = kIOReturnSuccess;
        goto out;
    }
    if ((surface->mapCache & kIOMapReadOnly) != 0 ||
        !gpuSurfaceRange(surface->memory->getLength(), surface->wrapDelta, surface->allocSize, &size)) {
        ret = kIOReturnUnsupported;
        goto out;
    }
    IOLockLock(gGPULock);
    for (uint32_t i = 0; i < GPU_BUFFERS; ++i) {
        if (fBuffers[i].memory != NULL && fBuffers[i].sync != NULL && fBuffers[i].sync->surface == surface) {
            request->handle = i + 1;
            request->size = fBuffers[i].size;
            IOLockUnlock(gGPULock);
            ret = kIOReturnSuccess;
            goto out;
        }
    }
    IOLockUnlock(gGPULock);
    memory = IOSubMemoryDescriptor::withSubRange(surface->memory, surface->wrapDelta, size, kIODirectionInOut);
    if (memory == NULL) { ret = kIOReturnNoMemory; goto out; }
    ret = memory->prepare();
    if (ret != kIOReturnSuccess) goto out;
    prepared = true;
    kernel = surface->kernelAddress();
    if (kernel == 0) { ret = kIOReturnNoMemory; goto out; }
    sync = IONewZero(struct gpu_surface_sync, 1);
    if (sync == NULL) { ret = kIOReturnNoMemory; goto out; }
    IOLockLock(gGPULock);
    /* Another import can have arrived while memory was being prepared. */
    for (uint32_t i = 0; i < GPU_BUFFERS; ++i) {
        if (fBuffers[i].memory != NULL && fBuffers[i].sync != NULL && fBuffers[i].sync->surface == surface) {
            request->handle = i + 1;
            request->size = fBuffers[i].size;
            IOLockUnlock(gGPULock);
            ret = kIOReturnSuccess;
            goto out;
        }
    }
    for (uint32_t n = 0; n < GPU_BUFFERS; ++n) {
        uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;
        if (fBuffers[i].memory == NULL) {
            b = &fBuffers[i]; request->handle = i + 1; fNextBuffer = i + 1; break;
        }
    }
    address = b != NULL ? sweet_gpu_address(size) : 0;
    if (address == 0) { IOLockUnlock(gGPULock); ret = kIOReturnNoMemory; goto out; }
    entered = true;
    for (uint64_t offset = 0; offset < size; offset += 4096) {
        IOByteCount segment = 0;
        addr64_t phys = memory->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
        if (phys == 0 || (phys & 4095u) != 0 || segment < 4096 || !sweet_gpu_map(address + offset, phys, false)) {
            sweet_gpu_unmap(address, size); sweet_gpu_address_free(address, size);
            entered = false; IOLockUnlock(gGPULock); ret = kIOReturnNoMemory; goto out;
        }
    }
    sweet_gpu_mapped();
    bzero(b, sizeof(*b));
    b->memory = memory; b->kernel = kernel; b->prepared = true;
    b->address = address; b->size = size; b->sync = sync;
    sync->surface = surface; sync->next = gSurfaceSync; gSurfaceSync = sync;
    request->size = size;
    IOLockUnlock(gGPULock);
    /* Ownership moves to the live BO/sync record, released only after fence. */
    memory = NULL; surface = NULL; sync = NULL; prepared = false;
    ret = kIOReturnSuccess;
out:
    (void)entered;
    if (prepared) (void)memory->complete();
    OSSafeReleaseNULL(memory);
    if (sync != NULL) IODelete(sync, struct gpu_surface_sync, 1);
    OSSafeReleaseNULL(surface);
    return ret;
}

/* fCallLock is held by call's wrapper. Regular BOs are WC; imported
 * surfaces are cached and may also be used by another GPU client. Join all
 * outstanding imports of that surface and invalidate before CPU access. */
IOReturn
SweetGPUUserClient::cpuPrep(const struct msm_gem_cpu_prep *request)
{
    IOLockLock(gGPULock);
    struct gpu_buffer *b = buffer(request->handle);
    if (b == NULL) { IOLockUnlock(gGPULock); return kIOReturnBadArgument; }
    uint64_t patience = (request->op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&request->timeout);
    if (b->sync != NULL) {
        SweetSurface *surface = b->sync->surface;
        surface->retain();
        IOLockUnlock(gGPULock);
        bool done = SweetGPUWaitForSurface(surface, patience);
        surface->release();
        return done ? kIOReturnSuccess : kIOReturnBusy;
    }
    bool done = b->fence == 0 || gpuWait(b->fence, patience);
    IOLockUnlock(gGPULock);
    return done ? kIOReturnSuccess : kIOReturnBusy;
}

IOReturn
SweetGPUUserClient::gemInfo(struct msm_gem_info *request)
{
	struct gpu_buffer *b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t)request->handle << 32;       /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;    /* the buffer's until it is given back (freedreno asks once) */
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {
		ret = kIOReturnUnsupported;     /* SET_IOVA among them: addresses are the kernel's to give */
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
SweetGPUUserClient::submit(struct msm_gem_submit *request)
{
	struct msm_gem_submit_bo *bos = NULL;
	struct msm_gem_submit_cmd *cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0, nr_bos = request->nr_bos, nr_cmds = request->nr_cmds;
	IOReturn ret = kIOReturnBadArgument;

	if (nr_cmds == 0 || nr_cmds > GPU_SUBMIT_CMDS || nr_bos == 0 || nr_bos > GPU_SUBMIT_BOS) {
		IOLog("SweetGPU: a submission of %u command buffers in %u buffers: not taken\n", nr_cmds, nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t)request->bos, bos, nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t)request->cmds, cmds, nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
    /* Cached SweetSurface pixels need clean/invalidate before GPU reads;
     * private WC BOs need only the existing ordering barrier. */
    for (uint32_t i = 0; i < nr_bos; ++i) {
        struct gpu_buffer *b = buffer(bos[i].handle);
        if (b == NULL) { IOLockUnlock(gGPULock); goto out; }
        if (b->sync != NULL) sweet_gpu_to_memory((void *)b->kernel, b->size);
    }
	sweet_gpu_to_memory(NULL, 0);           /* what the program wrote last is in memory (a barrier) */
	for (uint32_t i = 0; i < nr_cmds; i++) {
		struct gpu_buffer *b = cmds[i].submit_idx < nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                       /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t)cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("SweetGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = (count != 0 && !gGPUStopped) ? sweet_gpu_submit(addresses, words, count) : 0;
	if (request->fence == 0 && count != 0 && !gGPUStopped) {
		sweet_gpu_report("the ring stays full");
		gGPUStopped = true;
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	for (uint32_t i = 0; request->fence != 0 && i < nr_bos; i++) {
		struct gpu_buffer *b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
            if (b->sync != NULL) b->sync->fence = request->fence;
		}
	}
	if (request->fence != 0 && fSubmits++ < 4) {
		IOLog("SweetGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, nr_cmds);
	}
	return ret;
}

IOReturn
SweetGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	struct gpu_buffer *b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
SweetGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
    if (fCallLock == NULL) return kIOReturnNotReady;
    IOLockLock(fCallLock);
    IOReturn result = fClosing ? kIOReturnNotReady : call(selector, args);
    IOLockUnlock(fCallLock);
    return result;
}

IOReturn
SweetGPUUserClient::call(uint32_t selector, IOExternalMethodArguments *args)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
        struct sweet_gpu_surface surface;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer *b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!sweet_gpu_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		IOLockLock(gGPULock);
		ret = sweet_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		IOLockUnlock(gGPULock);
		break;
	case kMsmSetParam:                      /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
    case kSweetSurface:
        if (size != sizeof(r.surface) || args->structureOutput == NULL || args->structureOutputSize < sizeof(r.surface))
            return kIOReturnBadArgument;
        ret = surfaceBuffer(&r.surface);
        break;
    case kMsmGemCpuPrep:
        ret = cpuPrep(&r.cpuPrep);
        break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		IOLockLock(gGPULock);
		if (!gpuWait(r.waitFence.fence, millisecondsUntil(&r.waitFence.timeout))) {
			ret = kIOReturnTimeout;
		}
		IOLockUnlock(gGPULock);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:              /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			(void)copyout(&zero, (user_addr_t)r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
		}
		IOLockUnlock(gGPULock);
		break;
	default:
		IOLog("SweetGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t)size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
