// HeliosView.dll -- Retained UI Component Engine (Core C ABI Implementation)

#include <HeliosView/heliosview_ui.h>
#include "heliosview_internal.h"

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

struct heliosview_ui_widget {
    heliosview_ui_widget_desc_t desc{};
    void* user_data = nullptr;

    heliosview_ui_widget* parent = nullptr;
    std::vector<heliosview_ui_widget*> children;
    heliosview_host_t* host_attached = nullptr;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool visible = true;
    bool hovered = false;
    bool pressed = false;

    // Stack container layout configuration
    bool is_stack = false;
    bool is_vertical_stack = true;
    int stack_spacing = 8;
    int stack_padding = 10;

    heliosview_host_t* get_host() {
        if (host_attached) return host_attached;
        if (parent) return parent->get_host();
        return nullptr;
    }

    void request_repaint() {
        heliosview_host_t* h = get_host();
        if (h) {
            heliosview_host_ui_request_repaint(h);
        }
    }

    heliosview_ui_widget* hit_test(int px, int py, int* out_local_x, int* out_local_y) {
        if (!visible) return nullptr;
        if (px < x || px >= x + width || py < y || py >= y + height) return nullptr;

        int local_x = px - x;
        int local_y = py - y;

        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            heliosview_ui_widget* child = *it;
            heliosview_ui_widget* hit = child->hit_test(local_x, local_y, out_local_x, out_local_y);
            if (hit) return hit;
        }

        if (out_local_x) *out_local_x = local_x;
        if (out_local_y) *out_local_y = local_y;
        return this;
    }

    void screen_to_local(int root_x, int root_y, int* out_local_x, int* out_local_y) const {
        int off_x = 0;
        int off_y = 0;
        const heliosview_ui_widget* curr = this;
        while (curr) {
            off_x += curr->x;
            off_y += curr->y;
            curr = curr->parent;
        }
        if (out_local_x) *out_local_x = root_x - off_x;
        if (out_local_y) *out_local_y = root_y - off_y;
    }

    void render_tree(heliosview_painter_t* p, int /*origin_x*/, int /*origin_y*/) {
        if (!visible) return;

        heliosview_painter_save(p);
        heliosview_painter_translate(p, (float)x, (float)y);

        if (desc.paint) {
            desc.paint(this, p, user_data);
        }

        for (auto* child : children) {
            child->render_tree(p, 0, 0);
        }

        heliosview_painter_restore(p);
    }
};

struct HostUiBinding {
    heliosview_ui_widget_t* root = nullptr;
    heliosview_ui_widget_t* hovered_widget = nullptr;
    heliosview_ui_widget_t* pressed_widget = nullptr;
};

static thread_local std::vector<std::pair<heliosview_host_t*, HostUiBinding*>> s_bindings;

// ================= Widget Core API =================

heliosview_ui_widget_t* heliosview_ui_widget_create(
    const heliosview_ui_widget_desc_t* desc, void* user_data) {
    heliosview_ui_widget* w = nullptr;
    try {
        w = hv::hv_alloc<heliosview_ui_widget>();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    if (desc) w->desc = *desc;
    w->user_data = user_data;
    w->visible = true;
    return w;
}

void heliosview_ui_widget_destroy(heliosview_ui_widget_t* widget) {
    if (!widget) return;

    for (auto& pair : s_bindings) {
        if (pair.second) {
            if (pair.second->root == widget) pair.second->root = nullptr;
            if (pair.second->hovered_widget == widget) pair.second->hovered_widget = nullptr;
            if (pair.second->pressed_widget == widget) pair.second->pressed_widget = nullptr;
        }
    }
    widget->host_attached = nullptr;

    if (widget->parent) {
        auto& sibs = widget->parent->children;
        sibs.erase(std::remove(sibs.begin(), sibs.end(), widget), sibs.end());
        widget->parent = nullptr;
    }

    for (auto* child : widget->children) {
        child->parent = nullptr;
        heliosview_ui_widget_destroy(child);
    }
    widget->children.clear();

    if (widget->desc.destroy && widget->user_data) {
        widget->desc.destroy(widget->user_data);
        widget->user_data = nullptr;
    }

    hv::hv_dealloc(widget);
}

void heliosview_ui_widget_add_child(heliosview_ui_widget_t* parent, heliosview_ui_widget_t* child) {
    if (!parent || !child || child->parent == parent) return;

    if (child->parent) {
        heliosview_ui_widget_remove_child(child->parent, child);
    }

    child->parent = parent;
    parent->children.push_back(child);
    parent->request_repaint();
}

void heliosview_ui_widget_remove_child(heliosview_ui_widget_t* parent, heliosview_ui_widget_t* child) {
    if (!parent || !child || child->parent != parent) return;

    auto& sibs = parent->children;
    sibs.erase(std::remove(sibs.begin(), sibs.end(), child), sibs.end());
    child->parent = nullptr;
    parent->request_repaint();
}

size_t heliosview_ui_widget_get_child_count(const heliosview_ui_widget_t* widget) {
    return widget ? widget->children.size() : 0;
}

heliosview_ui_widget_t* heliosview_ui_widget_get_child_at(const heliosview_ui_widget_t* widget, size_t index) {
    if (!widget || index >= widget->children.size()) return nullptr;
    return widget->children[index];
}

void* heliosview_ui_widget_get_userdata(const heliosview_ui_widget_t* widget) {
    return widget ? widget->user_data : nullptr;
}

void heliosview_ui_widget_set_bounds(heliosview_ui_widget_t* widget, int x, int y, int width, int height) {
    if (!widget) return;
    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;
    widget->request_repaint();
}

void heliosview_ui_widget_get_bounds(const heliosview_ui_widget_t* widget, int* x, int* y, int* width, int* height) {
    if (!widget) return;
    if (x) *x = widget->x;
    if (y) *y = widget->y;
    if (width) *width = widget->width;
    if (height) *height = widget->height;
}

void heliosview_ui_widget_set_visible(heliosview_ui_widget_t* widget, int visible) {
    if (!widget) return;
    widget->visible = (visible != 0);
    widget->request_repaint();
}

int heliosview_ui_widget_is_visible(const heliosview_ui_widget_t* widget) {
    return (widget && widget->visible) ? 1 : 0;
}

void heliosview_ui_widget_request_repaint(heliosview_ui_widget_t* widget) {
    if (widget) widget->request_repaint();
}

// ================= Host Integration =================

static void HostPaintCallback(heliosview_host_t* host, heliosview_painter_t* painter, void* userdata) {
    auto* binding = static_cast<HostUiBinding*>(userdata);
    if (!binding || !binding->root) return;

    // Clear background with dark theme color
    heliosview_painter_clear(painter, 0xFF1E1E2E);

    int w = 0, h = 0;
    heliosview_host_get_bounds(host, nullptr, nullptr, &w, &h);
    binding->root->width = w;
    binding->root->height = h;
    heliosview_ui_widget_layout(binding->root);

    binding->root->render_tree(painter, 0, 0);
}

static void HostMouseCallback(heliosview_host_t* /*host*/, const heliosview_host_mouse_event_t* evt, void* userdata) {
    auto* binding = static_cast<HostUiBinding*>(userdata);
    if (!binding || !binding->root) return;

    if (evt->action == HELIOSVIEW_HOST_MOUSE_DOWN) {
        int lx = 0, ly = 0;
        heliosview_ui_widget_t* hit = binding->root->hit_test(evt->x, evt->y, &lx, &ly);
        binding->pressed_widget = hit;
        if (hit && hit->desc.event) {
            heliosview_host_mouse_event_t local_evt = *evt;
            local_evt.x = lx;
            local_evt.y = ly;
            hit->desc.event(hit, &local_evt, hit->user_data);
        }
        return;
    }

    if (evt->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
        if (binding->pressed_widget) {
            // Mouse drag in progress: route move event to the pressed widget even outside its bounds
            int lx = 0, ly = 0;
            binding->pressed_widget->screen_to_local(evt->x, evt->y, &lx, &ly);
            if (binding->pressed_widget->desc.event) {
                heliosview_host_mouse_event_t local_evt = *evt;
                local_evt.x = lx;
                local_evt.y = ly;
                binding->pressed_widget->desc.event(binding->pressed_widget, &local_evt, binding->pressed_widget->user_data);
            }
            return;
        }

        // Standard hover & mouse move
        int lx = 0, ly = 0;
        heliosview_ui_widget_t* hit = binding->root->hit_test(evt->x, evt->y, &lx, &ly);
        if (hit != binding->hovered_widget) {
            if (binding->hovered_widget) {
                binding->hovered_widget->hovered = false;
                binding->hovered_widget->request_repaint();
                if (binding->hovered_widget->desc.event) {
                    heliosview_host_mouse_event_t leave_evt = *evt;
                    leave_evt.action = HELIOSVIEW_HOST_MOUSE_LEAVE;
                    binding->hovered_widget->desc.event(binding->hovered_widget, &leave_evt, binding->hovered_widget->user_data);
                }
            }
            if (hit) {
                hit->hovered = true;
                hit->request_repaint();
            }
            binding->hovered_widget = hit;
        }

        if (hit && hit->desc.event) {
            heliosview_host_mouse_event_t local_evt = *evt;
            local_evt.x = lx;
            local_evt.y = ly;
            hit->desc.event(hit, &local_evt, hit->user_data);
        }
        return;
    }

    if (evt->action == HELIOSVIEW_HOST_MOUSE_UP) {
        heliosview_ui_widget_t* target = binding->pressed_widget;
        binding->pressed_widget = nullptr;

        int lx = 0, ly = 0;
        if (target) {
            target->screen_to_local(evt->x, evt->y, &lx, &ly);
            if (target->desc.event) {
                heliosview_host_mouse_event_t local_evt = *evt;
                local_evt.x = lx;
                local_evt.y = ly;
                target->desc.event(target, &local_evt, target->user_data);
            }
        } else {
            heliosview_ui_widget_t* hit = binding->root->hit_test(evt->x, evt->y, &lx, &ly);
            if (hit && hit->desc.event) {
                heliosview_host_mouse_event_t local_evt = *evt;
                local_evt.x = lx;
                local_evt.y = ly;
                hit->desc.event(hit, &local_evt, hit->user_data);
            }
        }

        // Re-evaluate hover target after mouse release
        int hx = 0, hy = 0;
        heliosview_ui_widget_t* cur_hit = binding->root->hit_test(evt->x, evt->y, &hx, &hy);
        if (cur_hit != binding->hovered_widget) {
            if (binding->hovered_widget) {
                binding->hovered_widget->hovered = false;
                binding->hovered_widget->request_repaint();
                if (binding->hovered_widget->desc.event) {
                    heliosview_host_mouse_event_t leave_evt = *evt;
                    leave_evt.action = HELIOSVIEW_HOST_MOUSE_LEAVE;
                    binding->hovered_widget->desc.event(binding->hovered_widget, &leave_evt, binding->hovered_widget->user_data);
                }
            }
            if (cur_hit) {
                cur_hit->hovered = true;
                cur_hit->request_repaint();
            }
            binding->hovered_widget = cur_hit;
        }
        return;
    }

    if (evt->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
        if (!binding->pressed_widget) {
            if (binding->hovered_widget) {
                binding->hovered_widget->hovered = false;
                binding->hovered_widget->pressed = false;
                binding->hovered_widget->request_repaint();
                if (binding->hovered_widget->desc.event) {
                    heliosview_host_mouse_event_t leave_evt = *evt;
                    binding->hovered_widget->desc.event(binding->hovered_widget, &leave_evt, binding->hovered_widget->user_data);
                }
                binding->hovered_widget = nullptr;
            }
        }
        return;
    }
}

void heliosview_host_ui_set_root(heliosview_host_t* host, heliosview_ui_widget_t* root_widget) {
    if (!host) return;

    HostUiBinding* binding = nullptr;
    for (auto& pair : s_bindings) {
        if (pair.first == host) {
            binding = pair.second;
            break;
        }
    }

    if (!binding) {
        if (!root_widget) return;
        try {
            binding = hv::hv_alloc<HostUiBinding>();
        } catch (const std::bad_alloc&) {
            return;
        }
        s_bindings.push_back({host, binding});
        heliosview_host_ui_set_paint_callback(host, HostPaintCallback, binding);
        heliosview_host_ui_set_mouse_callback(host, HostMouseCallback, binding);
    }

    binding->root = root_widget;
    if (root_widget) {
        root_widget->host_attached = host;
        heliosview_host_ui_request_repaint(host);
    }
}

heliosview_ui_widget_t* heliosview_host_ui_get_root(heliosview_host_t* host) {
    if (!host) return nullptr;
    for (const auto& pair : s_bindings) {
        if (pair.first == host && pair.second) {
            return pair.second->root;
        }
    }
    return nullptr;
}

void heliosview_host_ui_clear_binding(heliosview_host_t* host) {
    if (!host) return;
    for (auto it = s_bindings.begin(); it != s_bindings.end(); ++it) {
        if (it->first == host) {
            if (it->second) {
                hv::hv_dealloc(it->second);
            }
            s_bindings.erase(it);
            break;
        }
    }
}

// ================= Built-in Label Widget =================

struct LabelData {
    std::string text;
    uint32_t color = 0xFFFFFFFF;
    float font_size = 14.0f;
};

static void LabelPaint(heliosview_ui_widget_t* /*w*/, heliosview_painter_t* p, void* udata) {
    auto* d = static_cast<LabelData*>(udata);
    if (!d || d->text.empty()) return;

    heliosview_font_desc_t font{"Segoe UI", d->font_size, 0};
    heliosview_painter_set_font(p, &font);
    heliosview_painter_set_fill(p, d->color);
    heliosview_painter_draw_text(p, d->text.c_str(), 0, 0);
}

static void LabelDestroy(void* udata) {
    auto* d = static_cast<LabelData*>(udata);
    hv::hv_dealloc(d);
}

heliosview_ui_widget_t* heliosview_ui_label_create(const char* text) {
    LabelData* d = nullptr;
    try {
        d = hv::hv_alloc<LabelData>();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    if (text) d->text = text;

    heliosview_ui_widget_desc_t desc{};
    desc.paint = LabelPaint;
    desc.destroy = LabelDestroy;

    auto* w = heliosview_ui_widget_create(&desc, d);
    if (!w) {
        hv::hv_dealloc(d);
        return nullptr;
    }
    w->width = 120;
    w->height = 24;
    return w;
}

void heliosview_ui_label_set_text(heliosview_ui_widget_t* label, const char* text) {
    if (!label) return;
    auto* d = static_cast<LabelData*>(label->user_data);
    if (d) {
        d->text = text ? text : "";
        label->request_repaint();
    }
}

void heliosview_ui_label_set_color(heliosview_ui_widget_t* label, uint32_t argb) {
    if (!label) return;
    auto* d = static_cast<LabelData*>(label->user_data);
    if (d) {
        d->color = argb;
        label->request_repaint();
    }
}

void heliosview_ui_label_set_font_size(heliosview_ui_widget_t* label, float font_size) {
    if (!label) return;
    auto* d = static_cast<LabelData*>(label->user_data);
    if (d) {
        d->font_size = font_size;
        label->request_repaint();
    }
}

// ================= Built-in Button Widget =================

struct ButtonData {
    std::string label;
    uint32_t bg_color = 0xFF313244;
    uint32_t text_color = 0xFFCDD6F4;
    heliosview_ui_click_cb on_click = nullptr;
    void* click_udata = nullptr;
};

static void ButtonPaint(heliosview_ui_widget_t* w, heliosview_painter_t* p, void* udata) {
    auto* d = static_cast<ButtonData*>(udata);
    if (!d) return;

    uint32_t bg = d->bg_color;
    if (w->pressed) bg = 0xFF585B70;
    else if (w->hovered) bg = 0xFF45475A;

    heliosview_painter_set_fill(p, bg);
    heliosview_painter_set_stroke(p, 0xFF585B70, 1.0f);
    heliosview_painter_draw_round_rect(p, 0, 0, (float)w->width, (float)w->height, 6.0f);

    if (!d->label.empty()) {
        heliosview_font_desc_t font{"Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &font);
        heliosview_painter_set_fill(p, d->text_color);

        heliosview_text_metrics_t m{};
        heliosview_painter_measure_text(p, d->label.c_str(), &m);
        float tx = (w->width - m.width) / 2.0f;
        float ty = (w->height - m.height) / 2.0f;
        heliosview_painter_draw_text(p, d->label.c_str(), tx, ty);
    }
}

static int ButtonEvent(heliosview_ui_widget_t* w, const heliosview_host_mouse_event_t* e, void* udata) {
    auto* d = static_cast<ButtonData*>(udata);
    if (!d) return 0;

    if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN && e->button == 1) {
        w->pressed = true;
        w->request_repaint();
        return 1;
    } else if (e->action == HELIOSVIEW_HOST_MOUSE_UP && e->button == 1) {
        if (w->pressed) {
            w->pressed = false;
            w->request_repaint();
            // Only fire click if release happened inside button boundaries
            if (e->x >= 0 && e->x < w->width && e->y >= 0 && e->y < w->height) {
                if (d->on_click) {
                    d->on_click(w, d->click_udata);
                }
            }
            return 1;
        }
    }
    return 0;
}

static void ButtonDestroy(void* udata) {
    auto* d = static_cast<ButtonData*>(udata);
    hv::hv_dealloc(d);
}

heliosview_ui_widget_t* heliosview_ui_button_create(
    const char* label, heliosview_ui_click_cb on_click, void* user_data) {
    ButtonData* d = nullptr;
    try {
        d = hv::hv_alloc<ButtonData>();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    if (label) d->label = label;
    d->on_click = on_click;
    d->click_udata = user_data;

    heliosview_ui_widget_desc_t desc{};
    desc.paint = ButtonPaint;
    desc.event = ButtonEvent;
    desc.destroy = ButtonDestroy;

    auto* w = heliosview_ui_widget_create(&desc, d);
    if (!w) {
        hv::hv_dealloc(d);
        return nullptr;
    }
    w->width = 110;
    w->height = 38;
    return w;
}

void heliosview_ui_button_set_label(heliosview_ui_widget_t* button, const char* label) {
    if (!button) return;
    auto* d = static_cast<ButtonData*>(button->user_data);
    if (d) {
        d->label = label ? label : "";
        button->request_repaint();
    }
}

void heliosview_ui_button_set_color(heliosview_ui_widget_t* button, uint32_t bg_color, uint32_t text_color) {
    if (!button) return;
    auto* d = static_cast<ButtonData*>(button->user_data);
    if (d) {
        d->bg_color = bg_color;
        d->text_color = text_color;
        button->request_repaint();
    }
}

// ================= Built-in Layout Containers =================

void heliosview_ui_widget_layout(heliosview_ui_widget_t* widget) {
    if (!widget) return;
    if (!widget->is_stack) {
        // If not a stack, still lay out its children
        for (auto* child : widget->children) {
            heliosview_ui_widget_layout(child);
        }
        return;
    }

    // First, recursively lay out all children so any nested stacks compute their sizes
    for (auto* child : widget->children) {
        heliosview_ui_widget_layout(child);
    }

    if (widget->is_vertical_stack) {
        int cy = widget->stack_padding;
        int max_w = 0;
        for (auto* child : widget->children) {
            if (!child->visible) continue;
            child->x = widget->stack_padding;
            child->y = cy;
            cy += child->height + widget->stack_spacing;
            if (child->width > max_w) max_w = child->width;
        }
        // If this stack has no fixed size or is a nested container, size to content
        int total_h = (cy > widget->stack_padding) ? (cy - widget->stack_spacing + widget->stack_padding) : (widget->stack_padding * 2);
        int total_w = max_w + widget->stack_padding * 2;
        if (widget->parent != nullptr) {
            widget->width = total_w;
            widget->height = total_h;
        }
    } else {
        int cx = widget->stack_padding;
        int max_h = 0;
        for (auto* child : widget->children) {
            if (!child->visible) continue;
            child->x = cx;
            child->y = widget->stack_padding;
            cx += child->width + widget->stack_spacing;
            if (child->height > max_h) max_h = child->height;
        }
        int total_w = (cx > widget->stack_padding) ? (cx - widget->stack_spacing + widget->stack_padding) : (widget->stack_padding * 2);
        int total_h = max_h + widget->stack_padding * 2;
        if (widget->parent != nullptr) {
            widget->width = total_w;
            widget->height = total_h;
        }
    }
}

heliosview_ui_widget_t* heliosview_ui_vstack_create(int spacing, int padding) {
    heliosview_ui_widget_desc_t desc{};
    auto* w = heliosview_ui_widget_create(&desc, nullptr);
    w->is_stack = true;
    w->is_vertical_stack = true;
    w->stack_spacing = spacing;
    w->stack_padding = padding;
    return w;
}

heliosview_ui_widget_t* heliosview_ui_hstack_create(int spacing, int padding) {
    heliosview_ui_widget_desc_t desc{};
    auto* w = heliosview_ui_widget_create(&desc, nullptr);
    w->is_stack = true;
    w->is_vertical_stack = false;
    w->stack_spacing = spacing;
    w->stack_padding = padding;
    return w;
}
