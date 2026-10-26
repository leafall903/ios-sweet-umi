#include "UmiFramePacing.h"
/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off blanks the panel using write-only rows; previous
 *      multi-vsync read/modify/write fade was too slow on umi;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 */
#include "SweetSurface.h"
extern "C" bool SweetGPUWaitForSurface(SweetSurface *, uint64_t);

extern "C" void sweet_gpu_init(void);
extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */
/*
 * SWEET diagnostic: answer, from the kernel, the question the userspace stack
 * asks -- does IOServiceMatching("IOMobileFramebuffer") find this driver? It
 * runs at the first exec, when IOKit is up, and prints the counts so a
 * persistent log can tell a matching problem from a timing problem.
 */
extern "C" void sweet_diag_display(void);

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
	kSweetFBGetBlock = 79,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

extern "C" void
sweet_diag_display(void)
{
	OSIterator *it = IOService::getMatchingServices(
	    IOService::serviceMatching("IOMobileFramebuffer"));
	IOService *svc;
	int n = 0;

	while (it != NULL && (svc = OSDynamicCast(IOService, it->getNextObject())) != NULL) {
		n++;
		printf("SWEET DIAG: match[%d] %s provider=%s\n", n - 1,
		    svc->getName() ? svc->getName() : "?",
		    (svc->getProvider() && svc->getProvider()->getName()) ?
		    svc->getProvider()->getName() : "?");
	}
	if (it != NULL) {
		it->release();
	}
	printf("SWEET DIAG: IOServiceMatching(\"IOMobileFramebuffer\") found %d service(s)\n", n);

	IORegistryEntry *armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	int disp = 0;
	if (armio != NULL) {
		OSIterator *kids = armio->getChildIterator(gIODTPlane);
		IORegistryEntry *kid;
		while (kids != NULL && (kid = OSDynamicCast(IORegistryEntry, kids->getNextObject())) != NULL) {
			OSString *s = OSDynamicCast(OSString, kid->getProperty("device_type"));
			if (s != NULL && s->isEqualTo("display-subsystem")) {
				disp++;
				printf("SWEET DIAG: display-subsystem child: %s\n",
				    kid->getName() ? kid->getName() : "?");
			}
		}
		if (kids != NULL) {
			kids->release();
		}
		armio->release();
	}
	printf("SWEET DIAG: /arm-io display-subsystem children = %d\n", disp);
}

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);
    void publishModes(void);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uint8_t      *fBlackRow;        /* cached zero row: write-only panel blank */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	IOLock       *fPresentLock; /* order: present then state */
	uint64_t      fMinAbs, fNextPresent;
    uint64_t fVsyncDeadline, fVsyncPeriodAbs, fPresentCount, fSubmitCount;
    uint64_t fStatsAbs, fStatsPresents, fStatsSubmits;
    unsigned fStatsReports;
	bool          fPowerOff;      /* RequestPowerChange(0) */
	uint32_t      fFadeStep;      /* fade-out steps done while off */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
	void presentPending(void);
    void scheduleVsync(IOTimerEventSource *sender);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	printf("SWEET FB: AppleCLCD::start entered\n");

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	sweet_gpu_init();
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	printf("SWEET FB: base=0x%llx %ux%u row=%u\n", (uint64_t)fPhys, fWidth, fHeight, fRowBytes);
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fLock = IOLockAlloc();
	fPresentLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fBlackRow = (uint8_t *)IOMallocZeroData(fRowBytes);
	fWorkLoop = IOWorkLoop::workLoop();
	if (fBlackRow == NULL || fLock == NULL || fPresentLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 10;
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	if (hz > 60) hz = 60;
	nanoseconds_to_absolutetime(hz ? NSEC_PER_SEC / hz : 0, &fMinAbs);
    nanoseconds_to_absolutetime((uint64_t)SWEET_FB_VSYNC_US * 1000, &fVsyncPeriodAbs);
    IOLog("UMI DISPLAY requested_hz=%u vsync_us=%u absolute_pacing=1 present_phase=1\n", hz, SWEET_FB_VSYNC_US);
    scheduleVsync(fVsync);

    publishModes();
	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
    static uint64_t previous=0;static unsigned logged=0;
    uint64_t gap=0,ms=0;if(previous)absolutetime_to_nanoseconds(now-previous,&gap);
    previous=now;absolutetime_to_nanoseconds(now,&ms);
    if(logged<40 && gap>200000000ULL){++logged;IOLog("UMI VSYNC t_ms=%llu gap_ms=%llu\n",ms/1000000,gap/1000000);}

	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6];
    umi_fb_vsync_values(now, now, count, values);
	IOLockLock(fb->fPresentLock);
	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	if (fb->fPowerOff) {
        // The WC/NC scanout mapping is fast to write but extremely slow to
        // read: v9 measured ~720ms timer gaps during each RMW fade step.
        // Blank once using a cached zero row; never read scanout for animation.
        bool blank=fb->fFadeStep==0;
        fb->fFadeStep=1;
        IOLockUnlock(fb->fLock);
        if(blank){
            uint64_t before=mach_absolute_time(),ns=0,t=0;
            for(uint32_t y=0;y<fb->fHeight;++y)
                memcpy((void*)(fb->fPixels+(uint64_t)y*fb->fRowBytes),fb->fBlackRow,fb->fRowBytes);
            __asm__ volatile("dsb sy":::"memory");
            absolutetime_to_nanoseconds(mach_absolute_time()-before,&ns);
            absolutetime_to_nanoseconds(before,&t);
            IOLog("UMI BLANK t_ms=%llu write_us=%llu (no scanout reads)\n",t/1000000,ns/1000);
        }

		IOLockUnlock(fb->fPresentLock);
		fb->scheduleVsync(sender);
		return;
	}
	IOLockUnlock(fb->fLock);
	if (fb->fDirty && (fb->fMinAbs==0 || umi_present_due(mach_absolute_time(), fb->fNextPresent))) fb->presentPending();
	IOLockUnlock(fb->fPresentLock);

	fb->scheduleVsync(sender);
}

void
AppleCLCD::scheduleVsync(IOTimerEventSource *sender)
{
    uint64_t now = mach_absolute_time(), ns=0;
    if (!fStatsAbs) fStatsAbs=now;
    absolutetime_to_nanoseconds(now-fStatsAbs, &ns);
    if (ns>=1000000000ULL) {
        uint64_t shown=__atomic_load_n(&fPresentCount, __ATOMIC_RELAXED);
        uint64_t submitted=__atomic_load_n(&fSubmitCount, __ATOMIC_RELAXED);
        if(shown!=fStatsPresents || submitted!=fStatsSubmits)
            IOLog("UMI DISPLAY window_ms=%llu frames=%llu swaps=%llu\n", ns/1000000,
                shown-fStatsPresents, submitted-fStatsSubmits);
        fStatsAbs=now;fStatsPresents=shown;fStatsSubmits=submitted;++fStatsReports;
    }
    fVsyncDeadline=umi_vsync_next(fVsyncDeadline, fVsyncPeriodAbs, mach_absolute_time());
    sender->wakeAtTime(fVsyncDeadline);
}

/*
 * The display's modes, the way CoreAnimation reads them (properties TimingElements and
 * ColorElements; the current one is block 0x40 of selector 79): one timing, the presented size
 * at 60 Hz, one colour mode, 8-bit RGB. Without them CoreAnimation ran its frame clock at a rate
 * of its own guessing: 20 frames a second, 50 ms apart. (After HackiOS MiniIOMobileFramebuffer
 * publishModes(), APSL 2.0.)
 */
#define SWEET_FB_TIMING_ID      1
#define SWEET_FB_COLOR_ID       1

static void
sweet_set_number(OSDictionary *d, const char *key, uint64_t value)
{
	OSNumber *n = OSNumber::withNumber(value, 32);
	if (n != NULL) {
		d->setObject(key, n);
		n->release();
	}
}

void
AppleCLCD::publishModes(void)
{
	OSDictionary *timing = OSDictionary::withCapacity(8);
	OSDictionary *horizontal = OSDictionary::withCapacity(4);
	OSDictionary *vertical = OSDictionary::withCapacity(4);
	OSDictionary *color = OSDictionary::withCapacity(8);
	OSArray *timings = OSArray::withCapacity(1);
	OSArray *colors = OSArray::withCapacity(1);
	const uint32_t rate = 60u << 16;        /* 16.16 */

	if (timing != NULL && horizontal != NULL && vertical != NULL && color != NULL && timings != NULL && colors != NULL) {
		sweet_set_number(color, "ID", SWEET_FB_COLOR_ID);
		sweet_set_number(color, "PixelEncoding", 0);
		sweet_set_number(color, "Depth", 8);
		sweet_set_number(color, "Colorimetry", 1);
		sweet_set_number(color, "EOTF", 1);
		sweet_set_number(color, "DynamicRange", 0);
		sweet_set_number(color, "Score", 1);
		color->setObject("IsVirtual", kOSBooleanFalse);
		colors->setObject(color);
		sweet_set_number(horizontal, "Active", fLogicalW);
		sweet_set_number(horizontal, "Total", fLogicalW);
		sweet_set_number(horizontal, "SyncRate", rate);
		sweet_set_number(horizontal, "PreciseSyncRate", rate);
		sweet_set_number(vertical, "Active", fLogicalH);
		sweet_set_number(vertical, "Total", fLogicalH);
		sweet_set_number(vertical, "SyncRate", rate);
		sweet_set_number(vertical, "PreciseSyncRate", rate);
		sweet_set_number(timing, "ID", SWEET_FB_TIMING_ID);
		sweet_set_number(timing, "Score", 1);
		timing->setObject("IsVirtual", kOSBooleanFalse);
		timing->setObject("IsOverscanned", kOSBooleanFalse);
		timing->setObject("HorizontalAttributes", horizontal);
		timing->setObject("VerticalAttributes", vertical);
		timing->setObject("ColorModes", colors);
		timings->setObject(timing);
		setProperty("TimingElements", timings);
		setProperty("ColorElements", colors);
		IOLog("SWEET FB: modes published: %ux%u at 60 Hz\n", fLogicalW, fLogicalH);
	}
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}


void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;
static uint64_t sweet_fb_swap_count;

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * limits copies using sweetfb_hz, with synchronous presentation on swap (
 * default 10 Hz).
 */
// Adapted from ios-sailfish patch0092: copy on submission before buffer reuse.
// Fixed present->state lock order also covers display-off fading.
void
AppleCLCD::swap(const uint8_t *rec)
{
    __atomic_add_fetch(&fSubmitCount, 1, __ATOMIC_RELAXED);
    IOLockLock(fPresentLock);
    IOLockLock(fLock);
    memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
    fDirty = true;
    bool off = fPowerOff;
    IOLockUnlock(fLock);
    if (!off && (fMinAbs==0 || umi_present_due(mach_absolute_time(), fNextPresent))) presentPending();
    IOLockUnlock(fPresentLock);
    if ((++sweet_fb_swap_count % 1000) == 0) IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
}

// fPresentLock held by caller. No per-frame heap allocation.
void
AppleCLCD::presentPending(void)
{
    uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
    IOLockLock(fLock);
    memcpy(rec, fPending, sizeof(rec));
    fDirty = false;
    IOLockUnlock(fLock);
    uint64_t before = mach_absolute_time();
    present(rec);
    uint64_t finished = mach_absolute_time();
    fNextPresent = fMinAbs ? umi_vsync_next(fNextPresent, fMinAbs, before) : 0; // Preserve phase across timing jitter; skip missed slots.
    __atomic_add_fetch(&fPresentCount, 1, __ATOMIC_RELAXED);
    static uint64_t n,total,max,last;
    uint64_t elapsed=0,gap=0,uptime=0;
    absolutetime_to_nanoseconds(finished-before,&elapsed);
    if(last)absolutetime_to_nanoseconds(before-last,&gap);
    absolutetime_to_nanoseconds(before,&uptime);last=before;
    ++n;total+=elapsed;if(elapsed>max)max=elapsed;
    if(n<=48 || n%100==0) IOLog("UMI FRAME n=%llu t_ms=%llu gap_ms=%llu copy_us=%llu max_us=%llu\n",n,uptime/1000000,gap/1000000,elapsed/1000,max/1000);

}

void
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);


	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			sweet_console_muted = 1;
		}
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		if (!SweetGPUWaitForSurface(s, 50)) {
			static unsigned timeouts = 0;
			if (timeouts++ < 8) IOLog("UMI FB: GPU surface %u fence not complete, skip CPU read\n", s->id);
			s->release();
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->allocSize || s->wrapDelta > s->memory->getLength() || last > s->memory->getLength()-s->wrapDelta) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
            // Sparse RGB samples distinguish renderer-produced red from a
            // failed copy. Do not dump a screen or read outside the clipped rect.
            static unsigned samples=0;
            uint32_t sourceHash=2166136261U,destHash=2166136261U,red=0,black=0,white=0,alpha=0;
            bool sample=samples<48;
            if(sample)for(int j=0;j<32;++j)for(int i=0;i<16;++i){
                unsigned yy=(unsigned)((int64_t)(h-1)*j/31),xx=(unsigned)((int64_t)(w-1)*i/15);
                uint32_t v;memcpy(&v,(const void*)(src+s->offset+(uint64_t)(sy+yy)*s->bytesPerRow+(uint64_t)(sx+xx)*4),4);
                alpha+=(v>>24)!=255;v&=0xffffff;sourceHash=(sourceHash^v)*16777619U;
                red+=((v>>16)>160 && ((v>>8)&255)<80 && (v&255)<80);
                black+=((v>>16)<16 && ((v>>8)&255)<16 && (v&255)<16);
                white+=((v>>16)>200 && ((v>>8)&255)>200 && (v&255)>200);
            }

			for (int32_t y = 0; y < h; y++) {
				memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
				    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
				    (size_t)w * 4);
			}
            if(sample){
                __asm__ volatile("dsb sy":::"memory");
                for(int j=0;j<32;++j)for(int i=0;i<16;++i){
                    unsigned yy=(unsigned)((int64_t)(h-1)*j/31),xx=(unsigned)((int64_t)(w-1)*i/15);
                    uint32_t v=*(const volatile uint32_t*)(fPixels+(uint64_t)(dy+yy)*fRowBytes+(uint64_t)(dx+xx)*4);
                    destHash=(destHash^(v&0xffffff))*16777619U;
                }
                ++samples;IOLog("UMI PIX n=%u fmt=%x layer=%u red=%u black=%u white=%u alpha=%u src=%08x dst=%08x equal=%u\n",samples,s->pixelFormat,layer,red,black,white,alpha,sourceHash,destHash,sourceHash==destHash);
            }
		}
		s->release();
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	printf("SWEET FB: user client start task=%p\n", (void *)fTask);
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

static IOReturn
umi_fb_get_mode(IOExternalMethodArguments *args)
{
    uint64_t n=args->structureOutputDescriptor != NULL ? args->structureOutputDescriptor->getLength() : args->structureOutputSize;
    if(args->scalarInputCount<1 || n>4096)return kIOReturnBadArgument;
    if(!n)return kIOReturnSuccess;
    bool descriptor=args->structureOutputDescriptor != NULL;
    uint8_t *bytes=descriptor ? (uint8_t*)IOMallocData((size_t)n) : (uint8_t*)args->structureOutput;
    if(bytes==NULL)return descriptor?kIOReturnNoMemory:kIOReturnBadArgument;
    IOReturn ret=umi_fb_mode_block(args->scalarInput[0],bytes,n)?kIOReturnSuccess:kIOReturnBadArgument;
    if(ret==kIOReturnSuccess && descriptor) {
        IOMemoryDescriptor *out=args->structureOutputDescriptor;
        ret=out->prepare();
        if(ret==kIOReturnSuccess){
            bool full=out->writeBytes(0,bytes,n)==n;
            IOReturn completed=out->complete();
            if(!full)ret=kIOReturnVMError;
            else if(completed!=kIOReturnSuccess)ret=completed;
            else args->structureOutputDescriptorSize=n;
        }
    }
    if(descriptor)IOFreeData(bytes,(size_t)n);
    static uint32_t logged;
    if(__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)<6)
        IOLog("UMI DISPLAY mode block=0x%llx bytes=%llu result=0x%x\n",args->scalarInput[0],n,ret);
    return ret;
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBGetBlock:
        return umi_fb_get_mode(args);
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			IOLog("SWEET FB: display %s\n", off ? "off" : "on");
		}
		break;
	case kSweetFBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */
