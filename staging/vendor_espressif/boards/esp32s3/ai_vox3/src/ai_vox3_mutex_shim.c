/****************************************************************************
 * src/ai_vox3_mutex_shim.c
 *
 * Link-time provider for the nxmutex_* family (shim v4).
 *
 * Background (run#31..#33): libdrivers.a(es8311.o) emits undefined
 * references to nxmutex_init/nxmutex_lock/nxmutex_unlock, and nothing in
 * the link resolves them.  This file emits those symbols as real global
 * definitions inside libboard.a; libboard is linked after libdrivers in
 * NUTTXLIBS (tools/FlatLibs.mk), so these members resolve es8311.o's
 * references (the undefined refs drive the archive pull).
 *
 * Why the previous attempt (v1, run#33) failed -- proven locally with the
 * same xtensa-esp32s3-elf-gcc against the same header tree:
 *   v1 did a plain include of mutex.h/semaphore.h first, then undef'd the
 *   include guards and re-included with 'static' and 'inline_function'
 *   blanked.  But compiler.h had already entered through assert.h and
 *   re-defined 'inline_function' back to
 *     __attribute__((always_inline)) inline
 *   (GCC warning: "inline_function" redefined ... previous definition).
 *   The function bodies therefore expanded as C99 static inline
 *   definitions, which emit NO external symbols -- shim.o was compiled
 *   yet carried no nxmutex_* symbols at all (nm-verified).
 *
 * How v4 works (nm-verified: T nxmutex_init/nxmutex_lock/nxmutex_unlock):
 *   1. Include <nuttx/compiler.h> FIRST so its include guard is
 *      established; it can then never re-define inline_function again.
 *   2. #undef inline_function (drop always_inline) and #define it empty,
 *      plus blank 'static'.
 *   3. Include mutex.h/semaphore.h ONCE: every static inline_function
 *      body expands as a plain global definition in this translation
 *      unit.  Single-pass, no guard undefs, no redefinition conflicts.
 *
 * Safety against duplicate symbols:
 *  - With CONFIG_LIBC_SEM_MUTEX_NOINLINE pinned off (defconfig + audit
 *    gate), libc.a's lib_sem_mutex_noinline.o compiles EMPTY and defines
 *    nothing; sched.a defines only the nxsem_* primitives and the *_slow
 *    functions, none of which collide with the nxmutex_*/nxrmutex_*
 *    globals emitted here.
 *  - The emitted symbol set is identical to what the official
 *    lib_sem_mutex_noinline.c produces under NOINLINE=y (verified side
 *    by side with nm), so the link symbol set matches a supported
 *    Nuttx configuration.
 *
 * The build.yml CONFIG audit gate fails the build if the NOINLINE symbol
 * ever flips back on, which keeps this file conflict-free.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* Bring in the compiler layer FIRST so its include guard is established
 * and it can never re-define inline_function after we blank it. */

#include <nuttx/compiler.h>

#undef inline_function

#define static           /* blank: header inlines become globals */
#define inline_function  /* drop always_inline on emitted bodies */

#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
