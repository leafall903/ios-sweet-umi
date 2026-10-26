/* Compatibility ABI from HakiOS kern_minimac.c @5fbd7fd9; APSL derivative.
 * Caller supplies checked user-memory copy primitives. No firmware or MMIO.
 */
#ifndef UMI_AMFI_POLICY_H
#define UMI_AMFI_POLICY_H
#include <stdint.h>
#include <stddef.h>
#include <sys/errno.h>
struct UmiAmfiPolicyIo {
 int (*read)(uint64_t, void *, size_t);
 int (*write)(const void *, uint64_t, size_t);
};
static inline int umi_amfi_policy_dispatch(unsigned enabled, int call, uint64_t arg,
 const struct UmiAmfiPolicyIo *io) {
 struct {uint64_t input_flags, output_flags;} args;
 const uint64_t dyld_flags=0xffu, extension_state=0x86u;
 if(enabled!=1 || (call!=0x5a && call!=0x60)) return ENOPOLICY;
 if(call==0x60) return io->write(&extension_state,arg,sizeof(extension_state));
 int error=io->read(arg,&args,sizeof(args));
 return error ? error : io->write(&dyld_flags,args.output_flags,sizeof(dyld_flags));
}
#endif
