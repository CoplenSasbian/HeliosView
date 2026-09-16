#ifndef HELIOSVIEW_HELIOSVIEW_UI_H
#define HELIOSVIEW_HELIOSVIEW_UI_H

/**
 * HeliosView C API -- Lightweight Retained UI Component System
 *
 * Provides a lightweight, canvas-driven UI tree that lives inside a UIHost.
 * Widgets are pure memory objects (no OS HWND per control).
 * Custom widgets can be defined purely through descriptor tables (desc + user_data).
 *
 * Part of the public C ABI; 0 platform dependencies.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_canvas.h>
#include <HeliosView/heliosview_host.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heliosview_ui_widget heliosview_ui_widget_t;
typedef struct heliosview_host_mouse_event heliosview_host_mouse_event_t;

/* ================= Custom Widget Descriptor ================= */

/**
 * Descriptor table defining custom widget behavior.
 */
typedef struct heliosview_ui_widget_desc {
    /**
     * Paint callback: called when the widget needs to be drawn.
     * Coordinate system is relative to widget's own top-left bounds (0, 0).
     */
    void (*paint)(heliosview_ui_widget_t* widget, heliosview_painter_t* painter, void* user_data);

    /**
     * Event callback: called when a mouse event occurs inside the widget.
     * Coordinates in event are relative to the widget's own top-left.
     * Return non-zero if the event was consumed, 0 to allow pass-through.
     */
    int (*event)(heliosview_ui_widget_t* widget, const heliosview_host_mouse_event_t* event, void* user_data);

    /**
     * Measure callback: calculate preferred width and height for layout.
     * May be NULL if widget does not support auto-sizing.
     */
    void (*measure)(heliosview_ui_widget_t* widget, int* preferred_w, int* preferred_h, void* user_data);

    /**
     * Destroy callback: cleans up user_data when the widget is destroyed.
     * May be NULL if no cleanup is needed.
     */
    void (*destroy)(void* user_data);
} heliosview_ui_widget_desc_t;

/* ================= Widget Lifecycle & Hierarchy ================= */

/**
 * Create a custom widget with a behavioral descriptor and user data.
 */
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_ui_widget_create(
    const heliosview_ui_widget_desc_t* desc, void* user_data);

/**
 * Destroy a widget, detaching it from its parent and recursively destroying children.
 */
HELIOSVIEW_API void heliosview_ui_widget_destroy(heliosview_ui_widget_t* widget);

/**
 * Add a child widget to a parent container.
 */
HELIOSVIEW_API void heliosview_ui_widget_add_child(heliosview_ui_widget_t* parent, heliosview_ui_widget_t* child);

/**
 * Remove a child widget from its parent without destroying it.
 */
HELIOSVIEW_API void heliosview_ui_widget_remove_child(heliosview_ui_widget_t* parent, heliosview_ui_widget_t* child);

/**
 * Get the number of child widgets.
 */
HELIOSVIEW_API size_t heliosview_ui_widget_get_child_count(const heliosview_ui_widget_t* widget);

/**
 * Get a child widget at the given index. Returns NULL if out of range.
 */
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_ui_widget_get_child_at(const heliosview_ui_widget_t* widget, size_t index);

/**
 * Retrieve the user_data associated with a widget.
 */
HELIOSVIEW_API void* heliosview_ui_widget_get_userdata(const heliosview_ui_widget_t* widget);

/* ================= Geometry, State & Layout ================= */

/**
 * Set bounds (position and size) relative to the parent widget's client area.
 */
HELIOSVIEW_API void heliosview_ui_widget_set_bounds(heliosview_ui_widget_t* widget, int x, int y, int width, int height);
HELIOSVIEW_API void heliosview_ui_widget_get_bounds(const heliosview_ui_widget_t* widget, int* x, int* y, int* width, int* height);

/**
 * Set widget visibility (1 = visible, 0 = hidden).
 */
HELIOSVIEW_API void heliosview_ui_widget_set_visible(heliosview_ui_widget_t* widget, int visible);
HELIOSVIEW_API int heliosview_ui_widget_is_visible(const heliosview_ui_widget_t* widget);

/**
 * Request a repaint for this widget and its subtree.
 */
HELIOSVIEW_API void heliosview_ui_widget_request_repaint(heliosview_ui_widget_t* widget);

/**
 * Trigger layout calculation for container widgets.
 */
HELIOSVIEW_API void heliosview_ui_widget_layout(heliosview_ui_widget_t* widget);

/* ================= Host Integration ================= */

/**
 * Attach a root widget to a UIHost.
 * The UIHost will automatically route paints, resizes, and mouse events to the widget tree.
 */
HELIOSVIEW_API void heliosview_host_ui_set_root(heliosview_host_t* host, heliosview_ui_widget_t* root_widget);

/**
 * Get the root widget attached to a UIHost (or NULL if none).
 */
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_host_ui_get_root(heliosview_host_t* host);

/* ================= Built-in Basic Widgets (C API) ================= */

/* ---- Label Widget ---- */
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_ui_label_create(const char* text);
HELIOSVIEW_API void heliosview_ui_label_set_text(heliosview_ui_widget_t* label, const char* text);
HELIOSVIEW_API void heliosview_ui_label_set_color(heliosview_ui_widget_t* label, uint32_t argb);
HELIOSVIEW_API void heliosview_ui_label_set_font_size(heliosview_ui_widget_t* label, float font_size);

/* ---- Button Widget ---- */
typedef void (*heliosview_ui_click_cb)(heliosview_ui_widget_t* button, void* user_data);
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_ui_button_create(
    const char* label, heliosview_ui_click_cb on_click, void* user_data);
HELIOSVIEW_API void heliosview_ui_button_set_label(heliosview_ui_widget_t* button, const char* label);
HELIOSVIEW_API void heliosview_ui_button_set_color(heliosview_ui_widget_t* button, uint32_t bg_color, uint32_t text_color);

/* ---- Layout Containers ---- */
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_ui_vstack_create(int spacing, int padding);
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_ui_hstack_create(int spacing, int padding);

#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_UI_H */
