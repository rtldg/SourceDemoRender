#include "game_priv.h"

// The audio backend need times to be aligned to 4 sample boundaries.
// The skipped samples are remembered for the next iteration.
s32 game_audio_v1_align_sample_time(s32 value)
{
    return value & ~3;
}

void game_audio_v1_init()
{
}

void game_audio_v1_free()
{
}

void game_audio_v1_mix_audio_for_one_frame(s32 num_samples_to_mix)
{
    s32 paint_time = game_get_snd_paint_time_0();
    s32 raw_end_time = paint_time + num_samples_to_mix + game_state.snd_skipped_samples;
    s32 aligned_end_time = game_audio_v1_align_sample_time(raw_end_time);
    s32 num_samples = aligned_end_time - paint_time;

    game_state.snd_skipped_samples = raw_end_time - aligned_end_time;
    game_state.snd_num_samples = num_samples;

    if (num_samples > 0)
    {
        game_state.snd_is_painting = true;
        game_snd_paint_chans_override_0(aligned_end_time, game_state.snd_listener_underwater);
        game_state.snd_is_painting = false;
    }
}

GameAudioDesc game_audio_v1_desc =
{
    .name = "AudioV1",
    .init = game_audio_v1_init,
    .free = game_audio_v1_free,
    .mix_audio_for_one_frame = game_audio_v1_mix_audio_for_one_frame,
};
