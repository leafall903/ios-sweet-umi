#ifndef UMI_IOS165_PROBE_LOG_H
#define UMI_IOS165_PROBE_LOG_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
/* One file per process, same PID before/after exec. Never follows a symlink. */
static void umi_ios165_log(const char *event, const char *detail) {
    int saved_errno = errno;
    char path[96], line[512];
    int n = snprintf(path, sizeof path, "/private/var/tmp/umi-ios165-v1.%ld.log", (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof path) { errno = saved_errno; return; }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid()) {
        n = snprintf(line, sizeof line, "UMI-IOS165-V1 pid=%ld uid=%ld event=%s %s\n", (long)getpid(), (long)getuid(), event, detail ? detail : "");
        if (n > 0 && (size_t)n < sizeof line) {
            (void)write(fd, line, (size_t)n);
            (void)write(STDERR_FILENO, line, (size_t)n);
        }
    }
    if (fd >= 0) close(fd);
    errno = saved_errno;
}
#endif
