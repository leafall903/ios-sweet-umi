/*
 * A55 SMP bring-up for sweet using XNU's legacy IOCPU interface and PSCI.
 * The architected timer and SGI1 already enter through pe_fiq/sleh_irq.
 * CPU_OFF, suspend and the big-cluster cores still require separate work.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <arm64/proc_reg.h>
#include <machine/atomic.h>
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "UmiSmpMode.h"

/*
 * <arm/caches_internal.h> is a C header without an extern "C" wrapper: it
 * would give these C functions C++ linkage here, so declare the one we use
 * instead.  gVirtBase/gPhysBase are only declared by <mach/vm_param.h> for
 * MACH_KERNEL_PRIVATE builds, which the iokit is not.
 */
extern "C" {
void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern unsigned long gVirtBase, gPhysBase;
}

extern "C" void sweet_secondary_entry(void);
extern "C" boolean_t sweet_cpu_signal_is_enabled(unsigned int cpu);
extern "C" uint32_t sweet_cpu_signal_raw(unsigned int cpu);

/*
 * Left by a CPU that takes an exception between its first instruction and
 * the point where common_start installs the kernel's vectors
 * (Lsecondary_early_exception in start.s; that CPU then switches itself
 * off).  It has a cache line to itself: the writer has its caches off, and
 * the processor that started it reads the note back from memory.
 */
extern "C" {
struct sweet_smp_early_fault {
	uint64_t        valid;          /* 1 once the fields below are written */
	uint64_t        mpidr;
	uint64_t        esr;
	uint64_t        elr;
	uint64_t        far;
	uint64_t        lr;
	uint64_t        sp;
	uint64_t        spare;
	uint64_t        pad[8];         /* fields keep a cache line to themselves */
};

struct sweet_smp_early_fault sweet_smp_start_fault __attribute__((aligned(128)));
}

static_assert(sizeof(struct sweet_smp_early_fault) == 128, "one cache line");

class SweetCPU;
static SweetCPU *sweet_cpus[MAX_CPUS];
static uint32_t sweet_cpus_registered;

/*
 * Boot-arg sweetaff1max=N: only start CPUs whose MPIDR Aff1 (the core
 * number on Qualcomm) is <= N.  The device tree lists CPUs in ascending
 * Aff1 order and umi's four Cortex-A77s sit at Aff1 4..7, above the four
 * A55s at 0..3, so "cpus=8 sweetaff1max=3" starts the A55s only and
 * "cpus=8 sweetaff1max=4" adds the first A77.  Default 7: every CPU the
 * device tree offers (the behaviour before this option existed).
 */
#define SWEET_AFF1_DEFAULT 7u
static uint32_t sweet_aff1_max = SWEET_AFF1_DEFAULT;
static bool sweet_cpu_args_parsed;

/*
 * Boot-arg sweetsmp_tolerant=1: report a secondary that does not come up
 * and continue with the CPUs that did, instead of panicking.  Diagnosis
 * only: XNU has no way to unregister a processor that was handed to
 * processor_start() and never arrived, so a failed one stays in the
 * PROCESSOR_START state inside its processor set.  The scheduler must not
 * dispatch to it, but nothing removes it from the set either.
 */
static bool sweet_smp_tolerant;

/* Secondary handshake diagnosis; see initCPU(), startAll() and the report. */
static bool sweet_smp_irqwait = true;
static uint32_t sweet_smp_sgiwait_ms = 100;
static bool sweet_smp_sgireq = true;
/* How long startCPU() waits for an early-fault note from the new CPU (ms). */
static uint32_t sweet_smp_faultwait_ms = 500;

static void
sweet_parse_cpu_args(void)
{
	uint32_t value;

	if (sweet_cpu_args_parsed) {
		return;
	}
	sweet_cpu_args_parsed = true;

	value = SWEET_AFF1_DEFAULT;
	if (PE_parse_boot_argn("sweetaff1max", &value, sizeof(value)) &&
	    value < SWEET_AFF1_DEFAULT) {
		sweet_aff1_max = value;
	}
	value = 0;
	if (PE_parse_boot_argn("sweetsmp_tolerant", &value, sizeof(value))) {
		sweet_smp_tolerant = (value != 0);
	}
	value = 1;
	if (PE_parse_boot_argn("sweetsmp_irqwait", &value, sizeof(value))) {
		sweet_smp_irqwait = (value != 0);
	}
	value = 100;
	if (PE_parse_boot_argn("sweetsmp_sgiwait_ms", &value, sizeof(value))) {
		sweet_smp_sgiwait_ms = value > 5000 ? 5000 : value;
	}
	value = 1;
	if (PE_parse_boot_argn("sweetsmp_sgireq", &value, sizeof(value))) {
		sweet_smp_sgireq = (value != 0);
	}
	value = 500;
	if (PE_parse_boot_argn("sweetsmp_faultwait_ms", &value, sizeof(value))) {
		sweet_smp_faultwait_ms = value > 5000 ? 5000 : value;
	}
	kprintf("SWEET CPU: boot args: sweetaff1max=%u sweetsmp_tolerant=%u "
	    "irqwait=%u sgiwait_ms=%u sgireq=%u faultwait_ms=%u\n",
	    sweet_aff1_max, sweet_smp_tolerant ? 1u : 0u,
	    sweet_smp_irqwait ? 1u : 0u, sweet_smp_sgiwait_ms,
	    sweet_smp_sgireq ? 1u : 0u, sweet_smp_faultwait_ms);
}

static bool
sweet_cpu_wanted(const ml_topology_cpu_t *cpu)
{
	/* Never filter out the CPU this kernel is running on. */
	if (cpu == ml_get_topology_info()->boot_cpu) {
		return true;
	}
	return (cpu->phys_id >> 8) <= sweet_aff1_max;
}

/* How many of the CPUs the device tree offered this boot will start. */
static uint32_t
sweet_cpu_expected(const ml_topology_info_t *topology)
{
	uint32_t count = 0;

	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		if (sweet_cpu_wanted(&topology->cpus[i])) {
			count++;
		}
	}
	return count;
}

/* SMCCC 1.0: x0 is the signed PSCI result; do not assume x1-x17 survive. */
static int32_t
sweet_psci_cpu_on(uint64_t target, uint64_t entry, uint64_t context)
{
	register uint64_t x0 __asm__("x0") = 0xC4000003ULL;
	register uint64_t x1 __asm__("x1") = target;
	register uint64_t x2 __asm__("x2") = entry;
	register uint64_t x3 __asm__("x3") = context;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "cc", "memory");
	return (int32_t)x0;
}

class SweetCPU : public IOCPU {
	OSDeclareDefaultStructors(SweetCPU);
	const OSSymbol *cpuName = NULL;
	uint32_t initialized = 0; /* release/acquire publication after initCPU sends SGI */

public:
	static void startAll(void *arg, wait_result_t result);
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	void haltCPU(void) APPLE_KEXT_OVERRIDE;
	void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE;
	const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetCPU, IOCPU);

bool
SweetCPU::start(IOService *provider)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	OSNumber *logical = OSDynamicCast(OSNumber, provider->getProperty("logical-cpu-id"));
	OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));

	if (logical == NULL || reg == NULL || reg->getLength() != sizeof(uint32_t) ||
	    logical->unsigned32BitValue() >= topology->num_cpus) {
		return false;
	}
	const ml_topology_cpu_t *cpu = &topology->cpus[logical->unsigned32BitValue()];
	if (*(const uint32_t *)reg->getBytesNoCopy() != cpu->phys_id) {
		return false;
	}
	/* MPIDR Aff1 = core: 0x000-0x300 A55, 0x400-0x700 A77 on umi. */
	if (cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0 ||
	    topology->num_cpus > SWEET_SMP_MAX_CPUS) {
		panic("SWEET CPU: invalid topology cpu=%u mpidr=0x%x count=%u",
		    cpu->cpu_id, cpu->phys_id, topology->num_cpus);
	}
	/* Boot-arg filter: CPUs we do not intend to start stay unregistered. */
	sweet_parse_cpu_args();
	if (!sweet_cpu_wanted(cpu)) {
		kprintf("SWEET CPU: cpu=%u mpidr=0x%x skipped (sweetaff1max=%u)\n",
		    cpu->cpu_id, cpu->phys_id, sweet_aff1_max);
		return false;
	}
	if (!IOCPU::start(provider)) {
		return false;
	}
	cpuName = OSSymbol::withCStringNoCopy("Kryo 470");
	if (cpuName == NULL) {
		return false;
	}
	setCPUNumber(cpu->cpu_id);

	ml_processor_info_t info = {};
	info.cpu_id = reinterpret_cast<cpu_id_t>(this);
	info.phys_id = cpu->phys_id;
	info.log_id = cpu->cpu_id;
	info.cluster_id = cpu->cluster_id;
	info.cluster_type = cpu->cluster_type;
	info.l2_cache_id = cpu->l2_cache_id;
	info.l2_cache_size = cpu->l2_cache_size;
	info.l3_cache_id = cpu->l3_cache_id;
	info.l3_cache_size = cpu->l3_cache_size;
	perfmon_interrupt_handler_func pmi_handler = NULL;
	kern_return_t result = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler);
	if (result != KERN_SUCCESS) {
		panic("SWEET CPU: ml_processor_register failed: %d", result);
	}
	if (pmi_handler != NULL) {
		panic("SWEET CPU: PMI routing is not implemented");
	}

	registerService();
	__atomic_store_n(&sweet_cpus[cpu->cpu_id], this, __ATOMIC_RELEASE);
	__atomic_add_fetch(&sweet_cpus_registered, 1, __ATOMIC_ACQ_REL);
	if (cpu == topology->boot_cpu) {
		thread_t thread;
		result = kernel_thread_start(&SweetCPU::startAll, NULL, &thread);
		if (result != KERN_SUCCESS) {
			panic("SWEET CPU: cannot create startup thread: %d", result);
		}
		thread_deallocate(thread);
	}
	return true;
}

/* Do not wait in a nub's start(): matching can be asynchronous or serialized. */
void
SweetCPU::startAll(void *arg __unused, wait_result_t result __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	const uint32_t expected = sweet_cpu_expected(topology);
	uint32_t requested = 1;
	uint32_t started = 0;
	uint32_t failed = 0;
	PE_parse_boot_argn("cpus", &requested, sizeof(requested));
    if(!umi_cpu_boot_valid(requested))panic("UMI GUI CPU mode refused");

	/*
	 * Every message in this function is kprintf, not IOLog: IOLog defers to
	 * the IOKit log thread, so a hang right after it (for instance inside a
	 * PSCI SMC that never returns) would leave no trace on the console.
	 */
	printf("SWEET CPU: requested=%u selected=%u expected=%u (aff1max=%u limit=%u)\n",
	    requested, topology->num_cpus, expected, sweet_aff1_max, SWEET_SMP_MAX_CPUS);
	printf("SWEET CPU: starting CPU set:");
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		if (sweet_cpu_wanted(&topology->cpus[i])) {
			printf(" %u/0x%x", i, topology->cpus[i].phys_id);
		}
	}
	printf("\n");
	if (expected == 0) {
		panic("SWEET CPU: no CPU selected (sweetaff1max=%u)", sweet_aff1_max);
	}

	uint64_t deadline;
	nanoseconds_to_deadline(10 * NSEC_PER_SEC, &deadline);
	while (__atomic_load_n(&sweet_cpus_registered, __ATOMIC_ACQUIRE) < expected) {
		if (mach_absolute_time() > deadline) {
			for (unsigned int i = 0; i < topology->num_cpus; i++) {
				if (sweet_cpu_wanted(&topology->cpus[i]) &&
				    __atomic_load_n(&sweet_cpus[i], __ATOMIC_ACQUIRE) == NULL) {
					printf("SWEET CPU: cpu=%u mpidr=0x%x NOT registered (IOKit matching timeout)\n",
					    i, topology->cpus[i].phys_id);
				}
			}
			panic("SWEET CPU: CPU registration timeout");
		}
		IOSleep(1);
	}
	/* The supplied ADT has cpu0 as the boot CPU and ascending A55 nodes. */
	if (topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != 0) {
		panic("SWEET CPU: expected boot MPIDR 0 and logical CPU 0");
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		const ml_topology_cpu_t *tcpu = &topology->cpus[i];
		SweetCPU *cpu;
		kern_return_t kr;

		if (!sweet_cpu_wanted(tcpu)) {
			continue;
		}
		cpu = __atomic_load_n(&sweet_cpus[i], __ATOMIC_ACQUIRE);
		if (cpu == NULL) {
			failed++;
			printf("SWEET CPU: cpu=%u mpidr=0x%x was not registered; not started\n",
			    i, tcpu->phys_id);
			if (!sweet_smp_tolerant) {
				panic("SWEET CPU: cpu=%u missing from the registry", i);
			}
			continue;
		}
		printf("SWEET CPU: starting cpu=%u aff1=%u mpidr=0x%x (%u of %u)\n",
		    i, tcpu->phys_id >> 8, tcpu->phys_id, started + 1, expected);
		kr = processor_start(cpu->machProcessor);
		if (kr != KERN_SUCCESS) {
			failed++;
			printf("SWEET CPU: cpu=%u processor_start failed: %d\n", i, kr);
			if (!sweet_smp_tolerant) {
				panic("SWEET CPU: cpu=%u processor_start failed: %d", i, kr);
			}
			continue;
		}
		printf("SWEET SMP: cpu=%u processor_start returned %d DAIF=0x%llx\n",
		    i, kr, __builtin_arm_rsr64("DAIF"));
		/* processor_start() is asynchronous for secondaries in this XNU. */
		bool alive = true;
		nanoseconds_to_deadline(5 * NSEC_PER_SEC, &deadline);
		uint64_t report = mach_absolute_time();
		while (__atomic_load_n(&cpu->initialized, __ATOMIC_ACQUIRE) == 0 ||
		    (sweet_smp_sgireq && !sweet_cpu_signal_is_enabled(i))) {
			uint64_t now = mach_absolute_time();

			if (now > deadline) {
				alive = false;
				break;
			}
			if (now >= report) {
				printf("SWEET SMP: cpu=%u still starting initCPU=%u signal=0x%x now=%llu "
				    "deadline=%llu\n", i,
				    __atomic_load_n(&cpu->initialized, __ATOMIC_ACQUIRE),
				    sweet_cpu_signal_raw(i), now, deadline);
				nanoseconds_to_deadline(500 * NSEC_PER_MSEC, &report);
			}
			IOSleep(1);
		}
		if (!alive) {
			failed++;
			printf("SWEET SMP: cpu=%u wait timed out on cpu%u: initCPU=%u signal=0x%x "
			    "sgireq=%u tolerant=%u\n", i, cpu_number(),
			    __atomic_load_n(&cpu->initialized, __ATOMIC_ACQUIRE),
			    sweet_cpu_signal_raw(i), sweet_smp_sgireq ? 1u : 0u,
			    sweet_smp_tolerant ? 1u : 0u);
			printf("SWEET CPU: cpu=%u aff1=%u mpidr=0x%x did NOT come up in 5s "
			    "(initCPU=%u signal=%u)\n", i, tcpu->phys_id >> 8, tcpu->phys_id,
			    __atomic_load_n(&cpu->initialized, __ATOMIC_ACQUIRE),
			    sweet_cpu_signal_is_enabled(i) ? 1u : 0u);
			if (!sweet_smp_tolerant) {
				panic("SWEET CPU: cpu=%u startup timeout (PSCI/entry/reset/GIC/SGI)", i);
			}
			continue;
		}
		started++;
		printf("SWEET CPU: cpu=%u mpidr=0x%x came alive; SGI1 OK (%u/%u)\n",
		    i, tcpu->phys_id, started, expected);
	}
	/* Publish exactly once, after every selected CPU has completed initCPU(). */
	printf("SWEET SMP: %u alive, ml_cpu_init_completed next\n", started);
	ml_cpu_init_completed();
	printf("SWEET SMP: ml_cpu_init_completed returned\n");
	SweetCPU *boot = __atomic_load_n(&sweet_cpus[topology->boot_cpu->cpu_id], __ATOMIC_ACQUIRE);
	if (boot != NULL) {
		printf("SWEET SMP: cpusRunning\n");
		boot->cpusRunning();
		printf("SWEET SMP: cpusRunning returned\n");
	}
	printf("SWEET SMP: publishResource\n");
	IOService::publishResource(gIOAllCPUInitializedKey, kOSBooleanTrue);
	printf("SWEET SMP: publishResource returned\n");
	ml_set_max_cpus(topology->max_cpu_id + 1);
	printf("SWEET SMP: ml_set_max_cpus(%u) returned\n", topology->max_cpu_id + 1);
	printf("SWEET CPU: %u/%u CPUs initialized, SGI1 verified (failed=%u)\n",
	    started, expected, failed);
	printf("SWEET SMP: startAll terminating its thread\n");
	thread_terminate(current_thread());
}

void
SweetCPU::free(void)
{
	OSSafeReleaseNULL(cpuName);
	IOCPU::free();
}

void
SweetCPU::initCPU(bool boot)
{
	if (!boot) {
		panic("SWEET CPU: cpu=%u resume is not implemented", getCPUNumber());
	}
	assert(cpu_number() == (int)getCPUNumber());

	sweet_parse_cpu_args();

	kprintf("SWEET SMP: cpu=%u initCPU enter DAIF=0x%llx signal=0x%x\n",
	    getCPUNumber(), __builtin_arm_rsr64("DAIF"),
	    sweet_cpu_signal_raw(getCPUNumber()));

	ml_broadcast_cpu_event(CPU_BOOTED, getCPUNumber());
	kprintf("SWEET SMP: cpu=%u broadcast CPU_BOOTED done\n", getCPUNumber());

	/* Secondary startup runs at splsched: queue the self-IPI and return.
	 * startAll waits for its delivery after the startup thread unmasks IRQs.
	 * Waiting here with DAIF.I set would deadlock every secondary. */
	signalCPU(this);
	kprintf("SWEET SMP: cpu=%u self-SGI queued signal=0x%x\n",
	    getCPUNumber(), sweet_cpu_signal_raw(getCPUNumber()));

	/*
	 * While DAIF.I is set the SGI stays pending, so nothing proves that this
	 * CPU can take an interrupt until the idle thread later calls spllo().
	 * Open DAIF here for a bounded window instead: either the SGI arrives and
	 * SIGPdisabled is cleared before we return, or the log says it did not.
	 */
	if (sweet_smp_irqwait && !ml_get_interrupts_enabled()) {
		uint64_t start = mach_absolute_time();
		uint64_t deadline;

		(void)ml_set_interrupts_enabled(TRUE);
		nanoseconds_to_deadline(sweet_smp_sgiwait_ms * NSEC_PER_MSEC, &deadline);
		while (!sweet_cpu_signal_is_enabled(getCPUNumber()) &&
		    mach_absolute_time() < deadline) {
			__asm__ volatile("yield");
		}
		(void)ml_set_interrupts_enabled(FALSE);
		kprintf("SWEET SMP: cpu=%u self-SGI %s after %llu us signal=0x%x DAIF=0x%llx\n",
		    getCPUNumber(),
		    sweet_cpu_signal_is_enabled(getCPUNumber()) ? "DELIVERED" : "NOT DELIVERED",
		    (mach_absolute_time() - start) / 1000ULL,
		    sweet_cpu_signal_raw(getCPUNumber()),
		    __builtin_arm_rsr64("DAIF"));
	} else {
		kprintf("SWEET SMP: cpu=%u IRQ window skipped (irqwait=%u IRQs=%u)\n",
		    getCPUNumber(), sweet_smp_irqwait ? 1u : 0u,
		    ml_get_interrupts_enabled() ? 1u : 0u);
	}

	setCPUState(kIOCPUStateRunning);
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	kprintf("SWEET CPU: cpu=%u entered initCPU mpidr=0x%llx aff1=%llu EL%llu IRQs=%u\n",
	    getCPUNumber(), mpidr, (mpidr >> 8) & 0xffULL,
	    __builtin_arm_rsr64("CurrentEL") >> 2, ml_get_interrupts_enabled());
	__atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
	kprintf("SWEET SMP: cpu=%u initCPU done, returning to cpu_machine_init DAIF=0x%llx\n",
	    getCPUNumber(), __builtin_arm_rsr64("DAIF"));
}

void
SweetCPU::signalCPU(IOCPU *target)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	unsigned int logical = target->getCPUNumber();
	if (logical >= topology->num_cpus) {
		panic("SWEET CPU: invalid SGI target %u", logical);
	}
	/* Use the TARGET affinity, including Aff1, never the sender's MPIDR. */
	uint64_t mpidr = topology->cpus[logical].phys_id;
	unsigned int aff0 = mpidr & 0xff;
	if (aff0 >= 16) {
		panic("SWEET CPU: SGI target requires unsupported range selector");
	}
	uint64_t sgi = (((mpidr >> 32) & 0xff) << 48) |
	    (((mpidr >> 16) & 0xff) << 32) |
	    (((mpidr >> 8) & 0xff) << 16) |
	    ((uint64_t)SWEET_IPI_SGI << 24) | (1ULL << aff0);
	__builtin_arm_dsb(DSB_ISHST);
	__builtin_arm_wsr64("ICC_SGI1R_EL1", sgi);
	__builtin_arm_isb(ISB_SY);
}

kern_return_t
SweetCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
	const ml_topology_cpu_t *cpu = &ml_get_topology_info()->cpus[getCPUNumber()];
	/* cpu_start() passes NULL,NULL: x20/x21 are set by LowResetVectorBase. */
	if (start_paddr != 0 || arg_paddr != 0 || cpu->phys_id == 0 ||
	    __atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 0) {
		panic("SWEET CPU: cpu=%u unsupported restart", getCPUNumber());
	}
	vm_offset_t entry = ml_static_vtop((vm_offset_t)&sweet_secondary_entry);

	/*
	 * Forget any note from an earlier attempt and put the zero in memory.
	 * cpu_start() has just cleaned the kernel segments and every cpu_data
	 * for this core (osfmk/arm64/cpu.c), which is what the new core reads
	 * with its MMU and caches off; all that is left to publish is this note.
	 */
	sweet_smp_start_fault.valid = 0;
	FlushPoC_DcacheRegion((vm_offset_t)&sweet_smp_start_fault, sizeof(sweet_smp_start_fault));

	/*
	 * kprintf, not IOLog: if the SMC below never returns (firmware stuck on
	 * that core/cluster) this line is the last thing the console ever shows,
	 * so it must not be sitting in the deferred IOKit log queue.
	 */
	kprintf("SWEET CPU: cpu=%u CPU_ON target=0x%x entry_pa=0x%llx context=%u "
	    "caller_mpidr=0x%llx: calling SMC\n", getCPUNumber(), cpu->phys_id,
	    (uint64_t)entry, getCPUNumber(), __builtin_arm_rsr64("MPIDR_EL1"));
	__builtin_arm_dsb(DSB_SY);
	int32_t rc = sweet_psci_cpu_on(cpu->phys_id, entry, getCPUNumber());
	kprintf("SWEET CPU: cpu=%u PSCI CPU_ON rc=%d\n", getCPUNumber(), rc);
	/* cpu_start() discards PE_cpu_start's return; do not silently carry on. */
	if (rc != 0) {
		/* -1 NOT_SUPPORTED, -2 INVALID_PARAMETERS, -3 ALREADY_ON, -4 ON_PENDING */
		panic("SWEET CPU: cpu=%u NOT alive: PSCI CPU_ON rc=%d (mpidr=0x%x)",
		    getCPUNumber(), rc, cpu->phys_id);
	}

	/*
	 * From sweet_secondary_entry to common_start the new core runs with its
	 * MMU and caches off.  An exception there leaves a note in
	 * sweet_smp_start_fault (Lsecondary_early_exception) and switches the
	 * core off; without this loop the only evidence would be startAll()'s
	 * five-second timeout.  Read the note from memory, not from this
	 * processor's copy of it.
	 */
	if (sweet_smp_faultwait_ms != 0) {
		uint64_t deadline;

		nanoseconds_to_deadline((uint64_t)sweet_smp_faultwait_ms * NSEC_PER_MSEC,
		    &deadline);
		do {
			FlushPoC_DcacheRegion((vm_offset_t)&sweet_smp_start_fault,
			    sizeof(sweet_smp_start_fault));
			if (sweet_smp_start_fault.valid != 0) {
				uint64_t to_virtual = (uint64_t)gVirtBase - (uint64_t)gPhysBase;
				uint64_t esr = sweet_smp_start_fault.esr;

				kprintf("SWEET SMP: CPU%u early fault ESR=0x%llx ELR=0x%llx "
				    "FAR=0x%llx mpidr=0x%llx LR=0x%llx SP=0x%llx\n",
				    getCPUNumber(), esr, sweet_smp_start_fault.elr,
				    sweet_smp_start_fault.far, sweet_smp_start_fault.mpidr,
				    sweet_smp_start_fault.lr, sweet_smp_start_fault.sp);
				kprintf("SWEET SMP: cpu=%u early fault EC=0x%llx ISS=0x%llx "
				    "ELR/LR as kernel VA 0x%llx/0x%llx: the core is off\n",
				    getCPUNumber(), esr >> 26, esr & 0x3ffffffULL,
				    sweet_smp_start_fault.elr + to_virtual,
				    sweet_smp_start_fault.lr + to_virtual);
				return KERN_FAILURE;
			}
			if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 0) {
				break;
			}
			delay_for_interval(1, NSEC_PER_MSEC);
		} while (mach_absolute_time() < deadline);
	}
	return KERN_SUCCESS;
}

void
SweetCPU::quiesceCPU(void)
{
	panic("SWEET CPU: CPU quiesce is not implemented");
}

void
SweetCPU::haltCPU(void)
{
	panic("SWEET CPU: CPU halt/suspend is not implemented");
}

const OSSymbol *
SweetCPU::getCPUName(void)
{
	return cpuName;
}
#endif /* SWEET */
