/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

// NEW: state for the peripheral's (right-hand side's) battery level, fetched
// over the split BLE link. `valid` distinguishes "a real event arrived" from
// "nothing received yet" -- there is no zmk_battery_state_of_charge()-style
// getter for a cached peripheral level, so on the very first call there is
// nothing to report yet.
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
struct peripheral_battery_status_state {
    uint8_t source;
    uint8_t level;
    bool valid;
};
#endif

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw battery
    draw_battery(canvas, state);

    // Draw output status
    char output_text[10] = {};

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                strcat(output_text, LV_SYMBOL_WIFI);
            } else {
                strcat(output_text, LV_SYMBOL_CLOSE);
            }
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        break;
    }

    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc, output_text);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_arc_dsc_t arc_dsc;
    init_arc_dsc(&arc_dsc, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc_dsc_filled;
    init_arc_dsc(&arc_dsc_filled, LVGL_FOREGROUND, 9);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_dsc_black;
    init_label_dsc(&label_dsc_black, LVGL_BACKGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw circles
    int circle_offsets[5][2] = {
        {13, 13}, {55, 13}, {34, 34}, {13, 55}, {55, 55},
    };

    for (int i = 0; i < 5; i++) {
        bool selected = i == state->active_profile_index;

        lv_canvas_draw_arc(canvas, circle_offsets[i][0], circle_offsets[i][1], 13, 0, 360,
                           &arc_dsc);

        if (selected) {
            lv_canvas_draw_arc(canvas, circle_offsets[i][0], circle_offsets[i][1], 9, 0, 359,
                               &arc_dsc_filled);
        }

        char label[2];
        snprintf(label, sizeof(label), "%d", i + 1);
        lv_canvas_draw_text(canvas, circle_offsets[i][0] - 8, circle_offsets[i][1] - 10, 16,
                            (selected ? &label_dsc_black : &label_dsc), label);
    }

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);


    // TEMP DIAGNOSTIC — replacing the layer text with the raw peripheral
    // battery state, at a position we already know renders correctly.
    char text[20] = {};
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    snprintf(text, sizeof(text), "R%d C%d", state->peripheral_battery,
             state->peripheral_connected);
#else
    snprintf(text, sizeof(text), "NO FETCH");
#endif
    lv_canvas_draw_text(canvas, 0, 5, 68, &label_dsc, text);

    

// NEW: peripheral battery, drawn underneath the layer name where this
// canvas has plenty of free vertical space (the layer text is a single
// line near the top). Positioning is a first estimate since it can't be
// previewed here -- move the y-coordinate (currently 30) if it lands too
// close to the layer text for your taste.
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    lv_draw_label_dsc_t label_dsc_periph;
    init_label_dsc(&label_dsc_periph, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);

    char periph_text[16] = {};
    if (!state->peripheral_connected) {
        snprintf(periph_text, sizeof(periph_text), "R --");
    } else {
        snprintf(periph_text, sizeof(periph_text), "R %d%%", state->peripheral_battery);
    }
    lv_canvas_draw_text(canvas, 0, 30, 68, &label_dsc_periph, periph_text);
#endif

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

// Peripheral battery listener -- NOT built with ZMK_DISPLAY_WIDGET_LISTENER.
// Root cause found by reading ZMK core's actual macro (app/include/zmk/display.h):
// the macro's generated event callback only records a new value if
// zmk_display_is_initialized() is *already* true at the moment the event
// arrives -- otherwise it's silently dropped, no queue, no retry. The local
// battery widget never notices this because battery_status_get_state() (above)
// has zmk_battery_state_of_charge() as a direct fallback it can call again at
// any time. There is no equivalent getter for a peripheral's level -- only
// this event -- so if the peripheral's battery report (which arrives once
// split BLE finishes connecting and reading it, shortly after boot) happens
// to land before the display finishes initializing, it's gone for good until
// the level next changes, which for a slowly-draining battery can be hours.
// This was confirmed indirectly: CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY
// exposes the same event correctly to the host (via central_bas_proxy.c's own
// raw ZMK_LISTENER, which has no such gate) -- proof the event itself fires
// fine, and the bug was specifically in how this widget was consuming it.
//
// Fix: capture every event unconditionally into a plain cache, then submit
// the actual (LVGL-touching) redraw to zmk_display_work_q() -- the same
// queue the macro uses, so thread-safety is preserved -- and if the display
// isn't marked ready yet when that redraw work runs, reschedule itself
// briefly instead of giving up. In practice this resolves on the very first
// attempt: zmk_display_init() starts that work queue and enqueues its own
// init work on it before any BLE peripheral connection could possibly
// complete, so this redraw work almost always ends up queued (FIFO) behind
// that init work and only runs once it's done. The retry is just a safety
// net for the rare case the display device itself isn't ready yet.
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

static struct peripheral_battery_status_state cached_peripheral_state = {.valid = false};

static void set_peripheral_battery_status(struct zmk_widget_status *widget,
                                          struct peripheral_battery_status_state state) {
    // Single peripheral (source 0) on a 2-piece split. If you ever add a
    // dongle/third part, extend this to track each source separately.
    if (!state.valid || state.source != 0) {
        return;
    }

    widget->state.peripheral_battery = state.level;
    widget->state.peripheral_connected = true;

    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void peripheral_battery_draw_work_cb(struct k_work *work) {
    if (!cached_peripheral_state.valid) {
        return;
    }

    if (!zmk_display_is_initialized()) {
        k_work_reschedule_for_queue(zmk_display_work_q(),
                                    k_work_delayable_from_work(work), K_MSEC(500));
        return;
    }

    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_peripheral_battery_status(widget, cached_peripheral_state);
    }
}

K_WORK_DELAYABLE_DEFINE(peripheral_battery_draw_work, peripheral_battery_draw_work_cb);

static int peripheral_battery_raw_listener_cb(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    cached_peripheral_state = (struct peripheral_battery_status_state){
        .source = ev->source,
        .level = ev->state_of_charge,
        .valid = true,
    };

    k_work_reschedule_for_queue(zmk_display_work_q(), &peripheral_battery_draw_work, K_NO_WAIT);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(peripheral_battery_raw_listener, peripheral_battery_raw_listener_cb);
ZMK_SUBSCRIPTION(peripheral_battery_raw_listener, zmk_peripheral_battery_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) */

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;

    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

#ifdef CONFIG_NICE_VIEW_DISP_ROTATE_180 // sets positions for default and flipped canvases
int top_pos = 0;
int middle_pos = 68;
int bottom_pos = 136;
#else
int top_pos = 92;
int middle_pos = 24;
int bottom_pos = -44;
#endif

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, top_pos, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, middle_pos, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, bottom_pos, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
