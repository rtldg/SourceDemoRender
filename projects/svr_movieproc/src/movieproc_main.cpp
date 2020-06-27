#include <svr/movproc.hpp>
#include <svr/os.hpp>
#include <svr/config.hpp>
#include <svr/log_format.hpp>
#include <svr/graphics.hpp>
#include <svr/graphics_preview.hpp>
#include <svr/ui.hpp>
#include <svr/thread_context.hpp>
#include <svr/defer.hpp>

#include <stdio.h>
#include <charconv>

static svr::os_handle* arg_to_handle(const char* arg)
{
    uintptr_t ret;
    std::from_chars(arg, arg + strlen(arg), ret, 16);

    return (svr::os_handle*)ret;
}

static svr::graphics_texture* create_work_texture(svr::graphics_backend* graphics, uint32_t width, uint32_t height)
{
    using namespace svr;

    // Must use 32 bits per channel so we get the required accuracy.

    graphics_texture_desc tex_desc = {};
    tex_desc.width = width;
    tex_desc.height = height;
    tex_desc.format = GRAPHICS_FORMAT_R32G32B32A32_FLOAT;
    tex_desc.usage = GRAPHICS_USAGE_DEFAULT;
    tex_desc.view_access = GRAPHICS_VIEW_SRV | GRAPHICS_VIEW_UAV | GRAPHICS_VIEW_RTV;
    tex_desc.caps = GRAPHICS_CAP_DOWNLOADABLE;
    tex_desc.text_target = true;

    return graphics->create_texture("work texture", tex_desc);
}

static svr::graphics_texture* create_buffer_texture(svr::graphics_backend* graphics, uint32_t width, uint32_t height)
{
    using namespace svr;

    graphics_texture_desc tex_desc = {};
    tex_desc.width = width;
    tex_desc.height = height;
    tex_desc.format = GRAPHICS_FORMAT_B8G8R8A8_UNORM;
    tex_desc.usage = GRAPHICS_USAGE_DEFAULT;
    tex_desc.view_access = GRAPHICS_VIEW_SRV;

    return graphics->create_texture("buffer texture", tex_desc);
}

#define MAX_TEXTURES 1

svr::graphics_texture* textures[MAX_TEXTURES];
size_t tex_i;

int main(int argc, char* argv[])
{
    using namespace svr;

    log_set_function([](void* context, const char* text)
    {
        printf(text);
    }, nullptr);

    auto start_event = os_open_event("crashfort.movproc.start");

    auto resource_path = argv[0];
    auto width = atoi(argv[1]);
    auto height = atoi(argv[2]);
    auto name = argv[3];
    auto profile = argv[4];
    auto tex_h = arg_to_handle(argv[5]);
    auto mmap_h = arg_to_handle(argv[6]);

    auto mmap = os_open_mmap(mmap_h, sizeof(movproc_mmap_data));

    defer {
        os_destroy_mmap(mmap);
    };

    auto mmap_data = (movproc_mmap_data*)os_view_mmap(mmap);
    mmap_data->game_rate = 3600;

    auto graphics = graphics_create_d3d11_backend(resource_path);

    defer {
        graphics_destroy_backend(graphics);
    };

    graphics_texture_open_desc desc = {};
    desc.view_access = GRAPHICS_VIEW_SRV;

    auto tex = graphics->open_shared_texture("game", tex_h, desc);
    auto tex_srv = graphics->get_texture_srv(tex);

    defer {
        graphics->destroy_texture(tex);
    };

    thread_context_event ui_thread;
    graphics_preview* prev = nullptr;

    synchro_barrier start_barrier;

    ui_thread.run_task([&]()
    {
        prev = graphics_preview_create_winapi(graphics, width, height, false);
        start_barrier.open();
        ui_enter_message_loop();
        graphics_preview_destroy(prev);
        prev = nullptr;
    });

    start_barrier.wait();

    auto server_event = os_open_event("crashfort.movproc.server");
    auto break_event = os_open_event("crashfort.movproc.break");
    auto client_event = os_create_event("crashfort.movproc.client");

    defer {
        os_close_handle(start_event);
        os_close_handle(server_event);
        os_close_handle(break_event);
        os_close_handle(client_event);
    };

    for (size_t i = 0; i < MAX_TEXTURES; i++)
    {
        auto buf_tex = create_buffer_texture(graphics, width, height);

        if (buf_tex == nullptr)
        {
            return 1;
        }

        textures[i] = buf_tex;
    }

    defer {
        for (size_t i = 0; i < MAX_TEXTURES; i++) graphics->destroy_texture(textures[i]);
    };

    os_set_event(start_event);

    while (true)
    {
        os_reset_event(client_event);

        auto waited = os_handle_wait_either(server_event, break_event, -1);

        if (waited == break_event)
        {
            break;
        }

        auto buf_tex = textures[tex_i];
        auto buf_tex_srv = graphics->get_texture_srv(buf_tex);

        tex_i++;

        if (tex_i == MAX_TEXTURES)
        {
            tex_i = 0;
        }

        graphics->copy_texture(tex, buf_tex);

        os_set_event(client_event);
    }

    ui_exit_message_loop(ui_thread.get_thread_id());
    return 0;
}
