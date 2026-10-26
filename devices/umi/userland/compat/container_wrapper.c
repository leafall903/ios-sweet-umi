/* Versioned successor to the local umi-compat-candidate wrapper.
 * Based on MakrSas/ios-topaz's bbd_wrap.c pattern. This is an offline candidate.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "probe_log.h"
#define LIB "/usr/local/lib/umi-ios165-v1/libumi-ios165-v1-container.dylib"
int main(int argc, char **argv) {
    (void)argc;
    const char *name = getprogname(), *real = NULL;
    if (name && strcmp(name, "containermanagerd") == 0) real = "/usr/libexec/real/containermanagerd";
    if (name && strcmp(name, "containermanagerd_system") == 0) real = "/usr/libexec/real/containermanagerd_system";
    if (!real || !argv) return 127;
    umi_ios165_log("wrapper_enter", name);
    if (access(LIB, R_OK) != 0) {
        umi_ios165_log("library_missing", LIB);
    } else if (setenv("DYLD_INSERT_LIBRARIES", LIB, 1) != 0) {
        umi_ios165_log("environment_failed", "original executable still attempted");
    } else {
        umi_ios165_log("exec_with_library_requested", LIB);
    }
    argv[0] = (char *)real;
    execv(real, argv);
    umi_ios165_log("exec_failed", strerror(errno));
    return 127;
}
