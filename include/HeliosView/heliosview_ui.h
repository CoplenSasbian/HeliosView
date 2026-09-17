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

/* ================= Keyboard Focus & Text Input =================
 *
 * A widget tree only ever received mouse events; keyboard input (and with it IME
 * text) had nowhere to go. These calls add the missing half:
 *
 *   - a text and/or key callback per widget, set after creation so the widget
 *     descriptor stays ABI-stable,
 *   - focus, owned by the host's UI binding (set on mouse down, or requested by an
 *     application with heliosview_host_ui_set_focus),
 *   - heliosview_host_ui_dispatch_key / _text, which an application's message loop
 *     calls with the KEY_DOWN / TEXT_INPUT events it polls, so the focused widget
 *     gets them. C++ applications get this for free: helios::App::exec dispatches
 *     those events to the window, which forwards them to its UI hosts.
 *
 * Only the focused widget receives a routed event; nothing is propagated to
 * ancestors. A widget that wants keys but not text leaves the text callback NULL.
 */

/**
 * Called on the focused widget for TEXT_INPUT: UTF-8 text at its local coordinates,
 * IME commit strings included (an IME sends its result as WM_CHAR, so this is the
 * same path as typed text). Return non-zero when the widget consumed the text.
 */
typedef int (*heliosview_ui_text_cb)(heliosview_ui_widget_t* widget, const char* utf8, void* user_data);

/**
 * Called on the focused widget for KEY_DOWN / KEY_UP. `key` is a heliosview_keycode_t
 * and `modifiers` the HELIOSVIEW_MOD_* bits held. Return non-zero when consumed.
 */
typedef int (*heliosview_ui_key_cb)(heliosview_ui_widget_t* widget, int key, uint32_t modifiers, int is_down, void* user_data);

/** Set (or clear, with NULL) the focused-text callback of a widget. */
HELIOSVIEW_API void heliosview_ui_widget_set_text_callback(heliosview_ui_widget_t* widget, heliosview_ui_text_cb callback);

/** Set (or clear, with NULL) the key callback of a widget. */
HELIOSVIEW_API void heliosview_ui_widget_set_key_callback(heliosview_ui_widget_t* widget, heliosview_ui_key_cb callback);

/**
 * Called on the focused widget while an IME is composing (before the text is
 * committed): `utf8` is the current composition string, empty when composition
 * ended. A widget draws it as "not yet text" (typically underlined) and must NOT
 * append it to its value -- the committed characters arrive afterwards through the
 * text callback.
 */
typedef int (*heliosview_ui_composition_cb)(heliosview_ui_widget_t* widget, const char* utf8, void* user_data);

/** Set (or clear, with NULL) the IME-composition callback of a widget. */
HELIOSVIEW_API void heliosview_ui_widget_set_composition_callback(heliosview_ui_widget_t* widget, heliosview_ui_composition_cb callback);

/** Whether the widget can take keyboard focus (has a text or key callback). */
HELIOSVIEW_API int heliosview_ui_widget_is_focusable(const heliosview_ui_widget_t* widget);

/** The widget that currently has keyboard focus on this host, or NULL. */
HELIOSVIEW_API heliosview_ui_widget_t* heliosview_host_ui_get_focus(heliosview_host_t* host);

/**
 * Give keyboard focus to `widget` (NULL clears it). The widget must belong to the
 * host's tree and be focusable; the host repaints both the old and the new focus.
 */
HELIOSVIEW_API void heliosview_host_ui_set_focus(heliosview_host_t* host, heliosview_ui_widget_t* widget);

/**
 * Deliver a TEXT_INPUT to the focused widget. Returns non-zero when a widget
 * consumed it.
 */
HELIOSVIEW_API int heliosview_host_ui_dispatch_text(heliosview_host_t* host, const char* utf8);

/**
 * Deliver a KEY_DOWN / KEY_UP to the focused widget (is_down != 0 for a press).
 * Returns non-zero when a widget consumed it.
 */
HELIOSVIEW_API int heliosview_host_ui_dispatch_key(heliosview_host_t* host, int key, uint32_t modifiers, int is_down);

/**
 * Deliver an IME composition update to the focused widget ("" when composition
 * ended). Returns non-zero when a widget consumed it.
 */
HELIOSVIEW_API int heliosview_host_ui_dispatch_composition(heliosview_host_t* host, const char* utf8);

/**
 * Tell the OS IME where the caret is, so its composition window and candidate list
 * appear next to it instead of at the default corner. Coordinates are in the host's
 * own client space (the widget's local point plus the widget's offset in the tree is
 * what an application passes; a widget with no offset of its own passes its local
 * caret box). Safe to call for every composition update, and a no-op when the host
 * has no window.
 */
HELIOSVIEW_API void heliosview_host_ui_set_ime_caret(heliosview_host_t* host, int x, int y, int height);

/**
 * The same, for a widget that only knows its own local coordinates: the caret is
 * translated through the widget's ancestry to the host's client space, and the host
 * is found from the tree the widget belongs to. `line_height` is the caret height in
 * pixels (<= 0 means a default). A widget calls this whenever the caret moves while
 * it has focus -- that is what keeps the IME candidate window next to the text being
 * composed. No-op when the widget is not attached to a host.
 */
HELIOSVIEW_API void heliosview_ui_widget_report_ime_caret(heliosview_ui_widget_t* widget, int local_x, int local_y, float line_height);

/**
 * Whether the widget's tree is the one currently attached to a host (see
 * heliosview_host_ui_set_root). A detached tree can be kept and re-attached; this is
 * how a widget knows its host lookups would fail.
 */
HELIOSVIEW_API int heliosview_ui_widget_is_attached(const heliosview_ui_widget_t* widget);

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

/**
 * Clear and free any UI binding associated with a UIHost.
 */
HELIOSVIEW_API void heliosview_host_ui_clear_binding(heliosview_host_t* host);

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
