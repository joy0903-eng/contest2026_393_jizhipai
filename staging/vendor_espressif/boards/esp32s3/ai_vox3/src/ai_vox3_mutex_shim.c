/****************************************************************************
 * src/ai_vox3_mutex_shim.c
 *
 * Link-time provider for the nxmutex_* family (shim v5).
 *
 * Background (run#31..#34): libdrivers.a(es8311.o) emits undefined
 * references to nxmutex_init, nxmutex_lock and nxmutex_unlock, and
 * nothing in the link resolves them.  This file emits those symbols as
 * real global definitions inside libboard.a; libboard is linked after
 * libdrivers in NUTTXLIBS (tools/FlatLibs.mk), so these members resolve
 * es8311.o's references (the undefined refs drive the archive pull).
 *
 * History of failed attempts, all reproduced locally with the same
 * xtensa-esp32s3-elf-gcc against the same header tree:
 *
 *   v1 (run#33): blank 'static' and 'inline_function' FIRST, then
 *   include mutex.h and semaphore.h.  compiler.h entered afterwards
 *   through assert.h and re-defined 'inline_function' back to
 *   always_inline inline (GCC warning "inline_function" redefined).
 *   Every body expanded as a C99 static inline, which emits NO external
 *   symbol: the object compiled clean yet carried zero nxmutex symbols.
 *
 *   v4 (run#34): include compiler.h FIRST to pre-seed its guard.  That
 *   invents an include-tree shape no normal Nuttx file ever produces and
 *   broke the stdint.h chain on CI ('_int8_t' undeclared).  Also, a
 *   comment contained the sequence star-slash and truncated the block
 *   comment early.  Two independent compile kills.
 *
 *   Official re-inclusion trick (libs/libc/misc/lib_sem_mutex_noinline.c):
 *   include normally, then undef guards and re-include with 'static'
 *   blanked.  Only valid under NOINLINE=y, where the first pass yields
 *   extern declarations and the second pass definitions.  Under
 *   NOINLINE=n the first pass already yields full static inline bodies,
 *   so the second pass collides with dozens of redefinitions.
 *
 * How v5 works (nm-verified: T nxmutex_init, T nxmutex_lock,
 * T nxmutex_unlock, plus the full nxmutex_ and nxrmutex_ families):
 *   1. Include <assert.h> through the STANDARD path first.  This is the
 *      exact include shape every normal Nuttx TU has
 *      (mutex.h pulls assert.h, assert.h pulls compiler.h), so the
 *      stdint/types chain stays canonical.  It also establishes
 *      compiler.h's include guard.
 *   2. Undef 'inline_function' (drop always_inline), then define it
 *      empty, and blank 'static'.
 *   3. Include mutex.h and semaphore.h ONCE.  assert.h and compiler.h
 *      are now blocked by their guards, so the empty 'inline_function'
 *      and blank 'static' survive into every body: each static inline
 *      expands as a plain global definition in this translation unit.
 *      Single pass, no re-inclusion, no redefinition conflicts.
 *
 * Safety against duplicate symbols:
 *  - With CONFIG_LIBC_SEM_MUTEX_NOINLINE pinned off (defconfig plus the
 *    audit gate), libc.a's lib_sem_mutex_noinline.o compiles EMPTY and
 *    defines nothing; sched.a defines only the nxsem_* primitives and
 *    the _slow functions, none of which collide with the nxmutex_ and
 *    nxrmutex_ globals emitted here.
 *  - The emitted symbol set matches what the official
 *    lib_sem_mutex_noinline.c produces under NOINLINE=y (verified side
 *    by side with nm), so the link symbol set equals a supported Nuttx
 *    configuration.
 *
 * The build.yml CONFIG audit gate fails the build if the NOINLINE symbol
 * ever flips back on, which keeps this file conflict-free.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* Establish the compiler layer through the STANDARD include path first
 * (see step 1 above). */

#include <assert.h>

#undef inline_function

#define static           /* blank: header inlines become globals */
#define inline_function  /* drop always_inline on emitted bodies */

#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
