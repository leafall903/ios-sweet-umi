/* Sandbox op 4 for the umi iOS16 research kernel.
 * Based on HakiOS kern_minimac.c, successful widget commit
 * 2fbf11026a9980817612ddbafab58fdd02dd991c (APSL 2.0).
 * Unlike a whole-string-area search, use XNU's recorded initial env[] table
 * (CONFIG_DTRACE p_dtrace_envp), never an argv lookalike or invented path.
 * No path, UUID, environment string or device identity is logged here.
 */
#ifndef UMI_SANDBOX_CONTAINER_H
#define UMI_SANDBOX_CONTAINER_H
#ifndef UMI_SANDBOX_CONTAINER_HOST_TEST
#include <config_dtrace.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/proc_internal.h>
#include <sys/systm.h>
#include <kern/task.h>
#include <libkern/libkern.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#endif

struct umi_sandbox_container_args64 {
    int64_t pid;
    uint64_t pidversion;
    user_addr_t buffer;
    user_size_t size;
    uint64_t check_pidversion;
};
_Static_assert(sizeof(user_addr_t) == 8 && sizeof(user_size_t) == 8, "umi op4 only supports the arm64 user ABI");
_Static_assert(sizeof(struct umi_sandbox_container_args64) == 40, "op4 argument size");
_Static_assert(__builtin_offsetof(struct umi_sandbox_container_args64, buffer) == 16, "op4 output pointer offset");
_Static_assert(__builtin_offsetof(struct umi_sandbox_container_args64, size) == 24, "op4 output length offset");
_Static_assert(__builtin_offsetof(struct umi_sandbox_container_args64, check_pidversion) == 32, "op4 pid-version flag offset");

static int
umi_sandbox_container_path(struct proc *caller, user_addr_t arg)
{
#if !CONFIG_DTRACE
    (void)caller; (void)arg;
    return ENOTSUP; /* Never replace an unavailable env[] record by an argv scan. */
#else
    static const char prefix[] = "CFFIXED_USER_HOME=";
    struct umi_sandbox_container_args64 args;
    proc_t target;
    task_t task;
    vm_map_t map;
    vm_map_copy_t copy = NULL;
    vm_offset_t snapshot = 0;
    vm_size_t snapshot_size;
    user_addr_t top, envp, string_low, cursor;
    size_t argslen;
    vm_map_offset_t effective_mask;
    const char *home = NULL;
    size_t home_length = 0;
    int error;

    if (caller == PROC_NULL || !proc_is64bit_data(caller)) return ENOTSUP;
    error = copyin(arg, &args, sizeof(args));
    if (error) return error;
    if (args.pid <= 0 || (uint64_t)args.pid > 0x7fffffffULL || args.check_pidversion > 1) return EINVAL;
    if (args.buffer == 0) return EFAULT;
    if (args.size == 0) return ENAMETOOLONG;
    target = proc_find((int)args.pid);
    if (target == PROC_NULL) return ESRCH;
    if (args.check_pidversion && (uint64_t)proc_pidversion(target) != args.pidversion) {
        proc_rele(target); return ESRCH;
    }
    if (!proc_is64bit_data(target)) { proc_rele(target); return ENOTSUP; }
    proc_lock(target);
    top = target->user_stack;
    argslen = target->p_argslen;
    envp = target->p_dtrace_envp;
    proc_unlock(target);
    task = proc_task(target);
    if (!top || !envp || !argslen || task == TASK_NULL) { proc_rele(target); return ENOENT; }
    if (argslen > ARG_MAX || top < argslen || (envp & 7) || envp >= top - argslen) {
        proc_rele(target); return EINVAL;
    }
    string_low = top - argslen;
    /* Same task-to-map lifetime pattern as XNU8796 sysctl_procargsx(). */
    task_reference(task);
    proc_rele(target);
    map = get_task_map_reference(task);
    task_deallocate(task);
    if (map == VM_MAP_NULL) return ENOENT;
    effective_mask = vm_map_page_mask(map);
    /* The saved initial user stack top is page-aligned on this arm64 target. */
    if ((top & effective_mask) || envp > top || top - envp > 2ULL * ARG_MAX) {
        vm_map_deallocate(map); return EINVAL;
    }
    user_addr_t start = envp & ~(user_addr_t)effective_mask;
    snapshot_size = (vm_size_t)(top - start);
    if (!snapshot_size || snapshot_size > 2ULL * ARG_MAX + effective_mask) {
        vm_map_deallocate(map); return EINVAL;
    }
    if (kmem_alloc(kernel_map, &snapshot, snapshot_size, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
        vm_map_deallocate(map); return ENOMEM;
    }
    if (vm_map_copyin(map, (vm_map_address_t)start, snapshot_size, FALSE, &copy) != KERN_SUCCESS) {
        vm_map_deallocate(map);
        if (copy != NULL) vm_map_copy_discard(copy);
        kmem_free(kernel_map, snapshot, snapshot_size);
        return EFAULT;
    }
    vm_map_deallocate(map);
    if (vm_map_copy_overwrite(kernel_map, snapshot, copy, snapshot_size, FALSE) != KERN_SUCCESS) {
        vm_map_copy_discard(copy);
        kmem_free(kernel_map, snapshot, snapshot_size);
        return EFAULT;
    }
    /* Successful overwrite consumes the map-copy object. */
    copy = NULL;
    error = EINVAL;
    for (cursor = envp; cursor <= string_low && string_low - cursor >= sizeof(uint64_t); cursor += sizeof(uint64_t)) {
        uint64_t address;
        memcpy(&address, (const void *)(snapshot + (cursor - start)), sizeof(address));
        if (address == 0) { error = home ? 0 : ENOENT; break; }
        if (address < string_low || address >= top) break;
        const char *value = (const char *)(snapshot + (address - start));
        size_t available = (size_t)(top - address);
        size_t length = strnlen(value, available);
        if (length == available) break;
        if (!home && length >= sizeof(prefix) - 1 && memcmp(value, prefix, sizeof(prefix) - 1) == 0) {
            home = value + sizeof(prefix) - 1;
            home_length = length - (sizeof(prefix) - 1);
            if (!home_length || home[0] != '/' || home_length >= MAXPATHLEN) { error = EINVAL; break; }
        }
    }
    if (error == 0) {
        if (home_length >= args.size) error = ENAMETOOLONG;
        else if (args.buffer > UINT64_MAX - (home_length + 1)) error = EFAULT;
        else error = copyout(home, args.buffer, home_length + 1);
    }
    kmem_free(kernel_map, snapshot, snapshot_size);
    return error;
#endif
}
#endif
