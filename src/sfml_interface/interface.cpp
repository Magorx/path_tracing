#include "interface.h"


SFML_Interface::SFML_Interface(const char *window_name, Scene *scene_, const conf_PathTracer config_, int scr_w_, int scr_h_, int pixel_sampling_per_render_):
window(sf::VideoMode(scr_w_, scr_h_), window_name),
image_texture(),
image_sprite(),

scene(scene_),
config(config_),

scr_w(scr_w_),
scr_h(scr_h_),

pixel_cnt(0),
pixel_sampling_per_render(pixel_sampling_per_render_),

cur_image(nullptr),

frame(),
new_frame(),

consecutive_frames_cnt(0),

render_threader(config_.sysinf.kernel_cnt, render_threaded),

average_frame_ms(0),
average_frame_cnt(0)
{   
    if (!scene) {
        fprintf(stderr, "[ERR] scene is nullptr, aborting");
    }

    image_texture.create(scene->camera->res_w, scene->camera->res_h);
    sf::Vector2u img_size = image_texture.getSize();
    pixel_cnt = img_size.x * img_size.y;

    cur_image = (RGBA*) calloc(pixel_cnt, sizeof(RGBA));
    if (!cur_image) {
        fprintf(stderr, "[ERR] Can't calloc buffer for current/new image [%dx%d]", img_size.x, img_size.y);
    }

    frame = Frame<Color, Vec3d, double>(img_size.x, img_size.y);
    new_frame = Frame<Color, Vec3d, double>(img_size.x, img_size.y);

    image_sprite.setTexture(image_texture);
    image_sprite.setScale((double) scr_w / img_size.x, (double) scr_h / img_size.y);

// ============

    int lines_per_thread = frame.size_y / render_threader.get_threads_cnt();

    std::vector<ThreadRenderTask> rtasks;
    for (size_t i = 0; i < render_threader.get_threads_cnt() - 1; ++i) {
        int min_x = 0;
        int max_x = frame.size_x;
        int min_y = i * lines_per_thread;
        int max_y = min_y + lines_per_thread;
        render_threader.add_task(ThreadRenderTask(*scene, config, {min_x, max_x, min_y, max_y, static_cast<int>(i)}, new_frame));
    }

    int min_x = 0;
    int max_x = frame.size_x;
    int min_y = (render_threader.get_threads_cnt() - 1) * lines_per_thread;
    int max_y = frame.size_y;
    render_threader.add_task(ThreadRenderTask(*scene, config, {min_x, max_x, min_y, max_y, static_cast<int>(10)}, new_frame));
}

void SFML_Interface::render_frame_threaded() {
    Timer timer;

    render_threader.perform();
    timer.stop();

    average_frame_ms += timer.elapsed;
    average_frame_cnt++;

    logger.log("TMR", "timer", "frame[%d] %lldms | mean = %lldms", consecutive_frames_cnt, timer.elapsed, average_frame_ms / average_frame_cnt);
}

void SFML_Interface::render_depth_buffer() {
    new_frame.clear();
    config.render.PIXEL_SAMPLING = 1;
    render_frame_threaded();
}

void SFML_Interface::render_frame_portion() {
    new_frame.clear();
    config.render.PIXEL_SAMPLING = pixel_sampling_per_render;
    render_frame_threaded();

    new_frame.set_post_processing(rendered_frame_postproc);
    new_frame.postproc(rendered_frame_postproc_radius);
}

void SFML_Interface::accumulate_frame_portion() {
    memcpy(frame.data_normal, new_frame.data_normal, frame.pixel_cnt * sizeof(Vec3d));
    memcpy(frame.data_depth, new_frame.data_depth, frame.pixel_cnt * sizeof(double));

    if (!consecutive_frames_cnt) {
        memcpy(frame.data_color, new_frame.data_color, pixel_cnt * sizeof(Color));
    } else {
        double n = consecutive_frames_cnt + 1;
        for (int i = 0; i < pixel_cnt; ++i) {
            frame.data_color[i] = frame.final_image[i] * ((n - 1.0) / n) + new_frame.final_image[i] / n;
        }
    }

    frame.set_post_processing(accumulator_frame_postproc);
    frame.postproc(accumulator_frame_postproc_radius);
    color_to_rgb_buffer(frame.final_image, cur_image, config.render.GAMMA_CORRECTION, pixel_cnt);

    ++consecutive_frames_cnt;
}

void SFML_Interface::flush_to_texture() {
    image_texture.update((sf::Uint8*) cur_image);
}

void SFML_Interface::flush_to_window() {
    window.draw(image_sprite);
}

void SFML_Interface::tick() {
    window.clear();

    flush_to_window();
}

void SFML_Interface::run() {
    is_run = true;

    while (is_run)
    {
        if (is_moving) {
            consecutive_frames_cnt = 0;
            render_depth_buffer();
        } else {
            render_frame_portion();
        }

        accumulate_frame_portion();

        render_done = true;
    }
}

void SFML_Interface::stop() {
    render_threader.join();
    is_run = false;
}

void SFML_Interface::screenshot_to_file(const char *filename) {
    if (!filename) {
        printf("[ERR] can't screenshot to nullptr filename, will save to SCRSHT.png");
        filename = "SCRSHT.png";
    }

    image_texture.copyToImage().saveToFile(filename);
}

void SFML_Interface::handle_events() {
    sf::Event event;
    while (window.pollEvent(event))
    {
        if (event.type == sf::Event::Closed) {
            is_run = false;
            window.close();
        }
        
        if (event.type == sf::Event::KeyPressed) {
            if (event.key.code == sf::Keyboard::F) {
                auto filename = strdup(("scrsht_" + std::to_string(config.sysinf.timestamp) + "_" + std::to_string(consecutive_frames_cnt) + ".png").c_str());
                screenshot_to_file(filename);

                logger.logr("SCR", "sfml_interface", "screenshot (%s) saved", filename);
                free(filename);
            }
        }
    }
}

void SFML_Interface::handle_movement() {
    static FramePostproc preserved_frame_postproc;

    bool moved = false;

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::W)) {
        scene->camera->orig += scene->camera->dir;
        moved = true;
    }
    
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::S)) {
        scene->camera->orig -= scene->camera->dir;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::A)) {
        scene->camera->orig -= scene->camera->ort_w;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::D)) {
        scene->camera->orig += scene->camera->ort_w;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Space)) {
        scene->camera->orig += {0, 0, 1};
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::C)) {
        scene->camera->orig += {0, 0, -1};
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Left)) {
        scene->camera->z_ang += +Pi/60;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Right)) {
        scene->camera->z_ang += -Pi/60;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Up)) {
        scene->camera->y_ang += -Pi/60;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Down)) {
        scene->camera->y_ang += +Pi/60;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Q)) {
        scene->camera->orig += scene->camera->ort_h;
        moved = true;
    }

    if (sf::Keyboard::isKeyPressed(sf::Keyboard::E)) {
        scene->camera->orig -= scene->camera->ort_h;
        moved = true;
    }

    if (moved) {
        scene->camera->update();

        if (!is_moving) {
            preserved_frame_postproc = frame.postproc_id;
            accumulator_frame_postproc = FramePostproc::depth;
        }
    } else {
        if (is_moving) {
            accumulator_frame_postproc = preserved_frame_postproc;
        }
    }

    is_moving = moved;
    // printf("%s\n", is_moving ? "mov" : "not move");
}

void SFML_Interface::interaction_loop() {
    while (is_run) {
        handle_events();
        handle_movement();

        using namespace std::chrono_literals;
        std::this_thread::sleep_for(50ms);

        if (render_done) {
            window.clear();
            flush_to_texture();
            flush_to_window();
            window.display();
            render_done = false;
        }
    }
}
