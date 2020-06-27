#include "game_system.hpp"

#include <svr/movproc.hpp>
#include <svr/os.hpp>
#include <svr/graphics.hpp>
#include <svr/log_format.hpp>
#include <svr/format.hpp>
#include <svr/str.hpp>
#include <svr/config.hpp>
#include <svr/defer.hpp>

#include <charconv>

struct game_system
{
    const char* resource_path;

    bool movie_running = false;
    uint32_t width;
    uint32_t height;

    svr::graphics_backend* graphics = nullptr;
    svr::os_handle* game_tex_handle = nullptr;

    svr::os_handle* start_event = nullptr;
    svr::os_handle* server_event = nullptr;
    svr::os_handle* break_event = nullptr;
    svr::os_handle* client_event = nullptr;

    svr::os_mmap* mmap = nullptr;
    svr::movproc_mmap_data* mmap_data = nullptr;

    svr::os_handle* mov_proc = nullptr;
    svr::os_handle* mov_thread = nullptr;

    bool support_velocity_overlay = false;
};

static svr::str_builder build_movproc_args(game_system* sys, const char* name, const char* profile)
{
    using namespace svr;

    str_builder ret;
    fmt::memory_buffer buf;

    ret.append("\"");
    ret.append(sys->resource_path);
    ret.append("\"");
    ret.append(" ");

    format_with_null(buf, "{}", sys->width);
    ret.append(buf.data());
    ret.append(" ");

    format_with_null(buf, "{}", sys->height);
    ret.append(buf.data());
    ret.append(" ");

    ret.append("\"");
    ret.append(name);
    ret.append("\"");
    ret.append(" ");

    ret.append("\"");
    ret.append(profile ? profile : "default");
    ret.append("\"");
    ret.append(" ");

    format_with_null(buf, "{:x}", (uintptr_t)sys->game_tex_handle);
    ret.append(buf.data());
    ret.append(" ");

    format_with_null(buf, "{:x}", (uintptr_t)os_get_mmap_handle(sys->mmap));
    ret.append(buf.data());

    return ret;
}

game_system* sys_create(const char* resource_path, svr::graphics_backend* graphics)
{
    auto ret = new game_system;
    ret->resource_path = resource_path;
    ret->graphics = graphics;

    return ret;
}

void sys_destroy(game_system* sys)
{
    using namespace svr;

    graphics_destroy_backend(sys->graphics);
    if (sys->start_event) os_close_handle(sys->start_event);
    if (sys->server_event) os_close_handle(sys->server_event);
    if (sys->client_event) os_close_handle(sys->client_event);
    if (sys->break_event) os_close_handle(sys->break_event);
    if (sys->mmap) os_destroy_mmap(sys->mmap);
    if (sys->mov_proc) os_close_handle(sys->mov_proc);
    if (sys->mov_thread) os_close_handle(sys->mov_thread);
}

uint32_t sys_get_game_rate(game_system* sys)
{
    return sys->mmap_data->game_rate;
}

void sys_open_shared_game_texture(game_system* sys, svr::os_handle* ptr)
{
    sys->game_tex_handle = ptr;
    svr::log("Opened shared game texture\n");
}

void sys_new_frame(game_system* sys)
{
    using namespace svr;

    // When we are entered here, the active arch has already copied over the game content to a shared texture.
    // The shared texture handle is passed to the movie process and then opened there.
    // We set this event to notify that it is ok now to read and copy it.

    os_set_event(sys->server_event);

    // It's possible the game could lock up here in case we only waited for the client event.
    // If the movie process ends too early or crashes, we have check for that too.

    auto waited = os_handle_wait_either(sys->client_event, sys->mov_proc, -1);

    if (waited == sys->mov_proc)
    {
        log("Movie Process {} has ended too early\n", os_get_proc_id(sys->mov_proc));
    }

    // Always reset the server event last here, because we don't know if there will be coming more frames.
    // If this is not here, the movie process would consume 100% cpu, as the server event would always be signalled.
    os_reset_event(sys->server_event);
}

bool sys_movie_running(game_system* sys)
{
    return sys->movie_running;
}

void sys_set_velocity_overlay_support(game_system* sys, bool value)
{
    sys->support_velocity_overlay = value;
}

bool sys_use_velocity_overlay(game_system* sys)
{
    return sys->mmap_data->use_velocity_overlay;
}

void sys_provide_velocity_overlay(game_system* sys, svr::vec3 v)
{
    sys->mmap_data->velocity = v;
}

bool sys_start_movie(game_system* sys, const char* name, const char* profile, uint32_t width, uint32_t height)
{
    using namespace svr;

    if (sys->movie_running)
    {
        return false;
    }

    sys->width = width;
    sys->height = height;

    sys->mmap = os_create_mmap(sizeof(movproc_mmap_data));

    if (sys->mmap == nullptr)
    {
        log("Could not create movproc mmap\n");
        return false;
    }

    sys->mmap_data = (movproc_mmap_data*)os_view_mmap(sys->mmap);
    memset(sys->mmap_data, 0, sizeof(movproc_mmap_data));

    sys->start_event = os_create_event("crashfort.movproc.start");
    sys->server_event = os_create_event("crashfort.movproc.server");
    sys->break_event = os_create_event("crashfort.movproc.break");

    auto args = build_movproc_args(sys, name, profile);

    os_start_proc_desc proc_desc = {};
    proc_desc.suspended = true;

    str_builder exe_builder;
    exe_builder.append(sys->resource_path);
    exe_builder.append("svr_movieproc.exe");

    if (!os_start_proc(exe_builder.buf, sys->resource_path, args.buf, &proc_desc, &sys->mov_proc, &sys->mov_thread))
    {
        log("Could not create movproc\n");
        return false;
    }

    os_resume_thread(sys->mov_thread);

    os_handle_wait(sys->start_event, -1);

    os_close_handle(sys->start_event);
    sys->start_event = nullptr;

    sys->client_event = os_open_event("crashfort.movproc.client");

    log("Starting movie to '{}'\n", name);

    sys->movie_running = true;

    return true;
}

void sys_end_movie(game_system* sys)
{
    using namespace svr;

    if (!sys->movie_running)
    {
        return;
    }

    log("Ending movie, waiting for movie process\n");

    os_reset_event(sys->server_event);
    os_set_event(sys->break_event);
    os_handle_wait(sys->mov_proc, -1);

    os_close_handle(sys->mov_thread);
    sys->mov_thread = nullptr;

    os_close_handle(sys->mov_proc);
    sys->mov_proc = nullptr;

    os_close_handle(sys->break_event);
    sys->break_event = nullptr;

    os_close_handle(sys->server_event);
    sys->server_event = nullptr;

    os_close_handle(sys->client_event);
    sys->client_event = nullptr;

    sys->movie_running = false;

    os_destroy_mmap(sys->mmap);
    sys->mmap = nullptr;
    sys->mmap_data = nullptr;

    log("Ended movie\n");
}
