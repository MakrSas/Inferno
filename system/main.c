/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2020 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "system/system.h"

#ifdef CONFIG_SDL
    /*
     * SDL insists on wrapping the main() function with its own implementation on
     * some platforms; it does so via a macro that renames our main function, so
     * <SDL.h> must be #included here even with no SDL code called from this file.
     */
    #include <SDL.h>
#endif

#if defined(CONFIG_DARWIN) && !defined(CONFIG_IOS)
    #include <CoreFoundation/CoreFoundation.h>
#endif

static void* qemu_default_main(void* opaque)
{
    int status;

    bql_lock();
    status = qemu_main_loop();
    qemu_cleanup(status);
    bql_unlock();

    exit(status);
}

int (*qemu_main)(void);

/*
 * On macOS the UI needs the main thread, so QEMU hands it to CFRunLoop and runs
 * its own loop on a worker. On iOS the embedding app owns the run loop and
 * calls qemu_init()/qemu_main_loop() itself, so this must stay unset.
 */
#if defined(CONFIG_DARWIN) && !defined(CONFIG_IOS)
static int os_darwin_cfrunloop_main(void)
{
    CFRunLoopRun();
    assert_not_reached();
}
int (*qemu_main)(void) = os_darwin_cfrunloop_main;
#endif

#ifdef __ANDROID__
/*
 * Bionic's loader requires the main executable's PT_TLS segment to be aligned
 * to at least 64 bytes on arm64, and aborts at startup otherwise. QEMU's own
 * thread-locals only ask for 8, so pin the alignment here.
 */
static __thread char android_tls_align_pad __attribute__((aligned(64), used));
#endif

int main(int argc, char** argv)
{
    qemu_init(argc, argv);

    /*
     * qemu_init acquires the BQL and replay mutex lock. BQL is acquired when
     * initializing cpus, to block associated threads until initialization is
     * complete.
     *
     * On MacOS, qemu main event loop runs in a background thread, as main
     * thread must be reserved for UI. Thus, we need to transfer lock ownership,
     * and the simplest way to do that is to release them, and reacquire them
     * from qemu_default_main.
     */
    bql_unlock();

    if (qemu_main) {
        QemuThread main_loop_thread;
        qemu_thread_create(&main_loop_thread, "qemu_main", qemu_default_main, NULL, QEMU_THREAD_DETACHED);
        return qemu_main();
    }
    else {
        qemu_default_main(NULL);
        assert_not_reached();
    }
}
