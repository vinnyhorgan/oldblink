#ifndef ENGINE_H
#define ENGINE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

enum {
    ENGINE_SCALE2X = (1 << 0),
    ENGINE_SCALE3X = (1 << 1),
    ENGINE_SCALE4X = (1 << 2),
    ENGINE_CONSOLE = (1 << 3),
    ENGINE_RESIZABLE = (1 << 4),
    ENGINE_HIDECURSOR = (1 << 5)
};

typedef union { struct { uint8_t b, g, r, a; }; uint32_t w; } Color;
typedef struct { int x, y, w, h; } Rect;
typedef struct { Color *pixels; int w, h; } Image;
typedef struct { Rect rect; int xadv; } Glyph;
typedef struct { Image *image; Glyph glyphs[256]; } Font;

typedef struct {
    bool should_quit;
    bool hide_cursor;

    int char_buf[32];
    uint8_t key_state[256];
    uint8_t mouse_state[16];
    struct { int x, y; } mouse_pos;
    struct { int x, y; } mouse_delta;
    float mouse_scroll;

    double step_time;
    double prev_time;

    Rect clip;
    Image *screen;
    Font *font;

    int width, height;
    HWND hwnd;
    HDC hdc;
} Engine;

struct cs_audio_source_t;
typedef struct cs_audio_source_t Sound;

#define engine_max(a, b) ((a) > (b) ? (a) : (b))
#define engine_min(a, b) ((a) < (b) ? (a) : (b))
#define engine_lengthof(a) (sizeof(a) / sizeof((a)[0]))

#define engine_rect(X, Y, W, H) ((Rect) { (X), (Y), (W), (H) })
#define engine_rgba(R, G, B, A) ((Color) { .r = (R), .g = (G), .b = (B), .a = (A) })
#define engine_rgb(R, G, B) engine_rgba(R, G, B, 0xff)

#define ENGINE_WHITE engine_rgb(0xff, 0xff, 0xff)
#define ENGINE_BLACK engine_rgb(0, 0, 0)

Engine *engine_create(int width, int height, const char *title, int flags);
void engine_destroy(Engine *engine);
bool engine_update(Engine *engine, double *dt);
void *engine_read_file(const char *filename, int *length);
const char *engine_read_clipboard(Engine *engine);
void engine_write_clipboard(Engine *engine, const char *text);
void engine_open_url(const char *url);
void engine_show_message_box(const char *text, const char *title);

Image *engine_create_image(int width, int height);
Image *engine_load_image_mem(void *data, int length);
Image *engine_load_image_file(const char *filename);
Image *engine_screenshot(Engine *engine);
void engine_save_image(Image *image, const char *filename);
void engine_destroy_image(Image *image);

Font *engine_load_font_mem(void *data, int length);
Font *engine_load_font_file(const char *filename);
void engine_destroy_font(Font *font);
int engine_text_width(Font *font, const char *text);

int engine_get_char(Engine *engine);
bool engine_key_down(Engine *engine, int key);
bool engine_key_pressed(Engine *engine, int key);
bool engine_key_released(Engine *engine, int key);
void engine_mouse_pos(Engine *engine, int *x, int *y);
void engine_mouse_delta(Engine *engine, int *x, int *y);
bool engine_mouse_down(Engine *engine, int button);
bool engine_mouse_pressed(Engine *engine, int button);
bool engine_mouse_released(Engine *engine, int button);
float engine_mouse_scroll(Engine *engine);

void engine_clear(Engine *engine, Color color);
void engine_set_clip(Engine *engine, Rect rect);
void engine_draw_point(Engine *engine, int x, int y, Color color);
void engine_draw_rect(Engine *engine, Rect rect, Color color);
void engine_draw_rect_fill(Engine *engine, Rect rect, Color color);
void engine_draw_circle(Engine *engine, int x0, int y0, int radius, Color color);
void engine_draw_circle_fill(Engine *engine, int x0, int y0, int radius, Color color);
void engine_draw_line(Engine *engine, int x1, int y1, int x2, int y2, Color color);
void engine_draw_image(Engine *engine, Image *img, int x, int y);
void engine_draw_image2(Engine *engine, Image *img, int x, int y, Rect src, Color color);
void engine_draw_image3(Engine *engine, Image *img, Rect dst, Rect src, Color mul_color, Color add_color);
int engine_draw_text(Engine *engine, char *text, int x, int y, Color color);
int engine_draw_text2(Engine *engine, Font *font, char *text, int x, int y, Color color);

void engine_set_volume(float volume);
void engine_set_pan(float pan);
void engine_set_pause(bool pause);

Sound *engine_load_sound_mem_wav(void *data, int length);
Sound *engine_load_sound_mem_ogg(void *data, int length);
Sound *engine_load_sound_file(const char *filename);
void engine_destroy_sound(Sound *sound);

void engine_play_sound(Sound *sound);
void engine_play_music(Sound *sound, float fade);
void engine_stop_music(float fade);
void engine_pause_music();
void engine_resume_music();
void engine_set_music_volume(float volume);
void engine_set_music_loop(bool loop);
void engine_switch_music(Sound *sound, float fade_out, float fade_in);

#endif
