#include "engine.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define CUTE_PNG_IMPLEMENTATION
#include "cute_png.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define CUTE_SOUND_IMPLEMENTATION
#define CUTE_SOUND_SCALAR_MODE
#include "cute_sound.h"

enum {
    ENGINE_INPUT_DOWN = (1 << 0),
    ENGINE_INPUT_PRESSED = (1 << 1),
    ENGINE_INPUT_RELEASED = (1 << 2),
};

static void engine_panic(char *fmt, ...) {
    fprintf(stderr, "engine error: ");
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void *engine_alloc(int n) {
    void *res = calloc(1, n);
    if (!res) { engine_panic("out of memory"); }
    return res;
}

static Rect engine_intersect_rects(Rect a, Rect b) {
    int x1 = engine_max(a.x, b.x);
    int y1 = engine_max(a.y, b.y);
    int x2 = engine_min(a.x + a.w, b.x + b.w);
    int y2 = engine_min(a.y + a.h, b.y + b.h);
    return (Rect) { x1, y1, x2 - x1, y2 - y1 };
}

static bool engine_check_input_flag(uint8_t *t, uint32_t idx, uint32_t cap, int flag) {
    if (idx > cap) { return false; }
    return t[idx] & flag ? true : false;
}

static void engine_scale_size_by_flags(int *w, int *h, int flags) {
    if (flags & ENGINE_SCALE2X) { *w *= 2; *h *= 2; } else
    if (flags & ENGINE_SCALE3X) { *w *= 3; *h *= 3; } else
    if (flags & ENGINE_SCALE4X) { *w *= 4; *h *= 4; }
}

static inline Color engine_blend_pixel(Color dst, Color src) {
    Color res;
    res.w = (dst.w & 0xff00ff) + ((((src.w & 0xff00ff) - (dst.w & 0xff00ff)) * src.a) >> 8);
    res.g = dst.g + (((src.g - dst.g) * src.a) >> 8);
    res.a = dst.a;
    return res;
}

static inline Color engine_blend_pixel2(Color dst, Color src, Color clr) {
    src.a = (src.a * clr.a) >> 8;
    int ia = 0xff - src.a;
    dst.r = ((src.r * clr.r * src.a) >> 16) + ((dst.r * ia) >> 8);
    dst.g = ((src.g * clr.g * src.a) >> 16) + ((dst.g * ia) >> 8);
    dst.b = ((src.b * clr.b * src.a) >> 16) + ((dst.b * ia) >> 8);
    return dst;
}


static inline Color engine_blend_pixel3(Color dst, Color src, Color clr, Color add) {
  src.r = engine_min(255, src.r + add.r);
  src.g = engine_min(255, src.g + add.g);
  src.b = engine_min(255, src.b + add.b);
  return engine_blend_pixel2(dst, src, clr);
}

static bool engine_check_column(Image *img, int x, int y, int h) {
    while (h > 0) {
        if (img->pixels[x + y * img->w].a) {
            return true;
        }
        y++; h--;
    }
    return false;
}


static Font *engine_load_font_from_image(Image *img) {
    if (!img) { return NULL; }
    Font *font = engine_alloc(sizeof(Font));
    font->image = img;

    for (int i = 0; i < 256; i++) {
        Glyph *g = &font->glyphs[i];
        Rect r = {
            (img->w / 16) * (i % 16),
            (img->h / 16) * (i / 16),
            img->w / 16,
            img->h / 16
        };

        for (int x = r.x + r.w - 1; x >= r.x; x--) {
            if (engine_check_column(font->image, x, r.y, r.h)) { break; }
            r.w--;
        }

        for (int x = r.x; x < r.x + r.w; x++) {
            if (engine_check_column(font->image, x, r.y, r.h)) { break; }
            r.x++;
            r.w--;
        }

        g->xadv = r.w + 1;
        g->rect = r;
    }

    font->glyphs[' '].rect = (Rect) {0};
    font->glyphs[' '].xadv = font->glyphs['a'].xadv;

    return font;
}

static Rect engine_get_adjusted_window_rect(Engine *engine) {
    float src_ar = (float) engine->screen->h / engine->screen->w;
    float dst_ar = (float) engine->height / engine->width;
    int w, h;
    if (src_ar < dst_ar) {
        w = engine->width; h = ceil(w * src_ar);
    } else {
        h = engine->height; w = ceil(h / src_ar);
    }
    return engine_rect((engine->width - w) / 2, (engine->height - h) / 2, w, h);
}

static double engine_now() {
    return clock() / 1000.0;
}

static const char *engine_get_file_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return NULL;
    return dot;
}

static char *engine_utf8_from_wchar(const WCHAR *buf) {
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    if (!len) { return NULL; }
    char *buf2 = engine_alloc(len);
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, buf2, len, NULL, NULL)) { free(buf2); return NULL; }
    return buf2;
}

static WCHAR *engine_wchar_from_utf8(const char *buf) {
    int len = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (!len) { return NULL; }
    WCHAR *buf2 = engine_alloc(len * sizeof(WCHAR));
    if (!MultiByteToWideChar(CP_UTF8, 0, buf, -1, buf2, len)) { free(buf2); return NULL; }
    return buf2;
}

static LRESULT CALLBACK engine_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Engine *engine = (Engine*)GetProp(hwnd, "engine");

    switch (message) {
    case WM_PAINT:;
        BITMAPINFO bmi = {
            .bmiHeader.biSize = sizeof(BITMAPINFOHEADER),
            .bmiHeader.biBitCount = 32,
            .bmiHeader.biCompression = BI_RGB,
            .bmiHeader.biPlanes = 1,
            .bmiHeader.biWidth = engine->screen->w,
            .bmiHeader.biHeight = -engine->screen->h
        };

        Rect wr = engine_get_adjusted_window_rect(engine);

        StretchDIBits(engine->hdc,
            wr.x, wr.y, wr.w, wr.h,
            0, 0, engine->screen->w, engine->screen->h,
            engine->screen->pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);

        ValidateRect(hwnd, 0);
        break;

    case WM_SETCURSOR:
        if (engine->hide_cursor && LOWORD(lparam) == HTCLIENT) {
            SetCursor(0);
            break;
        }
        goto unhandled;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (lparam & (1 << 30)) {
            break;
        }
        engine->key_state[(uint8_t) wparam] = ENGINE_INPUT_DOWN | ENGINE_INPUT_PRESSED;
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        engine->key_state[(uint8_t) wparam] &= ~ENGINE_INPUT_DOWN;
        engine->key_state[(uint8_t) wparam] |= ENGINE_INPUT_RELEASED;
        break;

    case WM_CHAR:
        if (wparam < 32) { break; }
        for (int i = 0; i < engine_lengthof(engine->char_buf); i++) {
            if (engine->char_buf[i]) { continue; }
            engine->char_buf[i] = wparam;
            break;
        }
        break;

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:;
        int button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) ? 1 :
                     (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 2 : 3;
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN) {
            SetCapture(hwnd);
            engine->mouse_state[button] = ENGINE_INPUT_DOWN | ENGINE_INPUT_PRESSED;
        } else {
            ReleaseCapture();
            engine->mouse_state[button] &= ~ENGINE_INPUT_DOWN;
            engine->mouse_state[button] |= ENGINE_INPUT_RELEASED;
        }

    case WM_MOUSEMOVE:;
        wr = engine_get_adjusted_window_rect(engine);
        int prevx = engine->mouse_pos.x;
        int prevy = engine->mouse_pos.y;
        engine->mouse_pos.x = (GET_X_LPARAM(lparam) - wr.x) * engine->screen->w / wr.w;
        engine->mouse_pos.y = (GET_Y_LPARAM(lparam) - wr.y) * engine->screen->h / wr.h;
        engine->mouse_delta.x += engine->mouse_pos.x - prevx;
        engine->mouse_delta.y += engine->mouse_pos.y - prevy;
        break;

    case WM_MOUSEWHEEL:
        engine->mouse_scroll = (float)GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        break;

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            engine->width = LOWORD(lparam);
            engine->height = HIWORD(lparam);

            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &ps.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);

            RedrawWindow(engine->hwnd, 0, 0, RDW_INVALIDATE | RDW_UPDATENOW);
        }
        break;

    case WM_QUIT:
    case WM_CLOSE:
        engine->should_quit = true;
        break;

    default:
unhandled:
        return DefWindowProc(hwnd, message, wparam, lparam);
    }

    return 0;
}

static void engine_blit(Image *dst, Image *src, int dx, int dy, int sx, int sy, int w, int h) {
    Color *ts = &src->pixels[sy * src->w + sx];
    Color *td = &dst->pixels[dy * dst->w + dx];
    int st = src->w;
    int dt = dst->w;
    do {
        memcpy(td, ts, w * sizeof(Color));
        ts += st;
        td += dt;
    } while (--h);
}

static char font[256][8];

Engine *engine_create(int width, int height, const char *title, int flags) {
    Engine *engine = engine_alloc(sizeof(Engine));

    engine->hide_cursor = !!(flags & ENGINE_HIDECURSOR);
    engine->step_time = 1.0 / 60.0;
    engine->screen = engine_create_image(width, height);
    engine->clip = engine_rect(0, 0, width, height);

    RegisterClass(&(WNDCLASS) {
        .style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = engine_wndproc,
        .hCursor = LoadCursor(0, IDC_ARROW),
        .lpszClassName = title
    });

    engine_scale_size_by_flags(&width, &height, flags);
    RECT rect = { .right = width, .bottom = height };
    int style = WS_OVERLAPPEDWINDOW;

    if (!(flags & ENGINE_RESIZABLE)) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }

    AdjustWindowRect(&rect, style, 0);
    engine->hwnd = CreateWindow(
        title, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        0, 0, 0, 0
    );
    SetProp(engine->hwnd, "engine", engine);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(engine->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    if (flags & ENGINE_CONSOLE) {
        AllocConsole();
        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    ShowWindow(engine->hwnd, SW_NORMAL);
    engine->hdc = GetDC(engine->hwnd);

    timeBeginPeriod(1);

    Image *font_image = engine_create_image(128, 128);

    int x = 0;
    int y = 0;

    for (int i = 0; i < 256; i++) {
        Image *glyph = engine_create_image(8, 8);

        char *bitmap = font[i];
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                if (bitmap[i] & (1 << j))
                    glyph->pixels[i * 8 + j] = ENGINE_WHITE;
                else
                    glyph->pixels[i * 8 + j] = engine_rgba(0, 0, 0, 0);
            }
        }

        engine_blit(font_image, glyph, x, y, 0, 0, 8, 8);

        x += 8;
        if (x >= 128) {
            x = 0;
            y += 8;
        }
    }

    engine->font = engine_load_font_from_image(font_image);
    engine->prev_time = engine_now();

    cs_init(engine->hwnd, 44100, 4096, NULL);
    cs_spawn_mix_thread();

    return engine;
}

void engine_destroy(Engine *engine) {
    ReleaseDC(engine->hwnd, engine->hdc);
    DestroyWindow(engine->hwnd);
    engine_destroy_image(engine->screen);
    engine_destroy_font(engine->font);
    free(engine);
    cs_shutdown();
}

bool engine_update(Engine *engine, double *dt) {
    RedrawWindow(engine->hwnd, 0, 0, RDW_INVALIDATE | RDW_UPDATENOW);

    double now = engine_now();
    double wait = (engine->prev_time + engine->step_time) - now;
    double prev = engine->prev_time;
    if (wait > 0) {
        Sleep(wait * 1000);
        engine->prev_time += engine->step_time;
    } else {
        engine->prev_time = now;
    }
    if (dt) { *dt = engine->prev_time - prev; }

    memset(engine->char_buf, 0, sizeof(engine->char_buf));
    for (int i = 0; i < sizeof(engine->key_state); i++) {
        engine->key_state[i] &= ~(ENGINE_INPUT_PRESSED | ENGINE_INPUT_RELEASED);
    }
    for (int i = 0; i < sizeof(engine->mouse_state); i++) {
        engine->mouse_state[i] &= ~(ENGINE_INPUT_PRESSED | ENGINE_INPUT_RELEASED);
    }
    engine->mouse_scroll = 0;

    cs_update(*dt);

    MSG msg;
    while (PeekMessage(&msg, engine->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return !engine->should_quit;
}

void *engine_read_file(const char *filename, int *length) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) { return NULL; }
    fseek(fp, 0, SEEK_END);
    int n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = engine_alloc(n + 1);
    fread(buf, 1, n, fp);
    fclose(fp);
    if (length) { *length = n; }
    return buf;
}

const char *engine_read_clipboard(Engine *engine) {
    if (!OpenClipboard(engine->hwnd)) { return NULL; }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return NULL; }
    WCHAR *buf = (WCHAR*) GlobalLock(h);
    if (!buf) { CloseClipboard(); return NULL; }
    const char *text = engine_utf8_from_wchar(buf);
    GlobalUnlock(h);
    CloseClipboard();
    return text;
}

void engine_write_clipboard(Engine *engine, const char *text) {
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (!len) { return; }
    HANDLE h = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(WCHAR));
    if (!h) { return; }
    WCHAR *buf = (WCHAR*) GlobalLock(h);
    if (!buf) { GlobalFree(h); return; }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, buf, len);
    GlobalUnlock(h);
    if (!OpenClipboard(engine->hwnd)) { GlobalFree(h); return; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
}

void engine_open_url(const char *url) {
    if (strchr(url, '\'') != NULL) { return; }
    char *cmd = engine_alloc(strlen(url) + 32);
    sprintf(cmd, "explorer \"%s\"", url);
    int result = system(cmd);
    free(cmd);
}

void engine_show_message_box(const char *text, const char *title) {
    WCHAR *wtitle = engine_wchar_from_utf8(title);
    WCHAR *wtext = engine_wchar_from_utf8(text);
    if (!wtitle || !wtext) { return; }
    MessageBoxW(NULL, wtext, wtitle, MB_TASKMODAL);
    free(wtitle);
    free(wtext);
}

Image *engine_create_image(int width, int height) {
    if (!(width > 0 && height > 0)) { engine_panic("invalid image size"); }
    Image *image = engine_alloc(sizeof(Image) + width * height * sizeof(Color));
    image->pixels = (Color*) (image + 1);
    image->w = width;
    image->h = height;
    return image;
}

Image *engine_load_image_mem(void *data, int length) {
    cp_image_t png = cp_load_png_mem(data, length);

    Image *img = engine_create_image(png.w, png.h);
    for (int y = 0; y < png.h; y++) {
        for (int x = 0; x < png.w; x++) {
            cp_pixel_t p = png.pix[x + y * png.w];
            img->pixels[x + y * img->w] = engine_rgba(p.r, p.g, p.b, p.a);
        }
    }

    free(png.pix);
    return img;
}

Image *engine_load_image_file(const char *filename) {
    int len;
    void *data = engine_read_file(filename, &len);
    if (!data) { return NULL; }
    Image *res = engine_load_image_mem(data, len);
    free(data);
    return res;
}

Image *engine_screenshot(Engine *engine) {
    Image *screen = engine_create_image(engine->screen->w, engine->screen->h);
    for (int y = 0; y < screen->h; y++) {
        for (int x = 0; x < screen->w; x++) {
            screen->pixels[x + y * screen->w] = engine->screen->pixels[x + y * engine->screen->w];
            screen->pixels[x + y * screen->w].a = 255;
        }
    }

    return screen;
}

void engine_save_image(Image *image, const char *filename) {
    cp_image_t png = cp_load_blank(image->w, image->h);
    for (int y = 0; y < png.h; y++) {
        for (int x = 0; x < png.w; x++) {
            cp_pixel_t *p = &png.pix[x + y * png.w];
            Color c = image->pixels[x + y * image->w];
            p->r = c.r;
            p->g = c.g;
            p->b = c.b;
            p->a = c.a;
        }
    }

    cp_save_png(filename, &png);
    free(png.pix);
}

void engine_destroy_image(Image *image) {
    free(image);
}

Font *engine_load_font_mem(void *data, int length) {
    return engine_load_font_from_image(engine_load_image_mem(data, length));
}

Font *engine_load_font_file(const char *filename) {
    return engine_load_font_from_image(engine_load_image_file(filename));
}

void engine_destroy_font(Font *font) {
    free(font->image);
    free(font);
}

int engine_text_width(Font *font, const char *text) {
    int x = 0;
    for (uint8_t *p = (void*) text; *p; p++) {
        x += font->glyphs[*p].xadv;
    }
    return x;
}

int engine_get_char(Engine *engine) {
    for (int i = 0; i < engine_lengthof(engine->char_buf); i++) {
        if (!engine->char_buf[i]) { continue; }
        int res = engine->char_buf[i];
        engine->char_buf[i] = 0;
        return res;
    }
    return 0;
}

bool engine_key_down(Engine *engine, int key) {
    return engine_check_input_flag(engine->key_state, key, sizeof(engine->key_state), ENGINE_INPUT_DOWN);
}

bool engine_key_pressed(Engine *engine, int key) {
    return engine_check_input_flag(engine->key_state, key, sizeof(engine->key_state), ENGINE_INPUT_PRESSED);
}

bool engine_key_released(Engine *engine, int key) {
    return engine_check_input_flag(engine->key_state, key, sizeof(engine->key_state), ENGINE_INPUT_RELEASED);
}

void engine_mouse_pos(Engine *engine, int *x, int *y) {
    if (x) { *x = engine->mouse_pos.x; }
    if (y) { *y = engine->mouse_pos.y; }
}

void engine_mouse_delta(Engine *engine, int *x, int *y) {
    if (x) { *x = engine->mouse_delta.x; }
    if (y) { *y = engine->mouse_delta.y; }
}

bool engine_mouse_down(Engine *engine, int button) {
    return engine_check_input_flag(engine->mouse_state, button, sizeof(engine->mouse_state), ENGINE_INPUT_DOWN);
}

bool engine_mouse_pressed(Engine *engine, int button) {
    return engine_check_input_flag(engine->mouse_state, button, sizeof(engine->mouse_state), ENGINE_INPUT_PRESSED);
}

bool engine_mouse_released(Engine *engine, int button) {
    return engine_check_input_flag(engine->mouse_state, button, sizeof(engine->mouse_state), ENGINE_INPUT_RELEASED);
}

float engine_mouse_scroll(Engine *engine) {
    return engine->mouse_scroll;
}

void engine_clear(Engine *engine, Color color) {
    engine_draw_rect_fill(engine, engine_rect(0, 0, 0xffffff, 0xffffff), color);
}

void engine_set_clip(Engine *engine, Rect rect)
{
    Rect screen_rect = engine_rect(0, 0, engine->screen->w, engine->screen->h);
    engine->clip = engine_intersect_rects(rect, screen_rect);
}

void engine_draw_point(Engine *engine, int x, int y, Color color) {
    if (color.a == 0) { return; }
    Rect r = engine->clip;
    if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h ) {
        return;
    }
    Color *dst = &engine->screen->pixels[x + y * engine->screen->w];
    *dst = engine_blend_pixel(*dst, color);
}

void engine_draw_rect(Engine *engine, Rect rect, Color color) {
    if (color.a == 0) { return; }
    if (rect.w <= 0 || rect.h <= 0) { return; }
    if (rect.w == 1) {
        engine_draw_line(engine, rect.x, rect.y, rect.x, rect.y + rect.h, color);
    } else if (rect.h == 1) {
        engine_draw_line(engine, rect.x, rect.y, rect.x + rect.w, rect.y, color);
    } else {
        int x1 = rect.x + rect.w - 1;
        int y1 = rect.y + rect.h - 1;
        engine_draw_line(engine, rect.x, rect.y, x1, rect.y, color);
        engine_draw_line(engine, x1, rect.y, x1, y1, color);
        engine_draw_line(engine, x1, y1, rect.x, y1, color);
        engine_draw_line(engine, rect.x, y1, rect.x, rect.y, color);
    }
}

void engine_draw_rect_fill(Engine *engine, Rect rect, Color color) {
    if (color.a == 0) { return; }
    rect = engine_intersect_rects(rect, engine->clip);
    Color *d = &engine->screen->pixels[rect.x + rect.y * engine->screen->w];
    int dr = engine->screen->w - rect.w;
    for (int y = 0; y < rect.h; y++) {
        for (int x = 0; x < rect.w; x++) {
            *d = engine_blend_pixel(*d, color);
            d++;
        }
        d += dr;
    }
}

void engine_draw_circle(Engine *engine, int x0, int y0, int radius, Color color) {
    if (color.a == 0) { return; }

    int E = 1 - radius;
    int dx = 0;
    int dy = -2 * radius;
    int x = 0;
    int y = radius;

    engine_draw_point(engine, x0, y0 + radius, color);
    engine_draw_point(engine, x0, y0 - radius, color);
    engine_draw_point(engine, x0 + radius, y0, color);
    engine_draw_point(engine, x0 - radius, y0, color);

    while (x < y - 1) {
        x++;

        if (E >= 0) {
            y--;
            dy += 2;
            E += dy;
        }

        dx += 2;
        E += dx + 1;

        engine_draw_point(engine, x0 + x, y0 + y, color);
        engine_draw_point(engine, x0 - x, y0 + y, color);
        engine_draw_point(engine, x0 + x, y0 - y, color);
        engine_draw_point(engine, x0 - x, y0 - y, color);

        if (x != y) {
            engine_draw_point(engine, x0 + y, y0 + x, color);
            engine_draw_point(engine, x0 - y, y0 + x, color);
            engine_draw_point(engine, x0 + y, y0 - x, color);
            engine_draw_point(engine, x0 - y, y0 - x, color);
        }
    }
}

void engine_draw_circle_fill(Engine *engine, int x0, int y0, int radius, Color color) {
    if (color.a == 0) { return; }
    if (radius <= 0) { return; }

    int E = 1 - radius;
    int dx = 0;
    int dy = -2 * radius;
    int x = 0;
    int y = radius;

    engine_draw_line(engine, x0 - radius + 1, y0, x0 + radius, y0, color);

    while (x < y - 1) {
        x++;

        if (E >= 0) {
            y--;
            dy += 2;
            E += dy;
            engine_draw_line(engine, x0 - x + 1, y0 + y, x0 + x, y0 + y, color);
            engine_draw_line(engine, x0 - x + 1, y0 - y, x0 + x, y0 - y, color);
        }

        dx += 2;
        E += dx + 1;

        if (x != y) {
            engine_draw_line(engine, x0 - y + 1, y0 + x, x0 + y, y0 + x, color);
            engine_draw_line(engine, x0 - y + 1, y0 - x, x0 + y, y0 - x, color);
        }
    }
}

void engine_draw_line(Engine *engine, int x1, int y1, int x2, int y2, Color color) {
    int dx = abs(x2 - x1);
    int sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        engine_draw_point(engine, x1, y1, color);
        if (x1 == x2 && y1 == y2) { break; }
        int e2 = err << 1;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void engine_draw_image(Engine *engine, Image *img, int x, int y) {
    Rect dst = engine_rect(x, y, img->w, img->h);
    Rect src = engine_rect(0, 0, img->w, img->h);
    engine_draw_image3(engine, img, dst, src, ENGINE_WHITE, ENGINE_BLACK);
}

void engine_draw_image2(Engine *engine, Image *img, int x, int y, Rect src, Color color) {
    Rect dst = engine_rect(x, y, abs(src.w), abs(src.h));
    engine_draw_image3(engine, img, dst, src, color, ENGINE_BLACK);
}

void engine_draw_image3(Engine *engine, Image *img, Rect dst, Rect src, Color mul_color, Color add_color) {
    if (!src.w || !src.w || !dst.w || !dst.h) {
        return;
    }

    int cx1 = engine->clip.x;
    int cy1 = engine->clip.y;
    int cx2 = cx1 + engine->clip.w;
    int cy2 = cy1 + engine->clip.h;
    int stepx = (src.w << 10) / dst.w;
    int stepy = (src.h << 10) / dst.h;
    int sy = src.y << 10;

    int dy = dst.y;
    if (dy < cy1) { sy += (cy1 - dy) * stepy; dy = cy1; }
    int ey = engine_min(cy2, dst.y + dst.h);

    int blend_fn = 1;
    if (mul_color.w != 0xffffffff) { blend_fn = 2; }
    if ((add_color.w & 0xffffff00) != 0xffffff00) { blend_fn = 3; }

    for (; dy < ey; dy++) {
        if (dy >= cy1 && dy < cy2) {
            int sx = src.x << 10;
            Color *srow = &img->pixels[(sy >> 10) * img->w];
            Color *drow = &engine->screen->pixels[dy * engine->screen->w];

            int dx = dst.x;
            if (dx < cx1) { sx += (cx1 - dx) * stepx; dx = cx1; }
            int ex = engine_min(cx2, dst.x + dst.w);

            for (; dx < ex; dx++) {
                Color *s = &srow[sx >> 10];
                Color *d = &drow[dx];
                switch (blend_fn) {
                case 1: *d = engine_blend_pixel (*d, *s); break;
                case 2: *d = engine_blend_pixel2(*d, *s, mul_color); break;
                case 3: *d = engine_blend_pixel3(*d, *s, mul_color, add_color); break;
                }
                sx += stepx;
            }
        }
        sy += stepy;
    }
}

int engine_draw_text(Engine *engine, char *text, int x, int y, Color color) {
    return engine_draw_text2(engine, engine->font, text, x, y, color);
}

int engine_draw_text2(Engine *engine, Font *font, char *text, int x, int y, Color color) {
    for (uint8_t *p = (void*) text; *p; p++) {
        Glyph g = font->glyphs[*p];
        engine_draw_image2(engine, font->image, x, y, g.rect, color);
        x += g.xadv;
    }
    return x;
}

void engine_set_volume(float volume) {
    cs_set_global_volume(volume);
}

void engine_set_pan(float pan) {
    cs_set_global_pan(pan);
}

void engine_set_pause(bool pause) {
    cs_set_global_pause(pause);
}

Sound *engine_load_sound_mem_wav(void *data, int length) {
    return cs_read_mem_wav(data, length, NULL);
}

Sound *engine_load_sound_mem_ogg(void *data, int length) {
    return cs_read_mem_ogg(data, length, NULL);
}

Sound *engine_load_sound_file(const char *filename) {
    const char *ext = engine_get_file_extension(filename);

    if (strcmp(ext, ".wav") == 0) {
        return cs_load_wav(filename, NULL);
    } else if (strcmp(ext, ".ogg") == 0) {
        return cs_load_ogg(filename, NULL);
    }

    return NULL;
}

void engine_destroy_sound(Sound *sound) {
    cs_free_audio_source(sound);
}

void engine_play_sound(Sound *sound) {
    cs_play_sound(sound, cs_sound_params_default());
}

void engine_play_music(Sound *sound, float fade) {
    cs_music_play(sound, fade);
}

void engine_stop_music(float fade) {
    cs_music_stop(fade);
}

void engine_pause_music() {
    cs_music_pause();
}

void engine_resume_music() {
    cs_music_resume();
}

void engine_set_music_volume(float volume) {
    cs_music_set_volume(volume);
}

void engine_set_music_loop(bool loop) {
    cs_music_set_loop(loop);
}

void engine_switch_music(Sound *sound, float fade_out, float fade_in) {
    cs_music_switch_to(sound, fade_out, fade_in);
}

// Investigate why the other sound functions don't work...

static char font[256][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0000 (nul)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0001
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0002
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0003
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0004
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0005
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0006
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0007
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0008
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0009
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000A
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000B
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000C
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000D
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000E
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0010
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0011
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0012
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0013
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0014
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0015
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0016
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0017
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0018
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0019
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001A
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001B
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001C
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001D
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001E
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0020 (space)
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},   // U+0021 (!)
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0022 (")
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},   // U+0023 (#)
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},   // U+0024 ($)
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},   // U+0025 (%)
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},   // U+0026 (&)
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0027 (')
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},   // U+0028 (()
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},   // U+0029 ())
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},   // U+002A (*)
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},   // U+002B (+)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},   // U+002C (,)
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},   // U+002D (-)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},   // U+002E (.)
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},   // U+002F (/)
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},   // U+0030 (0)
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},   // U+0031 (1)
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},   // U+0032 (2)
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},   // U+0033 (3)
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},   // U+0034 (4)
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},   // U+0035 (5)
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},   // U+0036 (6)
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},   // U+0037 (7)
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+0038 (8)
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},   // U+0039 (9)
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},   // U+003A (:)
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},   // U+003B (;)
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},   // U+003C (<)
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},   // U+003D (=)
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},   // U+003E (>)
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},   // U+003F (?)
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},   // U+0040 (@)
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},   // U+0041 (A)
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},   // U+0042 (B)
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},   // U+0043 (C)
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},   // U+0044 (D)
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},   // U+0045 (E)
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},   // U+0046 (F)
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},   // U+0047 (G)
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},   // U+0048 (H)
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0049 (I)
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},   // U+004A (J)
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},   // U+004B (K)
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},   // U+004C (L)
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},   // U+004D (M)
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},   // U+004E (N)
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},   // U+004F (O)
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},   // U+0050 (P)
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},   // U+0051 (Q)
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},   // U+0052 (R)
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},   // U+0053 (S)
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0054 (T)
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},   // U+0055 (U)
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   // U+0056 (V)
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},   // U+0057 (W)
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},   // U+0058 (X)
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},   // U+0059 (Y)
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},   // U+005A (Z)
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},   // U+005B ([)
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},   // U+005C (\)
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},   // U+005D (])
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},   // U+005E (^)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},   // U+005F (_)
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0060 (`)
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},   // U+0061 (a)
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},   // U+0062 (b)
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},   // U+0063 (c)
    { 0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},   // U+0064 (d)
    { 0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},   // U+0065 (e)
    { 0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},   // U+0066 (f)
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+0067 (g)
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},   // U+0068 (h)
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0069 (i)
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},   // U+006A (j)
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},   // U+006B (k)
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+006C (l)
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},   // U+006D (m)
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},   // U+006E (n)
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},   // U+006F (o)
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},   // U+0070 (p)
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},   // U+0071 (q)
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},   // U+0072 (r)
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},   // U+0073 (s)
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},   // U+0074 (t)
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},   // U+0075 (u)
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   // U+0076 (v)
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},   // U+0077 (w)
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},   // U+0078 (x)
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+0079 (y)
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},   // U+007A (z)
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},   // U+007B ({)
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},   // U+007C (|)
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},   // U+007D (})
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+007E (~)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+007F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0080
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0081
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0082
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0083
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0084
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0085
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0086
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0087
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0088
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0089
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+008A
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+008B
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+008C
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+008D
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+008E
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+008F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0090
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0091
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0092
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0093
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0094
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0095
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0096
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0097
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0098
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0099
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+009A
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+009B
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+009C
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+009D
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+009E
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+009F
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+00A0 (no break space)
    { 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00},   // U+00A1 (inverted !)
    { 0x18, 0x18, 0x7E, 0x03, 0x03, 0x7E, 0x18, 0x18},   // U+00A2 (dollarcents)
    { 0x1C, 0x36, 0x26, 0x0F, 0x06, 0x67, 0x3F, 0x00},   // U+00A3 (pound sterling)
    { 0x00, 0x00, 0x63, 0x3E, 0x36, 0x3E, 0x63, 0x00},   // U+00A4 (currency mark)
    { 0x33, 0x33, 0x1E, 0x3F, 0x0C, 0x3F, 0x0C, 0x0C},   // U+00A5 (yen)
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},   // U+00A6 (broken pipe)
    { 0x7C, 0xC6, 0x1C, 0x36, 0x36, 0x1C, 0x33, 0x1E},   // U+00A7 (paragraph)
    { 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+00A8 (diaeresis)
    { 0x3C, 0x42, 0x99, 0x85, 0x85, 0x99, 0x42, 0x3C},   // U+00A9 (copyright symbol)
    { 0x3C, 0x36, 0x36, 0x7C, 0x00, 0x00, 0x00, 0x00},   // U+00AA (superscript a)
    { 0x00, 0xCC, 0x66, 0x33, 0x66, 0xCC, 0x00, 0x00},   // U+00AB (<<)
    { 0x00, 0x00, 0x00, 0x3F, 0x30, 0x30, 0x00, 0x00},   // U+00AC (gun pointing left)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+00AD (soft hyphen)
    { 0x3C, 0x42, 0x9D, 0xA5, 0x9D, 0xA5, 0x42, 0x3C},   // U+00AE (registered symbol)
    { 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+00AF (macron)
    { 0x1C, 0x36, 0x36, 0x1C, 0x00, 0x00, 0x00, 0x00},   // U+00B0 (degree)
    { 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x7E, 0x00},   // U+00B1 (plusminus)
    { 0x1C, 0x30, 0x18, 0x0C, 0x3C, 0x00, 0x00, 0x00},   // U+00B2 (superscript 2)
    { 0x1C, 0x30, 0x18, 0x30, 0x1C, 0x00, 0x00, 0x00},   // U+00B2 (superscript 3)
    { 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+00B2 (aigu)
    { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x03},   // U+00B5 (mu)
    { 0xFE, 0xDB, 0xDB, 0xDE, 0xD8, 0xD8, 0xD8, 0x00},   // U+00B6 (pilcrow)
    { 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00},   // U+00B7 (central dot)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x30, 0x1E},   // U+00B8 (cedille)
    { 0x08, 0x0C, 0x08, 0x1C, 0x00, 0x00, 0x00, 0x00},   // U+00B9 (superscript 1)
    { 0x1C, 0x36, 0x36, 0x1C, 0x00, 0x00, 0x00, 0x00},   // U+00BA (superscript 0)
    { 0x00, 0x33, 0x66, 0xCC, 0x66, 0x33, 0x00, 0x00},   // U+00BB (>>)
    { 0xC3, 0x63, 0x33, 0xBD, 0xEC, 0xF6, 0xF3, 0x03},   // U+00BC (1/4)
    { 0xC3, 0x63, 0x33, 0x7B, 0xCC, 0x66, 0x33, 0xF0},   // U+00BD (1/2)
    { 0x03, 0xC4, 0x63, 0xB4, 0xDB, 0xAC, 0xE6, 0x80},   // U+00BE (3/4)
    { 0x0C, 0x00, 0x0C, 0x06, 0x03, 0x33, 0x1E, 0x00},   // U+00BF (inverted ?)
    { 0x07, 0x00, 0x1C, 0x36, 0x63, 0x7F, 0x63, 0x00},   // U+00C0 (A grave)
    { 0x70, 0x00, 0x1C, 0x36, 0x63, 0x7F, 0x63, 0x00},   // U+00C1 (A aigu)
    { 0x1C, 0x36, 0x00, 0x3E, 0x63, 0x7F, 0x63, 0x00},   // U+00C2 (A circumflex)
    { 0x6E, 0x3B, 0x00, 0x3E, 0x63, 0x7F, 0x63, 0x00},   // U+00C3 (A ~)
    { 0x63, 0x1C, 0x36, 0x63, 0x7F, 0x63, 0x63, 0x00},   // U+00C4 (A umlaut)
    { 0x0C, 0x0C, 0x00, 0x1E, 0x33, 0x3F, 0x33, 0x00},   // U+00C5 (A ring)
    { 0x7C, 0x36, 0x33, 0x7F, 0x33, 0x33, 0x73, 0x00},   // U+00C6 (AE)
    { 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x18, 0x30, 0x1E},   // U+00C7 (C cedille)
    { 0x07, 0x00, 0x3F, 0x06, 0x1E, 0x06, 0x3F, 0x00},   // U+00C8 (E grave)
    { 0x38, 0x00, 0x3F, 0x06, 0x1E, 0x06, 0x3F, 0x00},   // U+00C9 (E aigu)
    { 0x0C, 0x12, 0x3F, 0x06, 0x1E, 0x06, 0x3F, 0x00},   // U+00CA (E circumflex)
    { 0x36, 0x00, 0x3F, 0x06, 0x1E, 0x06, 0x3F, 0x00},   // U+00CB (E umlaut)
    { 0x07, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+00CC (I grave)
    { 0x38, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+00CD (I aigu)
    { 0x0C, 0x12, 0x00, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},   // U+00CE (I circumflex)
    { 0x33, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+00CF (I umlaut)
    { 0x3F, 0x66, 0x6F, 0x6F, 0x66, 0x66, 0x3F, 0x00},   // U+00D0 (Eth)
    { 0x3F, 0x00, 0x33, 0x37, 0x3F, 0x3B, 0x33, 0x00},   // U+00D1 (N ~)
    { 0x0E, 0x00, 0x18, 0x3C, 0x66, 0x3C, 0x18, 0x00},   // U+00D2 (O grave)
    { 0x70, 0x00, 0x18, 0x3C, 0x66, 0x3C, 0x18, 0x00},   // U+00D3 (O aigu)
    { 0x3C, 0x66, 0x18, 0x3C, 0x66, 0x3C, 0x18, 0x00},   // U+00D4 (O circumflex)
    { 0x6E, 0x3B, 0x00, 0x3E, 0x63, 0x63, 0x3E, 0x00},   // U+00D5 (O ~)
    { 0xC3, 0x18, 0x3C, 0x66, 0x66, 0x3C, 0x18, 0x00},   // U+00D6 (O umlaut)
    { 0x00, 0x36, 0x1C, 0x08, 0x1C, 0x36, 0x00, 0x00},   // U+00D7 (multiplicative x)
    { 0x5C, 0x36, 0x73, 0x7B, 0x6F, 0x36, 0x1D, 0x00},   // U+00D8 (O stroke)
    { 0x0E, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},   // U+00D9 (U grave)
    { 0x70, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},   // U+00DA (U aigu)
    { 0x3C, 0x66, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x00},   // U+00DB (U circumflex)
    { 0x33, 0x00, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x00},   // U+00DC (U umlaut)
    { 0x70, 0x00, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x00},   // U+00DD (Y aigu)
    { 0x0F, 0x06, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x0F},   // U+00DE (Thorn)
    { 0x00, 0x1E, 0x33, 0x1F, 0x33, 0x1F, 0x03, 0x03},   // U+00DF (beta)
    { 0x07, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x7E, 0x00},   // U+00E0 (a grave)
    { 0x38, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x7E, 0x00},   // U+00E1 (a aigu)
    { 0x7E, 0xC3, 0x3C, 0x60, 0x7C, 0x66, 0xFC, 0x00},   // U+00E2 (a circumflex)
    { 0x6E, 0x3B, 0x1E, 0x30, 0x3E, 0x33, 0x7E, 0x00},   // U+00E3 (a ~)
    { 0x33, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x7E, 0x00},   // U+00E4 (a umlaut)
    { 0x0C, 0x0C, 0x1E, 0x30, 0x3E, 0x33, 0x7E, 0x00},   // U+00E5 (a ring)
    { 0x00, 0x00, 0xFE, 0x30, 0xFE, 0x33, 0xFE, 0x00},   // U+00E6 (ae)
    { 0x00, 0x00, 0x1E, 0x03, 0x03, 0x1E, 0x30, 0x1C},   // U+00E7 (c cedille)
    { 0x07, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},   // U+00E8 (e grave)
    { 0x38, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},   // U+00E9 (e aigu)
    { 0x7E, 0xC3, 0x3C, 0x66, 0x7E, 0x06, 0x3C, 0x00},   // U+00EA (e circumflex)
    { 0x33, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},   // U+00EB (e umlaut)
    { 0x07, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+00EC (i grave)
    { 0x1C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+00ED (i augu)
    { 0x3E, 0x63, 0x1C, 0x18, 0x18, 0x18, 0x3C, 0x00},   // U+00EE (i circumflex)
    { 0x33, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+00EF (i umlaut)
    { 0x1B, 0x0E, 0x1B, 0x30, 0x3E, 0x33, 0x1E, 0x00},   // U+00F0 (eth)
    { 0x00, 0x1F, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x00},   // U+00F1 (n ~)
    { 0x00, 0x07, 0x00, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+00F2 (o grave)
    { 0x00, 0x38, 0x00, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+00F3 (o aigu)
    { 0x1E, 0x33, 0x00, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+00F4 (o circumflex)
    { 0x6E, 0x3B, 0x00, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+00F5 (o ~)
    { 0x00, 0x33, 0x00, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+00F6 (o umlaut)
    { 0x18, 0x18, 0x00, 0x7E, 0x00, 0x18, 0x18, 0x00},   // U+00F7 (division)
    { 0x00, 0x60, 0x3C, 0x76, 0x7E, 0x6E, 0x3C, 0x06},   // U+00F8 (o stroke)
    { 0x00, 0x07, 0x00, 0x33, 0x33, 0x33, 0x7E, 0x00},   // U+00F9 (u grave)
    { 0x00, 0x38, 0x00, 0x33, 0x33, 0x33, 0x7E, 0x00},   // U+00FA (u aigu)
    { 0x1E, 0x33, 0x00, 0x33, 0x33, 0x33, 0x7E, 0x00},   // U+00FB (u circumflex)
    { 0x00, 0x33, 0x00, 0x33, 0x33, 0x33, 0x7E, 0x00},   // U+00FC (u umlaut)
    { 0x00, 0x38, 0x00, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+00FD (y aigu)
    { 0x00, 0x00, 0x06, 0x3E, 0x66, 0x3E, 0x06, 0x00},   // U+00FE (thorn)
    { 0x00, 0x33, 0x00, 0x33, 0x33, 0x3E, 0x30, 0x1F}    // U+00FF (y umlaut)
};

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
