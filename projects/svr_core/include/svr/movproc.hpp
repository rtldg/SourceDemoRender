#pragma once
#include <svr/vec.hpp>

#include <stdint.h>

namespace svr
{
    struct os_handle;

    struct movproc_mmap_data
    {
        bool use_preview_window;
        bool use_velocity_overlay;
        bool use_motion_blur;

        uint32_t game_rate;

        vec3 velocity;
    };
}
