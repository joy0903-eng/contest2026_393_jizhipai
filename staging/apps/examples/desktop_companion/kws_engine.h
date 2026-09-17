/****************************************************************************
 * apps/examples/desktop_companion/kws_engine.h
 *
 * Offline keyword spotting engine: DTW template matching on the CMN Mel
 * features produced by dsp_mfcc.
 *
 * Ported 1:1 (numerically) from the Arduino reference firmware
 * (voice_chat_src/keyword_engine.h) that was validated on real hardware:
 * wake word "ni hao, openvela", 3 takes per word, banded DTW
 * (Sakoe-Chiba +-KWS_DTW_BAND frames), adaptive threshold
 * "max intra-class distance * 1.35", empirically 2400.
 *
 * Porting notes (vs. Arduino):
 *   - FFat/File -> POSIX open/read/write/close; the path is a parameter.
 *   - heap_caps_malloc(MALLOC_CAP_SPIRAM) -> malloc().
 *   - Serial.printf -> syslog().
 *   - Boolean returns became OK (0) / negated errno, except kws_classify()
 *     and kws_recognize() which keep the "-1 = rejected" convention.
 *   - On-disk format keeps the Arduino "KWS1" magic; version 2 additionally
 *     stores the word names. Both versions can be read.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_KWS_ENGINE_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_KWS_ENGINE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "dsp_mfcc.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Engine parameters (mirrors Arduino config.h)
 ****************************************************************************/

#define KWS_WORDS_MAX      8       /* template bank slots                    */
#define KWS_TPL_PER_WORD   3       /* takes per word used for calibration    */
#define KWS_THRESH_MIN     40.0f   /* never accept a silly-small threshold   */
#define KWS_INTRA_SCALE    1.35f   /* adaptive threshold factor              */
#define KWS_NAME_MAX       16      /* bytes of a word name, incl. NUL        */
#define KWS_MIN_TPL_FRAMES 10      /* shortest usable template               */

#ifndef CONFIG_AIVOX3_KWS_STORE_PATH
#define KWS_STORE_PATH_DEFAULT "/mnt/sd0/wake.bin"
#else
#define KWS_STORE_PATH_DEFAULT CONFIG_AIVOX3_KWS_STORE_PATH
#endif

#ifndef CONFIG_AIVOX3_KWS_THRESHOLD
#define KWS_THRESH_DEFAULT 0.0f
#else
#define KWS_THRESH_DEFAULT ((float)CONFIG_AIVOX3_KWS_THRESHOLD)
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct kws_template_s
{
  int16_t *data;                   /* frames * FEAT_DIM int16 features      */
  int      frames;                 /* frame count of this template          */
};

typedef struct kws_template_s kws_template_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: kws_init
 *
 * Description:
 *   Reset the (empty) template bank and apply the build-time default
 *   threshold. Safe to call again; it does NOT free templates, use
 *   kws_wipe() first if that is wanted.
 *
 * Returned Value:
 *   OK (0).
 *
 ****************************************************************************/

int kws_init(void);

/****************************************************************************
 * Name: kws_deinit
 *
 * Description:
 *   Free every template and reset the engine.
 *
 ****************************************************************************/

void kws_deinit(void);

/****************************************************************************
 * Name: kws_register_word
 *
 * Description:
 *   Register a command word and return its slot index.
 *
 * Returned Value:
 *   Slot index >= 0 on success; -EINVAL for a bad name; -ENOSPC when the
 *   bank is full.
 *
 ****************************************************************************/

int kws_register_word(const char *name);

/****************************************************************************
 * Name: kws_word_count / kws_word_name
 *
 * Description:
 *   Number of registered words and their (read-only) names.
 *
 ****************************************************************************/

int kws_word_count(void);
const char *kws_word_name(int word_idx);

/****************************************************************************
 * Name: kws_add_template
 *
 * Description:
 *   Deep-copy a pre-computed feature matrix into the bank.
 *
 * Input Parameters:
 *   word_idx - slot returned by kws_register_word().
 *   feat     - frames * FEAT_DIM int16 feature matrix.
 *   frames   - frame count, [KWS_MIN_TPL_FRAMES, KWS_MAX_FRAMES].
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL, -ENOSPC or -ENOMEM on failure.
 *
 ****************************************************************************/

int kws_add_template(int word_idx, const int16_t *feat, int frames);

/****************************************************************************
 * Name: kws_learn
 *
 * Description:
 *   Convenience "teach me one take": run the front-end on a raw utterance
 *   and store the resulting feature matrix as a template. When the word
 *   reaches KWS_TPL_PER_WORD takes the threshold is calibrated
 *   automatically (kws_finalize_training()).
 *
 * Input Parameters:
 *   word_idx - slot returned by kws_register_word().
 *   pcm      - raw mono 16 kHz int16 utterance.
 *   nsamples - number of samples.
 *
 * Returned Value:
 *   Number of takes stored for this word (>= 1) on success;
 *   negated errno on failure.
 *
 ****************************************************************************/

int kws_learn(int word_idx, const int16_t *pcm, int nsamples);

/****************************************************************************
 * Name: kws_finalize_training
 *
 * Description:
 *   Calibrate the decision threshold from the intra-class DTW spread and
 *   log a separability diagnostic.
 *
 * Returned Value:
 *   OK (0) when the classes look separable, -ERANGE when the intra-class
 *   spread is too large or a word has fewer than 2 takes (the threshold is
 *   nevertheless stored, and the engine is left not-ready).
 *
 ****************************************************************************/

int kws_finalize_training(void);

/****************************************************************************
 * Name: kws_classify
 *
 * Description:
 *   Match a pre-computed feature matrix against every template.
 *
 * Input Parameters:
 *   feat      - frames * FEAT_DIM int16 feature matrix.
 *   frames    - frame count.
 *   score_out - optional; receives the best (smallest) normalized DTW
 *               distance.
 *
 * Returned Value:
 *   Matched word index (>= 0) when the best distance is within the
 *   threshold; -1 when rejected or when the engine is not ready.
 *
 ****************************************************************************/

int kws_classify(const int16_t *feat, int frames, float *score_out);

/****************************************************************************
 * Name: kws_recognize
 *
 * Description:
 *   kws_classify() on a raw utterance: runs dsp_mfcc_compute() first.
 *
 * Returned Value:
 *   Matched word index (>= 0), or -1 when rejected / not ready /
 *   the utterance was too short. *score_out is set to -1.0f if feature
 *   extraction failed and to the best distance otherwise.
 *
 ****************************************************************************/

int kws_recognize(const int16_t *pcm, int nsamples, float *score_out);

/****************************************************************************
 * Name: kws_wipe
 *
 * Description:
 *   Free every template, clear the counters and restore the default
 *   threshold. Does not touch the file on disk.
 *
 ****************************************************************************/

void kws_wipe(void);

/****************************************************************************
 * Name: kws_save / kws_load
 *
 * Description:
 *   Persist / restore the template bank through POSIX file I/O.
 *
 * Input Parameters:
 *   path - file path. NULL selects KWS_STORE_PATH_DEFAULT.
 *
 * Returned Value:
 *   OK (0) on success; negated errno on failure (-ENOENT when no writable
 *   filesystem is mounted at that path, -EINVAL for a bad/foreign file).
 *
 ****************************************************************************/

int kws_save(const char *path);
int kws_load(const char *path);

/****************************************************************************
 * Name: kws_set_threshold / kws_get_threshold
 *
 * Description:
 *   Override / read the decision threshold. Values <= 5 are ignored by
 *   kws_set_threshold() (guards against an uncalibrated 0).
 *
 ****************************************************************************/

void  kws_set_threshold(float threshold);
float kws_get_threshold(void);

/****************************************************************************
 * Name: kws_is_ready
 *
 * Description:
 *   Returns 1 when the engine has templates and a usable threshold.
 *
 ****************************************************************************/

int kws_is_ready(void);

/****************************************************************************
 * Name: kws_force_ready
 *
 * Description:
 *   Bypass the separability check and go live with the current templates.
 *   If threshold > 5 it replaces the current threshold.
 *
 ****************************************************************************/

void kws_force_ready(float threshold);

/****************************************************************************
 * Name: kws_total_templates
 *
 * Description:
 *   Total number of templates currently stored.
 *
 ****************************************************************************/

int kws_total_templates(void);

/****************************************************************************
 * Name: kws_first_incomplete
 *
 * Description:
 *   Report the first word that still misses takes, so a caller can resume
 *   an interrupted training session.
 *
 * Input Parameters:
 *   word - receives the word index (0 when everything is complete).
 *   take - receives how many takes that word already has.
 *
 ****************************************************************************/

void kws_first_incomplete(int *word, int *take);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_KWS_ENGINE_H */
