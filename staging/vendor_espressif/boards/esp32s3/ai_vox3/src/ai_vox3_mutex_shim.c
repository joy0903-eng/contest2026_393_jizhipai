/****************************************************************************
 * src/ai_vox3_mutex_shim.c
 *
 * Link-time provider for the nxmutex_* family.
 *
 * Background (run#31 / run#32): libdrivers.a(es8311.o) emits undefined
 * references to nxmutex_init/nxmutex_lock/nxmutex_unlock.  The intended
 * resolution paths are
 *   a) CONFIG_LIBC_SEM_MUTEX_NOINLINE=n: include/nuttx/mutex.h compiles
 *      every nxmutex_* as a static inline -- no external symbol needed;
 *   b) CONFIG_LIBC_SEM_MUTEX_NOINLINE=y: the real implementations come
 *      from libs/libc/misc/lib_sem_mutex_noinline.c (libc.a), which
 *      re-includes the headers with 'static' blanked.
 * Yet on the CI runner es8311.o carried the extern references in BOTH
 * configurations (run#31 default and run#32 with the symbol pinned off),
 * while the libc.a implementation was not pulled in to resolve them.
 *
 * This file implements (b) unconditionally for our build: force the
 * headers' static-inline branch, blank 'static', and re-include so the
 * nxmutex_* family becomes a set of real global definitions inside
 * libboard.a.  libboards is linked after libdrivers in NUTTXLIBS
 * (tools/FlatLibs.mk), so these members resolve es8311.o's references.
 *
 * Safety against duplicate symbols:
 *  - With CONFIG_LIBC_SEM_MUTEX_NOINLINE pinned off (defconfig + audit
 *    gate), libc.a's lib_sem_mutex_noinline.o compiles EMPTY and defines
 *    nothing; sched.a defines only the nxsem_* primitives and the *_slow
 *    functions, none of which collide with nxmutex_*.
 *  - Calls made from inside the generated bodies (nxsem_wait etc.) are
 *    either generated here as well (same re-inclusion trick) or resolved
 *    by the real sched.a implementations.
 *
 * The build.yml CONFIG audit gate fails the build if the NOINLINE symbol
 * ever flips back on, which keeps this file conflict-free.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* Force the static-inline branch of mutex.h/semaphore.h and emit the
 * inline bodies as real global functions.  Mirrors
 * libs/libc/misc/lib_sem_mutex_noinline.c exactly, minus its outer
 * CONFIG_LIBC_SEM_MUTEX_NOINLINE gate. */

#undef CONFIG_LIBC_SEM_MUTEX_NOINLINE
#undef __INCLUDE_NUTTX_MUTEX_H
#undef __INCLUDE_NUTTX_SEMAPHORE_H
#undef inline_function

#define static                /* blank: header inlines become globals */
#define inline_function       /* drop always_inline on emitted bodies */

#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
