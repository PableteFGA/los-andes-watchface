#include <pebble.h>

// Three hands — same design language: tapered shaft, circular boss, red tip, tail.
//
//   Hour:    boss R=10, tip y=-80  (cutoff y=-72),  center = s_center
//   Minute:  boss R=8,  tip y=-119 (cutoff y=-109), center = s_center
//   Seconds: boss R=3,  tip y=-34  (cutoff y=-31),  center = (129,184)
//            40 px total (tip to tail). Boss: white, NO outline.

static Window  *s_window;
static Layer   *s_canvas;
static GPoint   s_center;
static GBitmap *s_background;

// Fixed pivot for the seconds hand (not the watch center)
static const GPoint SEC_CENTER = {130, 184};

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

// Convenience: rotate around the main watch center
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

// segs[]: pairs of {from, to} points; boss arcs drawn instead of full circle.
// boss_half_ang = asin(shaft_half_width / boss_r) in TRIG units.
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

    GRect br = GRect(s_center.x - boss_r, s_center.y - boss_r,
                     2 * boss_r, 2 * boss_r);
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
    fill_hand(ctx, angle, s_hour_body, s_hour_tip, 10);
    outline_hand(ctx, angle, 10, 17 * TRIG_MAX_ANGLE / 360, segs, ARRAY_LENGTH(segs));
}

static void draw_min_hand(GContext *ctx, int32_t angle) {
    static const GPoint segs[] = {
        { 3, -7}, { 5,-109},  {-5,-109},{-3,  -7},
        {-3,  7}, {-3,  22},  {-3, 22}, { 3,  22},  { 3, 22}, { 3,  7},
    };
    fill_hand(ctx, angle, s_min_body, s_min_tip, 8);
    outline_hand(ctx, angle, 8, 22 * TRIG_MAX_ANGLE / 360, segs, ARRAY_LENGTH(segs));
}

// ── Seconds hand ────────────────────────────────────────────────────────────

static void draw_sec_hand(GContext *ctx, int32_t angle) {
    GPoint cw  = rotate_at(GPoint(0,   9), angle, SEC_CENTER);
    GPoint mid = rotate_at(GPoint(0, -30), angle, SEC_CENTER);
    GPoint tip = rotate_at(GPoint(0, -34), angle, SEC_CENTER);

    // Outline pass (light gray, +2px wider)
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

    int32_t hour_angle = ((t->tm_hour % 12) * TRIG_MAX_ANGLE / 12) +
                         (t->tm_min  * TRIG_MAX_ANGLE / 12 / 60);
    int32_t min_angle  =  t->tm_min  * TRIG_MAX_ANGLE / 60;
    int32_t sec_angle  =  t->tm_sec  * TRIG_MAX_ANGLE / 60;

    // Sub-seconds dial tick marks — 12 radial lines around the seconds face
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

    draw_hour_hand(ctx, hour_angle);
    draw_min_hand(ctx, min_angle);
    draw_sec_hand(ctx, sec_angle);

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

    s_background = gbitmap_create_with_resource(RESOURCE_ID_BACKGROUND);

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
    if (s_background) { gbitmap_destroy(s_background); s_background = NULL; }
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
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    if (s_window) { window_destroy(s_window); s_window = NULL; }
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
