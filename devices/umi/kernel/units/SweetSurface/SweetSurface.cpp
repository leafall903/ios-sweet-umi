#include "UmiSurfaceRange.h"
#include "UmiImageSpan.h"
/*
 * Built-in IOSurface for SWEET.
 *
 * Apple's IOSurface kext is closed. IOSurface.framework (iOS 16.7) finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc), +0x10 timestamps, then id, sizes and geometry (layout in
 * sweet_surface_fill_result). Every surface is wired kernel memory mapped
 * into each task that creates or looks it up; mappings are counted per
 * client and dropped on release or close.
 *
 * Values and bulk colour attachments follow ios-topaz 0133. Accelerator
 * binding stores identity only; no GPU execution is provided. Other calls
 * remain diagnostic stubs.
 */
#include "SweetSurface.h"

#if defined(SWEET)
extern "C" bool SweetGPUWaitForSurface(SweetSurface *, uint64_t);
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSString.h>
#include <sys/proc.h>

enum {
	kSweetSurfaceCreate             = 0,
	kSweetSurfaceLookup             = 4,
    kSweetSurfaceWrapClientImage=6,
    kSweetSurfaceWrapClientMemory=7,
	kSweetSurfaceSetYCbCrMatrix = 5,
	kSweetSurfaceSetValue = 9,
	kSweetSurfaceCopyValue = 10,
	kSweetSurfaceRemoveValue = 11,
	kSweetSurfaceBindAccel = 12,
	kSweetSurfaceSetPurgeable = 20,
	kSweetSurfaceSetBulkAttachments = 27,
	kSweetSurfaceLimits             = 13,
	kSweetSurfaceLookupFromPort     = 34,
	kSweetSurfaceCreatePort         = 35,
};

enum {
	kSweetSurfaceTrapIncrementUse   = 0,
	kSweetSurfaceTrapDecrementUse   = 1,
	kSweetSurfaceTrapLock           = 2,
	kSweetSurfaceTrapUnlock         = 3,
	kSweetSurfaceTrapRelease        = 4,
	kSweetSurfaceTrapReleaseFailed  = 5,
};

#define SWEET_SURFACE_RESULT_SIZE       0xa68
#define SWEET_SURFACE_MAX_ID            4096
#define SWEET_SURFACE_MAX_DIM           16384
#define SWEET_SURFACE_MAX_ALLOC         (256u << 20)
#define SWEET_SURFACE_MAX_PROPS         (64u << 10)
#define SWEET_SURFACE_ROW_ALIGN         64
#define SWEET_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define SWEET_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define SWEET_SURFACE_STATE_ACCEL       0x8     /* u16 accelerator id, u16 sub id */
#define SWEET_SURFACE_STATE_VSEED       0x20    /* u32 values seed */
#define SWEET_SURFACE_STATE_YCBCR       0x24    /* u32 */
#define SWEET_SURFACE_STATE_VSIZE       0x28    /* u32 room the values need when fetched, 0: none */
#define SWEET_SURFACE_STATE_BULK        0x30    /* 0x70 bytes of bulk attachments */
#define SWEET_SURFACE_BULK_SIZE         0x70
#define kIOSurfaceLockReadOnly          0x1

static IOLock       *sweet_surface_lock;
static SweetSurface *sweet_surface_table[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_users[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_next_id = 1;
static uint32_t      sweet_surface_creates_logged;
static uint64_t      sweet_surface_wired;       /* bytes of buffer surfaces */
#define SWEET_SURFACE_WIRED_MAX         (1536ull << 20)

OSDefineMetaClassAndStructors(SweetSurface, OSObject);

void
SweetSurface::free()
{
	if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
		OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&sweet_surface_wired);
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(values);
    if(wrapped && memory!=NULL)memory->complete();
    if(wrapCharge)OSAddAtomic64(-(SInt64)wrapCharge,(volatile SInt64*)&sweet_surface_wired);
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uintptr_t
SweetSurface::kernelAddress()
{
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		return (uintptr_t)buffer->getBytesNoCopy();
	}
	IOLockLock(sweet_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		kernelMap = memory->map((wrapped?kIOMapDefaultCache:kIOMapWriteCombineCache)|(mapCache&kIOMapReadOnly));
	}
	IOLockUnlock(sweet_surface_lock);
	return kernelMap != NULL ? (uintptr_t)kernelMap->getVirtualAddress()+wrapDelta : 0;
}

static uint32_t *
sweet_surface_seed(SweetSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_SEED);
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
sweet_surface_insert_locked(SweetSurface *s)
{
	for (uint32_t n = 0; n < SWEET_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = sweet_surface_next_id;
		sweet_surface_next_id = id + 1 < SWEET_SURFACE_MAX_ID ? id + 1 : 1;
		if (sweet_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			sweet_surface_table[id] = s;
			sweet_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
sweet_surface_get(SweetSurface *s)
{
	bool ok = true;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s) {
		sweet_surface_users[s->id]++;
	} else if (sweet_surface_table[s->id] == NULL) {
		s->retain();
		sweet_surface_table[s->id] = s;
		sweet_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
sweet_surface_put(SweetSurface *s)
{
	bool drop = false;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s && --sweet_surface_users[s->id] == 0) {
		sweet_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(sweet_surface_lock);
	if (drop) {
		s->release();
	}
}

SweetSurface *
sweet_surface_copy(uint32_t id)
{
	SweetSurface *s = NULL;
	if (id == 0 || id >= SWEET_SURFACE_MAX_ID || sweet_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(sweet_surface_lock);
	s = sweet_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(sweet_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
sweet_surface_init_lock(void)
{
	if (sweet_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static SweetSurface *
sweet_surface_alloc(void)
{
	sweet_surface_init_lock();
	SweetSurface *s = OSTypeAlloc(SweetSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	return s;
}

static bool
sweet_surface_publish(SweetSurface *s)
{
	bool ok;
	IOLockLock(sweet_surface_lock);
	ok = sweet_surface_insert_locked(s);
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

SweetSurface *
sweet_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory == NULL || !sweet_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
sweet_surface_port_in_task(SweetSurface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
sweet_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static uint32_t
sweet_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
sweet_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)sweet_div_up(width, ew) * bpe;
	return (uint32_t)((row + SWEET_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(SWEET_SURFACE_ROW_ALIGN - 1));
}

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
sweet_surface_layout(SweetSurface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = sweet_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = sweet_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)sweet_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = sweet_dict_u32(props, "IOSurfacePixelFormat", 0);
	s->cacheMode = sweet_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = sweet_dict_u32(props, "IOSurfaceOffset", 0);

    /* HackiOS MiniIOSurface's allocation-time negotiation: with the GPU on,
     * CoreAnimation can request b3a8 for the screen. Its two planes contain
     * wide-range RGB10 and separate A8, which our BGRA panel does not scan.
     * Allocate genuine BGRA instead, and return that actual geometry below.
     * Never reinterpret memory supplied by a client, including malformed
     * address properties. The binary wrap selectors bypass this function.
     * AddressRanges is not implemented by this driver; do not silently turn
     * that existing-memory request into a normalized owned allocation.
     */
    if (s->pixelFormat == 0x62336138u) { /* 'b3a8' */
        if (props->getObject("IOSurfaceAddressRanges") != NULL) return kIOReturnUnsupported;
        if (props->getObject("IOSurfaceAddress") == NULL) {
            OSNumber *width = OSDynamicCast(OSNumber, props->getObject("IOSurfaceWidth"));
            OSNumber *height = OSDynamicCast(OSNumber, props->getObject("IOSurfaceHeight"));
            OSNumber *format = OSDynamicCast(OSNumber, props->getObject("IOSurfacePixelFormat"));
            OSObject *rawOffset = props->getObject("IOSurfaceOffset");
            OSNumber *offset = OSDynamicCast(OSNumber, rawOffset);
            if (width == NULL || height == NULL || format == NULL ||
                format->unsigned64BitValue() != 0x62336138u ||
                width->unsigned64BitValue() == 0 || height->unsigned64BitValue() == 0 ||
                width->unsigned64BitValue() > SWEET_SURFACE_MAX_DIM ||
                height->unsigned64BitValue() > SWEET_SURFACE_MAX_DIM ||
                (rawOffset != NULL && offset == NULL) ||
                (offset != NULL && offset->unsigned64BitValue() > UINT32_MAX)) return kIOReturnBadArgument;
            s->width = (uint32_t)width->unsigned64BitValue();
            s->height = (uint32_t)height->unsigned64BitValue();
            s->offset = offset != NULL ? (uint32_t)offset->unsigned64BitValue() : 0;
            s->pixelFormat = 0x42475241u; /* 'BGRA', real bytes B G R A */
            s->bytesPerElement = 4;
            s->elementWidth = s->elementHeight = 1;
            s->bytesPerRow = sweet_row_bytes(s->width, 1, 4);
            s->planeCount = 0;
            bzero(s->planes, sizeof(s->planes));
            end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * s->height;
            if (end == 0 || end > SWEET_SURFACE_MAX_ALLOC) return kIOReturnNoMemory;
            uint64_t alloc = round_page(end);
            if (alloc < end || alloc > SWEET_SURFACE_MAX_ALLOC) return kIOReturnNoMemory;
            s->allocSize = (uint32_t)alloc;
            static uint32_t normalized;
            if (__atomic_fetch_add(&normalized, 1, __ATOMIC_RELAXED) < 16)
                IOLog("UMI SURFACE v23: owned b3a8 -> BGRA %ux%u row=%u size=%u planes=0\n",
                    s->width, s->height, s->bytesPerRow, s->allocSize);
            return kIOReturnSuccess;
        }
    }
	if (s->width > SWEET_SURFACE_MAX_DIM || s->height > SWEET_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = sweet_dict_u32(props, "IOSurfaceBytesPerRow",
	    sweet_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)sweet_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * sweet_div_up(s->height, s->elementHeight);

	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		if (planes->getCount() > SWEET_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct sweet_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = sweet_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = sweet_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)sweet_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > SWEET_SURFACE_MAX_DIM || p->height > SWEET_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = sweet_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    sweet_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = sweet_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = sweet_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * sweet_div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
	}

	uint64_t alloc = sweet_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	alloc = round_page(alloc);
	if (alloc == 0 || alloc > SWEET_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class SweetSurfaceRef : public OSObject {
	OSDeclareDefaultStructors(SweetSurfaceRef);

public:
	SweetSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;
    bool detached;

	bool init() APPLE_KEXT_OVERRIDE { detached=false;return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetSurfaceRef, OSObject);

void
SweetSurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		sweet_surface_put(surface);
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	printf("SWEET SURF: IOCoreSurfaceRoot::start entered\n");
	if (!IOService::start(provider)) {
		return false;
	}
	sweet_surface_init_lock();
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("SWEET SURF: IOCoreSurfaceRoot registered\n");
	return true;
}

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;

	SweetSurfaceRef *findRef(uint32_t id, unsigned int *index, bool detached=false);
	IOReturn addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(SweetSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);
    IOReturn wrapClient(uint32_t selector, IOExternalMethodArguments *args);
	IOReturn valueCall(uint32_t selector, IOExternalMethodArguments *args);
	IOReturn bulkAttachments(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
    IOReturn trapDetach(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t sweet_surf_logged;      /* selectors / traps already logged (< 64) */

static void
sweet_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (sweet_surf_logged & (1ULL << bit))) {
		return;
	}
	sweet_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET SURF: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
SweetSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index, bool detached)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id && ref->detached==detached) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 *
 * A lookup of a surface this client already has maps nothing and counts
 * nothing (*existing): IOSurface.framework then gets base address -1 in the
 * lock result and reuses its cached IOSurfaceClient. Counting it instead
 * made the framework build a second client for the same id; releasing that
 * one (traps 4 + 5) unmapped the memory under the first (backboardd
 * snapshot crash in CGDataProviderCreateWithData).
 */
IOReturn
IOSurfaceRootUserClient::addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
        ref->retain();
		IOLockUnlock(fLock);
		sweet_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(SweetSurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		sweet_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	SweetSurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
        raced->retain();
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
    bool stored=fRefs->setObject(ref);
    IOLockUnlock(fLock);
    if(!stored){ref->release();return kIOReturnNoMemory;}
    // fRefs and the reply each hold one reference until returnSurface completes.
    *refOut=ref;
	return kIOReturnSuccess;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x68). */
static void
sweet_surface_fill_result(SweetSurface *s, SweetSurfaceRef *ref, uint8_t *out)
{
	bzero(out, SWEET_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress()+s->wrapDelta);
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + SWEET_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x68 + i * 0x80;
		const struct sweet_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		put32(p, 0x08, pl->bytesPerRow);
		put32(p, 0x0c, pl->offset);
		put32(p, 0x14, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

static IOReturn
sweet_struct_out(IOExternalMethodArguments *args,const void *buf,uint32_t len)
{
    if(args->structureOutputDescriptor!=NULL){
        IOMemoryDescriptor *out=args->structureOutputDescriptor;
        if(len>out->getLength())return kIOReturnNoSpace;
        IOReturn ret=out->prepare();if(ret!=kIOReturnSuccess)return ret;
        bool full=out->writeBytes(0,buf,len)==len;
        IOReturn completed=out->complete();
        if(!full)return kIOReturnVMError;
        if(completed!=kIOReturnSuccess)return completed;
        args->structureOutputDescriptorSize=len;return kIOReturnSuccess;
    }
    if(args->structureOutput==NULL || len>args->structureOutputSize)return kIOReturnNoSpace;
    memcpy(args->structureOutput,buf,len);args->structureOutputSize=len;return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(SweetSurface *s, IOExternalMethodArguments *args)
{
	SweetSurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	uint8_t *out = (uint8_t *)IOMallocZeroData(SWEET_SURFACE_RESULT_SIZE);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		sweet_surface_fill_result(s, ref, out);
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* "already yours": use the cached client */
		}
		ret = sweet_struct_out(args, out, SWEET_SURFACE_RESULT_SIZE);
		IOFreeData(out, SWEET_SURFACE_RESULT_SIZE);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
        // Undo this exact view, not an older detached view with the same id.
        IOLockLock(fLock);
        for(unsigned i=0;i<fRefs->getCount();++i)if(fRefs->getObject(i)==ref){
            if(ref->count>0 && --ref->count==0)fRefs->removeObject(i);
            break;
        }
        IOLockUnlock(fLock);
	}
    ref->release();
	return ret;
}

static IOReturn
sweet_surface_use_task_memory(SweetSurface*s,task_t task,uint64_t address,uint64_t size)
{
    uint64_t first=0,length=0;uint32_t delta=0;
    if(!umi_surface_task_range(address,size,page_size,&first,&length,&delta))return kIOReturnBadArgument;
    uint64_t old=__atomic_load_n(&sweet_surface_wired,__ATOMIC_RELAXED);
    for(;;){
        if(old>SWEET_SURFACE_WIRED_MAX || length>SWEET_SURFACE_WIRED_MAX-old)return kIOReturnNoMemory;
        if(__atomic_compare_exchange_n(&sweet_surface_wired,&old,old+length,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))break;
    }
    IOMemoryDescriptor*md=NULL;bool readonly=false;IOReturn prepared=kIOReturnNoMemory;
    for(unsigned attempt=0;attempt<2;++attempt){
        md=IOMemoryDescriptor::withAddressRange(first,length,attempt?kIODirectionOut:kIODirectionInOut,task);
        prepared=md?md->prepare():kIOReturnNoMemory;
        if(prepared==kIOReturnSuccess){readonly=attempt!=0;break;}
        OSSafeReleaseNULL(md);
    }
    if(prepared!=kIOReturnSuccess){
        OSAddAtomic64(-(SInt64)length,(volatile SInt64*)&sweet_surface_wired);
        return prepared;
    }
    s->memory=md;s->wrapped=true;s->wrapCharge=length;s->wrapDelta=delta;
    s->mapCache=kIOMapDefaultCache|(readonly?kIOMapReadOnly:0);s->allocSize=(uint32_t)size;
    return kIOReturnSuccess;
}

static IOReturn
sweet_surface_attach_client_props(SweetSurface*s,task_t task,OSDictionary*props)
{
    OSObject*raw=props->getObject("IOSurfaceAddress");if(raw==NULL)return kIOReturnSuccess;
    OSNumber*address=OSDynamicCast(OSNumber,raw);
    OSObject*rawOwned=props->getObject("IOSurfaceAllocSize");
    OSNumber*owned=OSDynamicCast(OSNumber,rawOwned);
    if(address==NULL || (rawOwned!=NULL&&owned==NULL) || !s->elementHeight)return kIOReturnBadArgument;
    uint64_t rows=((uint64_t)s->height+s->elementHeight-1)/s->elementHeight;
    uint64_t used=(uint64_t)s->offset+(uint64_t)s->bytesPerRow*rows;
    if(s->planeCount==0 && !umi_image_minimum_span(s->width,s->height,s->bytesPerElement,s->elementWidth,s->elementHeight,s->bytesPerRow,s->offset,&used))return kIOReturnBadArgument;
    for(uint32_t i=0;i<s->planeCount;++i)if((uint64_t)s->planes[i].offset+s->planes[i].size>used)used=(uint64_t)s->planes[i].offset+s->planes[i].size;
    uint64_t size=owned&&owned->unsigned64BitValue()?owned->unsigned64BitValue():used;
    IOReturn ret=size<used?kIOReturnBadArgument:sweet_surface_use_task_memory(s,task,address->unsigned64BitValue(),size);
    static uint32_t spanOkLogs,spanBadLogs;
    if(ret==kIOReturnSuccess?__atomic_fetch_add(&spanOkLogs,1,__ATOMIC_RELAXED)<16:__atomic_fetch_add(&spanBadLogs,1,__ATOMIC_RELAXED)<32)IOLog("UMI SURF v6 span=%llu declared=%llu rows=%llu row=%u width=%u height=%u result=%x\n",used,size,rows,s->bytesPerRow,s->width,s->height,ret);
    if(ret==kIOReturnSuccess && !sweet_surface_publish(s))ret=kIOReturnNoResources;
    static uint32_t logged;
    if(__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<16)IOLog("UMI SURFACE v15: client image id=%u size=%llu result=0x%x\n",s->id,size,ret);
    return ret;
}

static uint8_t *sweet_struct_in(IOExternalMethodArguments *args,uint32_t *len);

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
    uint32_t len=0;uint8_t *copy=sweet_struct_in(args,&len);
    if(copy==NULL)return kIOReturnBadArgument;
    OSObject *obj=OSUnserializeXML((const char*)copy,len);
    IOFreeData(copy,len);IOReturn ret;

	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("SWEET SURF: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = sweet_surface_layout(s, props);
    if(ret==kIOReturnSuccess)ret=sweet_surface_attach_client_props(s,fTask,props);
	if (ret == kIOReturnSuccess && s->memory==NULL) {
		IOBufferMemoryDescriptor *mem = NULL;
		if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired) +
		    s->allocSize <= SWEET_SURFACE_WIRED_MAX) {
			mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
		}
		if (mem == NULL) {
			OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
			ret = kIOReturnNoMemory;
		} else {
			bzero(mem->getBytesNoCopy(), s->allocSize);
			s->memory = mem;
			s->mapCache = kIOMapDefaultCache;
			if (!sweet_surface_publish(s)) {
				ret = kIOReturnNoResources;
			}
		}
	}
    static uint32_t createFailures;
    uint32_t failed=ret!=kIOReturnSuccess?__atomic_add_fetch(&createFailures,1,__ATOMIC_RELAXED):0;
	if ((failed && (failed<=24 || (failed&(failed-1))==0)) || sweet_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		sweet_surface_creates_logged++;
		IOLog("SWEET SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

/* The structure input of a call, wherever it came (inline or a descriptor), copied. Free with IOFreeData. */
static uint8_t *
sweet_struct_in(IOExternalMethodArguments *args, uint32_t *len)
{
	uint8_t *copy;

	uint64_t requested = args->structureInputDescriptor != NULL ?
        args->structureInputDescriptor->getLength() : args->structureInputSize;
    if (requested == 0 || requested > SWEET_SURFACE_MAX_PROPS) return NULL;
    *len = (uint32_t)requested;
    if (args->structureInputDescriptor == NULL && args->structureInput == NULL) return NULL;
    copy = (uint8_t *)IOMallocData(*len);
    if (copy == NULL) return NULL;
    if (args->structureInputDescriptor != NULL) {
        IOMemoryDescriptor *in=args->structureInputDescriptor;
        IOReturn prepared=in->prepare();
        if(prepared!=kIOReturnSuccess){IOFreeData(copy,*len);return NULL;}
        bool full=in->readBytes(0,copy,*len)==*len;
        IOReturn completed=in->complete();
        if(!full || completed!=kIOReturnSuccess){IOFreeData(copy,*len);return NULL;}
    } else memcpy(copy, args->structureInput, *len);
	return copy;
}

static uint32_t *
sweet_state32(SweetSurface *s, uint32_t offset)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + offset);
}

/* After a change of the values: a new (odd) seed and the room a fetch of all of them needs. Lock held. */
static void
sweet_values_changed(SweetSurface *s)
{
	uint32_t size = 0;

	if (s->values != NULL && s->values->getCount() != 0) {
		OSSerialize *ser = OSSerialize::binaryWithCapacity(4096);
		if (ser != NULL && s->values->serialize(ser)) {
			size = ser->getLength() + 8;
		}
		OSSafeReleaseNULL(ser);
	}
	*sweet_state32(s, SWEET_SURFACE_STATE_VSIZE) = size;
    __atomic_store_n(sweet_state32(s, SWEET_SURFACE_STATE_VSEED),
        (*sweet_state32(s, SWEET_SURFACE_STATE_VSEED) + 2) | 1, __ATOMIC_RELEASE);
}

static IOReturn
sweet_reply_seed(IOExternalMethodArguments *args, uint32_t seed)
{
    if (args->structureOutput == NULL && args->structureOutputDescriptor == NULL && args->structureOutputSize == 0) return kIOReturnSuccess;
    return sweet_struct_out(args, &seed, sizeof(seed));
}

/*
 * Values. Requests start with the surface id (8 bytes). SetValue: then a
 * serialized array, [value, key] or [dictionary of them all]. RemoveValue and
 * CopyValue: then a key, NUL-terminated (empty: all). Changes answer the new
 * seed; CopyValue answers 4 unused bytes, the seed, the serialized value.
 */
IOReturn
IOSurfaceRootUserClient::valueCall(uint32_t selector, IOExternalMethodArguments *args)
{
	uint32_t len = 0, id, seed = 0;
	uint8_t *in = sweet_struct_in(args, &len);
	IOReturn ret = kIOReturnBadArgument;
	SweetSurface *s;
	OSSerialize *ser = NULL;
	uint32_t length = 0;

	if (in == NULL) {
		return kIOReturnBadArgument;
	}
	if (len < 9 || (selector != kSweetSurfaceSetValue && in[len - 1] != 0)) {
		IOFreeData(in, len);
		return kIOReturnBadArgument;
	}
	memcpy(&id, in, sizeof(id));
	s = sweet_surface_copy(id);
	if (s == NULL) {
		IOFreeData(in, len);
		return kIOReturnNotFound;
	}
	OSObject *obj = selector == kSweetSurfaceSetValue ? OSUnserializeXML((const char *)in + 8, len - 8) : NULL;
	IOLockLock(sweet_surface_lock);
	if (s->values == NULL) {
		s->values = OSDictionary::withCapacity(4);
	}
	if (s->values == NULL) {
		ret = kIOReturnNoMemory;
	} else if (selector == kSweetSurfaceSetValue) {
		OSArray *array = OSDynamicCast(OSArray, obj);
		OSString *key = array != NULL ? OSDynamicCast(OSString, array->getObject(1)) : NULL;
		OSDictionary *all = array != NULL ? OSDynamicCast(OSDictionary, array->getObject(0)) : NULL;
        OSDictionary *next = OSDictionary::withDictionary(s->values);
        bool valid = false;
        if (next != NULL && array != NULL && array->getCount() == 2 && key != NULL && array->getObject(0) != NULL) {
            valid = next->setObject(key, array->getObject(0));
        } else if (next != NULL && array != NULL && array->getCount() == 1 && all != NULL) {
            valid = next->merge(all);
        }
        OSSerialize *check = valid ? OSSerialize::binaryWithCapacity(4096) : NULL;
        if (valid && check != NULL && next->serialize(check) &&
            check->getLength() <= SWEET_SURFACE_MAX_PROPS - 8) {
            s->values->release(); s->values = next; next = NULL;
            sweet_values_changed(s); ret = kIOReturnSuccess;
        } else if (valid) ret = kIOReturnNoSpace;
        else if (next == NULL) ret = kIOReturnNoMemory;
        OSSafeReleaseNULL(check);
        OSSafeReleaseNULL(next);
	} else if (selector == kSweetSurfaceRemoveValue) {
		const char *key = (const char *)in + 8;
		if (key[0] == 0) {
			s->values->flushCollection();
		} else {
			s->values->removeObject(key);
		}
		sweet_values_changed(s);
		ret = kIOReturnSuccess;
	} else {
		const char *key = (const char *)in + 8;
		OSObject *value = key[0] != 0 ? s->values->getObject(key) : s->values;
		ret = kIOReturnSuccess;
		if (value != NULL && (key[0] != 0 || s->values->getCount() != 0)) {
			ser = OSSerialize::binaryWithCapacity(4096);
			if (ser == NULL || !value->serialize(ser)) {
				ret = kIOReturnNoMemory;
			} else {
				length = ser->getLength();
			}
		}
	}
	seed = *sweet_state32(s, SWEET_SURFACE_STATE_VSEED);
	IOLockUnlock(sweet_surface_lock);
	OSSafeReleaseNULL(obj);
	IOFreeData(in, len);

	if (ret == kIOReturnSuccess && selector == kSweetSurfaceCopyValue) {
		uint32_t header[2] = { 0, seed }, total = (uint32_t)sizeof(header) + length;
        uint8_t *reply = (uint8_t *)IOMallocData(total);
        if (reply == NULL) ret = kIOReturnNoMemory;
        else {
            memcpy(reply, header, sizeof(header));
            if (length != 0) memcpy(reply + sizeof(header), ser->text(), length);
            ret = sweet_struct_out(args, reply, total);
            IOFreeData(reply, total);
        }
	} else if (ret == kIOReturnSuccess) {
		ret = sweet_reply_seed(args, seed);
	}
	OSSafeReleaseNULL(ser);
    static uint32_t logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 24)
        IOLog("UMI SURFACE v12: values sel=%u result=0x%x seed=%u\n", selector, ret, seed);
	s->release();
	return ret;
}

/*
 * Bulk attachments (colour space, transfer function, ...): 0x70 bytes, then at
 * +0x70 their size, +0x78 a mask of the fields that changed, +0x80 the surface
 * id. The fields, by their bit, are these byte ranges of the 0x70.
 */
IOReturn
IOSurfaceRootUserClient::bulkAttachments(IOExternalMethodArguments *args)
{
	static const uint8_t field[15][2] = {
		{ 0x00, 0x20 }, { 0x20, 0x30 }, { 0x30, 0x38 }, { 0x38, 0x39 }, { 0x39, 0x3a }, { 0x3a, 0x3b }, { 0x3b, 0x3c },
		{ 0x3c, 0x3d }, { 0x3d, 0x3e }, { 0x3e, 0x3f }, { 0x3f, 0x40 }, { 0x40, 0x58 }, { 0x58, 0x5c }, { 0x5c, 0x64 },
		{ 0x64, 0x65 },
	};
	uint32_t len = 0, id;
	uint64_t size, mask;
	uint8_t *in = sweet_struct_in(args, &len);
	SweetSurface *s;

	if (in == NULL || len < 0x84) {
		if (in != NULL) {
			IOFreeData(in, len);
		}
		return kIOReturnBadArgument;
	}
	memcpy(&size, in + 0x70, 8);
	memcpy(&mask, in + 0x78, 8);
	memcpy(&id, in + 0x80, 4);
	if (size > SWEET_SURFACE_BULK_SIZE || (s = sweet_surface_copy(id)) == NULL) {
		IOFreeData(in, len);
		return size > SWEET_SURFACE_BULK_SIZE ? kIOReturnBadArgument : kIOReturnNotFound;
	}
    for (unsigned i = 0; i < 15; ++i) {
        if ((mask & (1ull << i)) && field[i][1] > size) {
            IOFreeData(in, len); s->release(); return kIOReturnBadArgument;
        }
    }
	uint8_t *bulk = (uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_BULK;
	IOLockLock(sweet_surface_lock);
	for (unsigned i = 0; i < 15; i++) {
		if ((mask & (1ull << i)) && field[i][1] <= size) {
			memcpy(bulk + field[i][0], in + field[i][0], field[i][1] - field[i][0]);
		}
	}
	IOLockUnlock(sweet_surface_lock);
	IOFreeData(in, len);
    static uint32_t logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 24)
        IOLog("UMI SURFACE v12: bulk size=%llu mask=0x%llx stored\n", size, mask);
	s->release();
	return kIOReturnSuccess;
}


IOReturn
IOSurfaceRootUserClient::wrapClient(uint32_t selector,IOExternalMethodArguments*args)
{
    uint64_t address=0,size=0;uint32_t width=0,height=1,format=0,bpe=1,bpr=0;
    if(selector==kSweetSurfaceWrapClientImage){
        uint32_t n=0;uint8_t*in=sweet_struct_in(args,&n);
        if(in==NULL)return kIOReturnBadArgument;
        if(n<0x20){IOFreeData(in,n);return kIOReturnBadArgument;}
        memcpy(&address,in,8);memcpy(&width,in+8,4);memcpy(&height,in+12,4);
        memcpy(&format,in+16,4);memcpy(&bpe,in+20,4);memcpy(&bpr,in+24,4);IOFreeData(in,n);
        if(!width||!height||width>SWEET_SURFACE_MAX_DIM||height>SWEET_SURFACE_MAX_DIM||!bpe||bpe>16||bpr<(uint64_t)width*bpe)return kIOReturnBadArgument;
        size=(uint64_t)bpr*height;
    }else{
        if(args->scalarInputCount<2)return kIOReturnBadArgument;
        address=args->scalarInput[0];size=args->scalarInput[1];
        if(size>SWEET_SURFACE_MAX_ALLOC)return kIOReturnBadArgument;
        width=bpr=(uint32_t)size;
    }
    SweetSurface*s=sweet_surface_alloc();if(!s)return kIOReturnNoMemory;
    IOReturn ret=sweet_surface_use_task_memory(s,fTask,address,size);
    if(ret==kIOReturnSuccess){
        s->width=width;s->height=height;s->pixelFormat=format;s->bytesPerElement=(uint16_t)bpe;
        s->elementWidth=s->elementHeight=1;s->bytesPerRow=bpr;s->planeCount=0;
        if(!sweet_surface_publish(s))ret=kIOReturnNoResources;
    }
    static uint32_t logged;
    if(__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<16)IOLog("UMI SURFACE v15: wrap selector=%u id=%u size=%llu result=0x%x\n",selector,s->id,size,ret);
    if(ret==kIOReturnSuccess)ret=returnSurface(s,args);
    s->release();return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kSweetSurfaceLimits: {
		uint8_t limits[0x18] = { 0 };
		put32(limits, 0x0, SWEET_SURFACE_ROW_ALIGN - 1);
		put32(limits, 0x4, SWEET_SURFACE_ROW_ALIGN - 1);
		limits[0x16] = 1;
		sweet_surf_log_once("sel", selector);
		return sweet_struct_out(args, limits, sizeof(limits));
	}
	case kSweetSurfaceCreate:
		return create(args);
    case kSweetSurfaceWrapClientImage:
    case kSweetSurfaceWrapClientMemory:
        return wrapClient(selector,args);
	case kSweetSurfaceSetValue:
	case kSweetSurfaceCopyValue:
	case kSweetSurfaceRemoveValue:
		sweet_surf_log_once("sel", selector);
		return valueCall(selector, args);
	case kSweetSurfaceSetBulkAttachments:
		sweet_surf_log_once("sel", selector);
		return bulkAttachments(args);
	case kSweetSurfaceSetPurgeable:
	case kSweetSurfaceBindAccel:
	case kSweetSurfaceSetYCbCrMatrix: {
		/* in: surface, state (out: the old one) / surface, accelerator, sub id / surface, matrix */
		SweetSurface *s = args->scalarInputCount >= 2 ? sweet_surface_copy((uint32_t)arg0) : NULL;
		sweet_surf_log_once("sel", selector);
		if (s == NULL) {
			return args->scalarInputCount >= 2 ? kIOReturnNotFound : kIOReturnBadArgument;
		}
		IOLockLock(sweet_surface_lock);
		if (selector == kSweetSurfaceSetPurgeable) {
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = s->purgeable;
				args->scalarOutputCount = 1;
			}
			if (args->scalarInput[1] != 3) {        /* 3: only ask */
				s->purgeable = (uint32_t)args->scalarInput[1];
			}
		} else if (selector == kSweetSurfaceBindAccel && args->scalarInputCount >= 3) {
			uint16_t accel[2] = { (uint16_t)args->scalarInput[1], (uint16_t)args->scalarInput[2] };
			memcpy((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_ACCEL, accel, sizeof(accel));
		} else if (selector == kSweetSurfaceSetYCbCrMatrix) {
			*sweet_state32(s, SWEET_SURFACE_STATE_YCBCR) = (uint32_t)args->scalarInput[1];
		}
		IOLockUnlock(sweet_surface_lock);
		s->release();
		return kIOReturnSuccess;
	}

	case kSweetSurfaceLookup: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = sweet_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		SweetSurface *s = OSDynamicCast(SweetSurface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && sweet_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceCreatePort: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = sweet_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	default:
        if ((args->structureOutputDescriptor!=NULL && args->structureOutputDescriptor->getLength()>=SWEET_SURFACE_RESULT_SIZE) ||
            (args->structureOutput!=NULL && args->structureOutputSize>=SWEET_SURFACE_RESULT_SIZE)) {
            sweet_surf_log_once("sel (refused lock result)",selector);
            return kIOReturnUnsupported;
        }
		/* Values, notifications, ownership, timestamps...: accept and ignore. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
sweet_surface_seed_out(SweetSurface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)sweet_surface_seed(s)) + 1 :
	    *sweet_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static SweetSurface *
sweet_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	SweetSurface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, __unused void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
    /* sweet_surface_held() returned a retain after dropping fLock. Do not
     * hold a surface/client lock while taking the GPU lock: imported GPU
     * cleanup may itself release a SweetSurface. This includes offscreen
     * wallpaper/widget consumers which never reach framebuffer present. */
    IOReturn ret = SweetGPUWaitForSurface(s, 1000) ?
        sweet_surface_seed_out(s, seedOut, false) : kIOReturnTimeout;
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = sweet_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapDetach(void *id,void *,void *,void *,void *,void *)
{
    IOLockLock(fLock);
    SweetSurfaceRef *ref=findRef((uint32_t)(uintptr_t)id,NULL);
    if(ref!=NULL)ref->detached=true;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index, true);
    if(ref==NULL)ref=findRef((uint32_t)(uintptr_t)id,&index);
	if (ref != NULL && --ref->count == 0) {
		fRefs->removeObject(index);     /* unmaps and drops the user count */
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap sweet_surface_traps[] = {
	[kSweetSurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kSweetSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kSweetSurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kSweetSurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	[kSweetSurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapDetach },
	[kSweetSurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(sweet_surface_traps) / sizeof(sweet_surface_traps[0])) {
		sweet_surf_log_once("trap", index);
		return NULL;
	}
	sweet_surf_log_once("trap", index);
	*targetP = this;
	return &sweet_surface_traps[index];
}
#endif /* SWEET */
