/*
 * pipe2() for iOS.
 *
 * The iOS SDK declares pipe2 only for iOS 27 and later, so a build made with
 * that SDK but deployed further back weak-imports the symbol. glib detects
 * pipe2 at configure time all the same, and g_unix_open_pipe() tail-calls it;
 * on any older phone the weak import resolves to NULL and the process jumps to
 * address zero the moment QEMU opens its first event notifier — a segfault in
 * event_notifier_init() before the machine even starts.
 *
 * A strong definition here wins over the weak import for everything linked
 * into this library, glib included, and is what actually runs on the phone.
 */
#include "qemu/osdep.h"

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

    #include <fcntl.h>
    #include <unistd.h>

int pipe2(int fds[2], int flags);

int pipe2(int fds[2], int flags)
{
    int i;

    if (pipe(fds) != 0) { return -1; }

    for (i = 0; i < 2; i++) {
        int want = 0;

        if ((flags & O_CLOEXEC) != 0 && fcntl(fds[i], F_SETFD, FD_CLOEXEC) == -1) { goto fail; }
        if ((flags & O_NONBLOCK) != 0) { want |= O_NONBLOCK; }
        if (want != 0 && fcntl(fds[i], F_SETFL, want) == -1) { goto fail; }
    }

    return 0;

fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

#endif
