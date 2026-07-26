#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize esp-tts Chinese voice synthesis engine.
 *
 * Loads the Chinese voice model (xiaoxin) from the default model partition.
 * Must be called after audio_player_init() so I2S1 is ready.
 *
 * @return true  on success.
 * @return false if voice model loading fails.
 */
bool tts_player_init(void);

/**
 * @brief Synthesize Chinese text to speech and play through MAX98357A.
 *
 * This function sends the text to a dedicated TTS task queue and returns immediately.
 * The TTS task will:
 *   1. Pauses ESP-SR microphone capture (feed task) to avoid feedback.
 *   2. Synthesizes PCM via esp-tts in a streaming loop.
 *   3. Routes each PCM chunk to I2S1 (MAX98357A) via audio_play_pcm().
 *   4. Unmutes the speaker, plays, then re-mutes after playback.
 *   5. Resumes ESP-SR microphone capture.
 *
 * The call is NON-BLOCKING — it returns immediately after queuing the request.
 * Use tts_is_playing() to check if playback is in progress.
 *
 * @param text  Null-terminated UTF-8 Chinese text to speak (max 127 chars).
 */
void tts_play_text(const char *text);

/**
 * @brief Check whether TTS is currently playing (non-blocking poll).
 *
 * @return true if TTS playback is in progress.
 */
bool tts_is_playing(void);

/**
 * @brief Deinitialize the TTS engine and free resources.
 */
void tts_player_deinit(void);

#ifdef __cplusplus
}
#endif
