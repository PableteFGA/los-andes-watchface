#include <pebble.h>

// Three hands — same design language: tapered shaft, circular boss, red tip, tail.
//
//   Hour:    boss R=10, tip y=-80  (cutoff y=-72),  center = s_center
//   Minute:  boss R=8,  tip y=-119 (cutoff y=-109), center = s_center
//   Seconds: boss R=3,  tip y=-34  (cutoff y=-31),  center = (130,184)
//            40 px total (tip to tail). Boss: white, NO outline.

static Window  *s_window;
static Layer   *s_canvas;
static GPoint   s_center;
static GBitmap *s_background;
static GBitmap *s_battery_icon;

// Fixed pivot for the seconds hand / battery display (not the watch center)
static const GPoint SEC_CENTER = {130, 184};

// true  → seconds hand + tick marks
// false → battery indicator (9 segments, 3 zones: red / dark-blue / white)
static bool s_display_seconds = true;

// Shake animation: hour does 1 revolution, minute does 3, over 6 seconds
#define ANIM_DURATION_MS 3000
#define ANIM_INTERVAL_MS   50

static AppTimer *s_anim_timer    = NULL;
static uint32_t  s_anim_elapsed  = 0;
static int32_t   s_anim_hour_start = 0;
static int32_t   s_anim_min_start  = 0;

// Battery display geometry:
// 11 segments over the lower semicircle (180° arc), CCW from 270° (left=0%)
// through 180° (bottom=50%) to 90° (right=100%).
// Step = -18° per segment (CCW direction).

// ── Hour hand ──────────────────────────────────────────────────────────────
static GPath *s_hour_body;
static GPath *s_hour_tip;

static GPoint s_hour_body_pts[] = {
    { 3,  -9},   { 5, -72},   {-5, -72},   {-3,  -9},
    {-3,   9},   {-3,  20},   { 3,  20},   { 3,   9},
};
static GPoint s_hour_tip_pts[] = {
    { 6, -72},   { 3, -77},   { 2, -80},   {-2, -80},   {-3, -77},   {-6, -72},
};

// ── Minute hand ────────────────────────────────────────────────────────────
static GPath *s_min_body;
static GPath *s_min_tip;

static GPoint s_min_body_pts[] = {
    { 3,  -7},   { 5,-109},   {-5,-109},   {-3,  -7},
    {-3,   7},   {-3,  22},   { 3,  22},   { 3,   7},
};
static GPoint s_min_tip_pts[] = {
    { 6,-109},   { 3,-115},   { 2,-119},   {-2,-119},   {-3,-115},   {-6,-109},
};

// ── Seconds hand ───────────────────────────────────────────────────────────
// Drawn with stroke_width lines — reliable at all rotation angles.
// GPath fill becomes invisible at ~45° when the polygon is only 1-2 px wide.
// Three segments with decreasing width create the taper:
//   counterweight: 7 px wide  (y=0 → y=+6)
//   main shaft:    4 px wide  (y=0 → y=-22)
//   thin tip:      2 px wide  (y=-22 → y=-34)
// Boss R=3, white, no outline.

// ============================================================================
// DRAWING HELPERS
// ============================================================================

static int16_t trig_round(int32_t val) {
    if (val > 0) return (int16_t)((val + TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO);
    if (val < 0) return (int16_t)((val - TRIG_MAX_RATIO / 2) / TRIG_MAX_RATIO);
    return 0;
}

// Rotate pt around an arbitrary center point
static GPoint rotate_at(GPoint pt, int32_t angle, GPoint center) {
    int32_t c = cos_lookup(angle);
    int32_t s = sin_lookup(angle);
    return (GPoint){
        .x = center.x + trig_round(pt.x * c - pt.y * s),
        .y = center.y + trig_round(pt.x * s + pt.y * c)
    };
}


static GPoint rotate_gpoint(GPoint pt, int32_t angle) {
    return rotate_at(pt, angle, s_center);
}

// ── Hour / minute shared draw ───────────────────────────────────────────────

static void fill_hand(GContext *ctx, int32_t angle,
                      GPath *body, GPath *tip, int boss_r) {
    gpath_rotate_to(body, angle);  gpath_move_to(body, s_center);
    gpath_rotate_to(tip,  angle);  gpath_move_to(tip,  s_center);

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, s_center, boss_r);
    gpath_draw_filled(ctx, body);

    graphics_context_set_fill_color(ctx, GColorRed);
    gpath_draw_filled(ctx, tip);
}


static void outline_hand(GContext *ctx, int32_t angle,
                         int boss_r, int32_t boss_half_ang,
                         const GPoint *segs, int n) {
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);

    for (int i = 0; i + 1 < n; i += 2) {
        graphics_draw_line(ctx,
            rotate_gpoint(segs[i],     angle),
            rotate_gpoint(segs[i + 1], angle));
    }

    // Arc at boss_r+1 so fill_circle (radius boss_r) cannot cover it
    GRect br = GRect(s_center.x - boss_r - 1, s_center.y - boss_r - 1,
                     2 * boss_r + 2, 2 * boss_r + 2);
    int32_t half = TRIG_MAX_ANGLE / 2;

    // Right arc (CW through 90°) and left arc (CW through 270°)
    graphics_draw_arc(ctx, br, GOvalScaleModeFitCircle,
                      angle + boss_half_ang, angle + half - boss_half_ang);
    graphics_draw_arc(ctx, br, GOvalScaleModeFitCircle,
                      angle + half + boss_half_ang,
                      angle + TRIG_MAX_ANGLE - boss_half_ang);
}

static void draw_hour_hand(GContext *ctx, int32_t angle) {
    static const GPoint segs[] = {
        { 3, -9}, { 5,-72},   {-5,-72}, {-3, -9},
        {-3,  9}, {-3, 20},   {-3, 20}, { 3, 20},   { 3, 20}, { 3,  9},
    };
    outline_hand(ctx, angle, 10, 17 * TRIG_MAX_ANGLE / 360, segs, ARRAY_LENGTH(segs));
    fill_hand(ctx, angle, s_hour_body, s_hour_tip, 10);
}

static void draw_min_hand(GContext *ctx, int32_t angle) {
    static const GPoint segs[] = {
        { 3, -7}, { 5,-109},  {-5,-109},{-3,  -7},
        {-3,  7}, {-3,  22},  {-3, 22}, { 3,  22},  { 3, 22}, { 3,  7},
    };
    outline_hand(ctx, angle, 8, 22 * TRIG_MAX_ANGLE / 360, segs, ARRAY_LENGTH(segs));
    fill_hand(ctx, angle, s_min_body, s_min_tip, 8);
}

// ── Seconds hand ────────────────────────────────────────────────────────────

static void draw_sec_hand(GContext *ctx, int32_t angle) {
    GPoint cw  = rotate_at(GPoint(0,   9), angle, SEC_CENTER);
    GPoint mid = rotate_at(GPoint(0, -30), angle, SEC_CENTER);
    GPoint tip = rotate_at(GPoint(0, -34), angle, SEC_CENTER);

    // Outline pass 
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 5);
    graphics_draw_line(ctx, SEC_CENTER, cw);
    graphics_context_set_stroke_width(ctx, 4);
    graphics_draw_line(ctx, SEC_CENTER, mid);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, mid, tip);

    // White fill pass
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, SEC_CENTER, cw);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, SEC_CENTER, mid);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, mid, tip);

    // Boss circle (white, no outline) + small dark center dot
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, SEC_CENTER, 3);
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_circle(ctx, SEC_CENTER, 1);
}

// ── Battery indicator ───────────────────────────────────────────────────────
// 9 visible segments (clock positions 5,6,7 hidden at the bottom).
// Segments light up from bottom-left (0 %) to bottom-right (100 %).
// Unlit segments are drawn dim (GColorDarkGray) so the gauge shape is visible.
static void draw_battery_display(GContext *ctx) {
    BatteryChargeState bs = battery_state_service_peek();
    int pct = (int)bs.charge_percent;

    // Lower semicircle: start=270° (left=0%), end=90° (right=100%), CCW through 180°
    int32_t start = 3 * TRIG_MAX_ANGLE / 4;        // 270°
    int32_t span  = TRIG_MAX_ANGLE / 2;             // 180°
    int32_t step  = -(int32_t)(span / 10);          // -18° per segment (CCW)

    // 11 white segments
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 1);
    for (int b = 0; b < 11; b++) {
        int32_t a  = (start + b * step + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;
        int32_t sn = sin_lookup(a);
        int32_t cn = cos_lookup(a);
        GPoint inner = { SEC_CENTER.x + trig_round(sn * 28),
                         SEC_CENTER.y - trig_round(cn * 28) };
        GPoint outer = { SEC_CENTER.x + trig_round(sn * 33),
                         SEC_CENTER.y - trig_round(cn * 33) };
        graphics_draw_line(ctx, inner, outer);
    }

    // Labels at "0" (270°), "50" (180°), "100" (90°) — radius 18, inward from segments
    GFont small_font = fonts_get_system_font(FONT_KEY_GOTHIC_09);
    graphics_context_set_text_color(ctx, GColorWhite);
    static const int   label_segs[3]  = {0, 5, 10};
    static const char *label_texts[3] = {"0", "50", "100"};
    for (int i = 0; i < 3; i++) {
        int32_t a  = (start + label_segs[i] * step + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;
        int32_t sn = sin_lookup(a);
        int32_t cn = cos_lookup(a);
        int16_t tx = SEC_CENTER.x + trig_round(sn * 18);
        int16_t ty = SEC_CENTER.y - trig_round(cn * 18);
        graphics_draw_text(ctx, label_texts[i], small_font,
                           GRect(tx - 10, ty - 5, 20, 10),
                           GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }

    // Pointer: 0% → 270°, 100% → 90°, going CCW through bottom
    int32_t bat_angle = (start - (int32_t)pct * span / 100 + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;
    draw_sec_hand(ctx, bat_angle);

    // Battery icon on top of the boss (drawn last in this layer, still below hour/min hands)
    if (s_battery_icon) {
        GSize icon_size = gbitmap_get_bounds(s_battery_icon).size;
        GRect icon_rect = GRect(SEC_CENTER.x - icon_size.w / 2,
                                SEC_CENTER.y - icon_size.h / 2,
                                icon_size.w, icon_size.h);
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, s_battery_icon, icon_rect);
    }
}

// ============================================================================
// SHAKE ANIMATION
// ============================================================================

static void anim_timer_cb(void *context) {
    s_anim_elapsed += ANIM_INTERVAL_MS;
    if (s_anim_elapsed < ANIM_DURATION_MS) {
        s_anim_timer = app_timer_register(ANIM_INTERVAL_MS, anim_timer_cb, NULL);
    } else {
        s_anim_elapsed = ANIM_DURATION_MS;
        s_anim_timer   = NULL;
    }
    if (s_canvas) layer_mark_dirty(s_canvas);
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
    if (s_anim_timer) {
        app_timer_cancel(s_anim_timer);
    }
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    s_anim_hour_start = ((t->tm_hour % 12) * TRIG_MAX_ANGLE / 12) +
                        (t->tm_min  * TRIG_MAX_ANGLE / 12 / 60);
    s_anim_min_start  =  t->tm_min  * TRIG_MAX_ANGLE / 60;
    s_anim_elapsed    = 0;
    s_anim_timer      = app_timer_register(ANIM_INTERVAL_MS, anim_timer_cb, NULL);
}

// ============================================================================
// CANVAS
// ============================================================================

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_antialiased(ctx, true);

    if (s_background) {
        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
        graphics_draw_bitmap_in_rect(ctx, s_background, bounds);
    } else {
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return;

    int32_t hour_angle, min_angle;
    int32_t sec_angle = t->tm_sec * TRIG_MAX_ANGLE / 30;

    if (s_anim_timer || s_anim_elapsed == ANIM_DURATION_MS) {
        // Animated: hour = 1 rev in 6 s, minute = 3 rev in 6 s
        int32_t p = (int32_t)s_anim_elapsed;
        hour_angle = (int32_t)(s_anim_hour_start +
                     (int64_t)p * TRIG_MAX_ANGLE / ANIM_DURATION_MS) % TRIG_MAX_ANGLE;
        min_angle  = (int32_t)(s_anim_min_start  +
                     (int64_t)p * 3 * TRIG_MAX_ANGLE / ANIM_DURATION_MS) % TRIG_MAX_ANGLE;
    } else {
        hour_angle = ((t->tm_hour % 12) * TRIG_MAX_ANGLE / 12) +
                     (t->tm_min  * TRIG_MAX_ANGLE / 12 / 60);
        min_angle  =  t->tm_min  * TRIG_MAX_ANGLE / 60;
    }

    // ── Seconds / battery layer (below hour and minute hands) ────────────────
    if (s_display_seconds) {
        graphics_context_set_stroke_color(ctx, GColorWhite);
        graphics_context_set_stroke_width(ctx, 1);
        for (int i = 0; i < 12; i++) {
            int32_t a = i * TRIG_MAX_ANGLE / 12;
            int32_t s = sin_lookup(a);
            int32_t c = cos_lookup(a);
            GPoint inner = { SEC_CENTER.x + trig_round(s * 28),
                             SEC_CENTER.y - trig_round(c * 28) };
            GPoint outer = { SEC_CENTER.x + trig_round(s * 33),
                             SEC_CENTER.y - trig_round(c * 33) };
            graphics_draw_line(ctx, inner, outer);
        }
        draw_sec_hand(ctx, sec_angle);
    } else {
        draw_battery_display(ctx);
    }

    // ── Hour and minute hands (on top of everything) ─────────────────────────
    draw_hour_hand(ctx, hour_angle);
    draw_min_hand(ctx, min_angle);

    // Center axle dot for hour/minute
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_circle(ctx, s_center, 4);
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_circle(ctx, GPoint(s_center.x - 2, s_center.y - 2), 1);
}

// ============================================================================
// TICK
// ============================================================================

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    if (s_canvas) layer_mark_dirty(s_canvas);
}

// ============================================================================
// WINDOW LIFECYCLE
// ============================================================================

static void window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);
    s_center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

    s_canvas = layer_create(bounds);
    layer_set_update_proc(s_canvas, canvas_update_proc);
    layer_add_child(root, s_canvas);

    s_background   = gbitmap_create_with_resource(RESOURCE_ID_BACKGROUND);
    s_battery_icon = gbitmap_create_with_resource(RESOURCE_ID_BATTERY_ICON);

    s_hour_body = gpath_create(&(GPathInfo){ ARRAY_LENGTH(s_hour_body_pts), s_hour_body_pts });
    s_hour_tip  = gpath_create(&(GPathInfo){ ARRAY_LENGTH(s_hour_tip_pts),  s_hour_tip_pts  });
    s_min_body  = gpath_create(&(GPathInfo){ ARRAY_LENGTH(s_min_body_pts),  s_min_body_pts  });
    s_min_tip   = gpath_create(&(GPathInfo){ ARRAY_LENGTH(s_min_tip_pts),   s_min_tip_pts   });
}

static void window_unload(Window *window) {
    if (s_hour_body)  { gpath_destroy(s_hour_body);  s_hour_body  = NULL; }
    if (s_hour_tip)   { gpath_destroy(s_hour_tip);   s_hour_tip   = NULL; }
    if (s_min_body)   { gpath_destroy(s_min_body);   s_min_body   = NULL; }
    if (s_min_tip)    { gpath_destroy(s_min_tip);    s_min_tip    = NULL; }
    if (s_background)   { gbitmap_destroy(s_background);   s_background   = NULL; }
    if (s_battery_icon) { gbitmap_destroy(s_battery_icon); s_battery_icon = NULL; }
    if (s_canvas)     { layer_destroy(s_canvas);        s_canvas     = NULL; }
}

// ============================================================================
// APP LIFECYCLE
// ============================================================================

static void init(void) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
        .load   = window_load,
        .unload = window_unload
    });
    window_stack_push(s_window, true);
    tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
    accel_tap_service_subscribe(accel_tap_handler);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    accel_tap_service_unsubscribe();
    if (s_anim_timer) { app_timer_cancel(s_anim_timer); s_anim_timer = NULL; }
    if (s_window) { window_destroy(s_window); s_window = NULL; }
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
