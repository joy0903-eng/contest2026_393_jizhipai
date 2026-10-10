/****************************************************************************
 * apps/examples/desktop_companion/novel_reader.h
 *
 * Audiobook ("read the novel") feature: load a novel text file from the
 * on-board FFat, split it into speakable segments (by sentence / line), and
 * walk through them one at a time.
 *
 * This module is PURE LOGIC. It knows nothing about the network, LVGL or the
 * audio pipeline -- main.c drives it and feeds each segment's text to the PC
 * bridge /tts endpoint, then plays the returned PCM through audio_pipeline.
 *
 * Splitting policy
 * ----------------
 * The text is cut into segments at sentence terminators (。！？!?…；;) and
 * newlines, so each segment is one breath for the TTS engine and one natural
 * pause/resume/next unit. Chapter headings of the form 第N章 / 第N回 / 第N卷
 * are detected to drive the "chapter / line" readout on the UI.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_NOVEL_READER_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_NOVEL_READER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: novel_reader_open
 *
 * Description:
 *   Open the novel text at `path` (NULL/"" uses the build-time default
 *   AIVOX3_READ_NOVEL_PATH). The file is read entirely into a heap buffer,
 *   split into segments and indexed.
 *
 *   If the file does not exist, the built-in sample (AIVOX3_BUILTIN_NOVEL)
 *   is written to `path` (best effort -- requires a mounted, writable FFat)
 *   and then read back; if even that fails, the sample is used in memory so
 *   the reading demo always has something to read.
 *
 * Returned Value:
 *   OK (0) on success, negated errno otherwise (-ENOENT when the text could
 *   not be loaded at all).
 ****************************************************************************/

int novel_reader_open(const char *path);

/****************************************************************************
 * Name: novel_reader_close
 *
 * Description:
 *   Release all resources allocated by novel_reader_open(). Safe to call when
 *   no novel is open; re-openable afterwards.
 ****************************************************************************/

int novel_reader_close(void);

/****************************************************************************
 * Name: novel_reader_current
 *
 * Description:
 *   Copy the current segment's text into `out` (NUL-terminated, clipped to
 *   cap-1). `out_len` receives the true segment length.
 *
 * Returned Value:
 *   OK, or -ENOENT when there is no current segment (end of book).
 ****************************************************************************/

int novel_reader_current(char *out, size_t cap, size_t *out_len);

/****************************************************************************
 * Name: novel_reader_next / novel_reader_prev
 *
 * Description:
 *   Advance / rewind the segment cursor by one.
 *
 * Returned Value:
 *   OK, or -ENOENT when already at the start / end (cursor unchanged).
 ****************************************************************************/

int novel_reader_next(void);
int novel_reader_prev(void);

/****************************************************************************
 * Name: novel_reader_chapter / novel_reader_line
 *
 * Description:
 *   Chapter index (1-based) and line-within-chapter (1-based) of the current
 *   segment. Both default to 1 / 0 when no segment is current.
 ****************************************************************************/

int novel_reader_chapter(void);
int novel_reader_line(void);

/****************************************************************************
 * Name: novel_reader_total
 *
 * Description:
 *   Total number of segments in the loaded novel (0 when none loaded).
 ****************************************************************************/

int novel_reader_total(void);

/****************************************************************************
 * Name: novel_reader_progress
 *
 * Description:
 *   Playback progress as a percentage (0..100) of the current segment index
 *   over the total. 0 when nothing is loaded.
 ****************************************************************************/

int novel_reader_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_NOVEL_READER_H */
