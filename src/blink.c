#include "engine.h"

#include "wren.h"

static void wren_write(WrenVM *vm, const char *text) {
    printf("%s", text);
}

static void wren_error(WrenVM *vm, WrenErrorType type, const char *module, int line, const char *message) {
    switch (type) {
    case WREN_ERROR_COMPILE:
        printf("[%s line %d] %s\n", module, line, message);
        break;
    case WREN_ERROR_RUNTIME:
        printf("%s\n", message);
        break;
    case WREN_ERROR_STACK_TRACE:
        printf("[%s line %d] in %s\n", module, line, message);
        break;
    }
}

int main() {
    Engine *engine = engine_create(200, 200, "Blink", ENGINE_SCALE3X | ENGINE_CONSOLE | ENGINE_RESIZABLE);

    Image *cat = engine_load_image_file("assets/cat.png");
    Sound *jump = engine_load_sound_file("assets/jump.wav");
    Sound *song = engine_load_sound_file("assets/song.ogg");

    char *source = engine_read_file("assets/hello.wren", NULL);

    WrenConfiguration config;
    wrenInitConfiguration(&config);

    config.writeFn = wren_write;
    config.errorFn = wren_error;

    WrenVM *vm = wrenNewVM(&config);
    wrenInterpret(vm, "hello", source);

    free(source);
    wrenFreeVM(vm);

    engine_play_music(song, 3.0f);

    double dt;
    while (engine_update(engine, &dt)) {
        engine_clear(engine, engine_rgb(255, 255, 255));
        engine_draw_point(engine, 50, 50, engine_rgb(0, 0, 255));
        engine_draw_rect(engine, engine_rect(60, 60, 25, 25), engine_rgb(255, 0, 0));
        engine_draw_line(engine, 10, 50, 50, 150, engine_rgb(255, 0, 0));
        engine_draw_image2(engine, cat, 100, 100, engine_rect(0, 0, 48, 48), ENGINE_WHITE);
        engine_draw_text(engine, "Hello blink!", 10, 10, ENGINE_BLACK);

        engine_draw_circle_fill(engine, 50, 50, 20, engine_rgb(0, 255, 255));

        if (engine_key_pressed(engine, ' ')) {
            engine_play_sound(jump);
        }
    }

    engine_destroy_image(cat);
    engine_destroy_sound(jump);
    engine_destroy_sound(song);

    engine_destroy(engine);

    return 0;
}
