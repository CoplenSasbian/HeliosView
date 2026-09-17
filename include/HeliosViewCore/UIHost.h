#pragma once

/**
 * HeliosView.Core -- UIHost: a retained widget tree hosted in a child viewport.
 *
 * A typed, RAII wrapper over the UI half of <HeliosView/heliosview_host.h>; the C
 * header stays the contract, this one only changes the spelling:
 *
 *   heliosview_host_create_ui(window, ...)  -> helios::UIHost::create(window, ...)
 *   heliosview_host_destroy(host)           -> ~UIHost()
 *   heliosview_host_set_bounds / get_bounds -> setBounds / bounds
 *   heliosview_host_set_visible / is_visible-> setVisible / isVisible
 *   heliosview_host_ui_get_canvas           -> canvas
 *   heliosview_host_ui_request_repaint      -> requestRepaint
 *   heliosview_host_ui_set_root             -> setRoot / setRootWidget
 *   heliosview_host_ui_get_root             -> rootHandle
 *   heliosview_host_ui_present              -> present
 *   heliosview_host_get_parent              -> parent
 *   heliosview_host_native_handle           -> nativeHandle
 *
 * Typical wiring -- the host renders a widget tree into its own canvas and routes
 * mouse events and repaints to it, so the only calls the application makes are the
 * tree itself and requestRepaint:
 *
 *     helios::Window window(1020, 800, "Gallery");
 *     auto host = helios::UIHost::create(window, 0, 0, 1020, 800);
 *
 *     auto root = HeliosView::UI::VStack::create(16, 24);
 *     root->add(HeliosView::UI::Label::create("Hello"));
 *     host.setRootWidget(root);                  // the host keeps the widget alive
 *
 *     window.show();
 *     return app.exec();
 *
 * Ownership -- a UIHost owns its native host and destroys it on scope exit, move,
 * or close(). Window::createUIHost() returns one of these attached to the window,
 * so it does NOT own it there: the window destroys its hosts on close, which makes
 * the UIHost a safe borrowed handle (Window::destroyHost() detaches one early).
 * One host is owned exactly once -- never destroy a host both ways.
 *
 * A UIHost is move-only. An invalid one (the default-constructed one, or a failed
 * create) is a plain empty object: every call on it is a no-op returning false, not
 * a crash.
 *
 * Threading: UIHost is not thread-safe. Create, mutate and destroy it on the
 * message-loop thread, and keep it alive for as long as the tree may need it.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_host.h>
#include <HeliosView/heliosview_ui.h>
#include <HeliosViewCore/System.h>
#include <HeliosViewCore/Window.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace HeliosView::UI {
class Widget; /* Defined in <HeliosViewCore/UI/Widget.h>; only its handle() is needed here */
}

namespace helios {

class UIHost;

class UIHost {
public:
	// An empty host: no native viewport, every call fails. Useful as a member that
	// is assigned later.
	UIHost() = default;

	/* ---- creating hosts ----
	 * All three return an invalid host on failure instead of throwing; the reason is
	 * the library's thread-local last error (helios::lastErrorDescription).
	 *
	 * Defined at the bottom of <HeliosViewCore/Window.h>: creating a host needs the
	 * complete Window, which is the header this one is pulled in from.
	 */

	// A widget-tree viewport of `width` x `height` at (x, y) inside `window`, drawn
	// on `engine` (the heliosview_canvas_engine_t value, e.g.
	// HELIOSVIEW_ENGINE_BLEND2D; HELIOSVIEW_ENGINE_AUTO picks the default). The host
	// is attached to the window, which destroys it on close.
	static UIHost create(Window& window, int x, int y, int width, int height,
						 uint32_t engine = HELIOSVIEW_ENGINE_BLEND2D);

	// Same, but the returned host owns the native host: it is NOT registered with the
	// window, so destroy it by letting this object go out of scope.
	static UIHost createDetached(Window& window, int x, int y, int width, int height,
								 uint32_t engine = HELIOSVIEW_ENGINE_BLEND2D);

	// An embedded web viewport, owned by this object exactly like createDetached.
	// Get the WebView itself with heliosview_host_get_webview(host.handle()).
	static UIHost createWebView(Window& window, int x, int y, int width, int height);

	~UIHost()
	{
		if (m_host != nullptr && m_owner == Owner::UiHost)
			heliosview_host_destroy(m_host);
	}

	UIHost(const UIHost&) = delete;
	UIHost& operator=(const UIHost&) = delete;

	UIHost(UIHost&& other) noexcept
		: m_host(other.m_host), m_owner(other.m_owner)
	{
		other.m_host = nullptr;
		other.m_owner = Owner::Window;
	}

	UIHost& operator=(UIHost&& other) noexcept
	{
		if (this != &other) {
			close();
			m_host = other.m_host;
			m_owner = other.m_owner;
			other.m_host = nullptr;
			other.m_owner = Owner::Window;
		}
		return *this;
	}

	// True when the host exists. An invalid host is an empty object, not a broken one.
	bool valid() const
	{
		return m_host != nullptr;
	}

	// The underlying C handle (NULL when !valid()), for the C API calls this wrapper
	// does not cover.
	heliosview_host_t* handle() const
	{
		return m_host;
	}

	// The native child viewport (HWND on Windows), or nullptr
	void* nativeHandle() const
	{
		return m_host != nullptr ? heliosview_host_native_handle(m_host) : nullptr;
	}

	// The window this host is attached to, or nullptr when it is not attached
	heliosview_window_t* parent() const
	{
		return m_host != nullptr ? heliosview_host_get_parent(m_host) : nullptr;
	}

	// Destroy the native host now (only when this object owns it) and leave an empty
	// object behind. The destructor calls this, so calling it first is fine.
	void close()
	{
		if (m_host != nullptr && m_owner == Owner::UiHost) {
			heliosview_host_destroy(m_host);
		}
		m_host = nullptr;
	}

	/* ---- geometry, visibility ---- */

	// Position and size relative to the parent window's client area
	void setBounds(int x, int y, int width, int height)
	{
		heliosview_host_set_bounds(m_host, x, y, width, height);
	}

	// The current bounds; empty Rect when !valid()
	Rect bounds() const
	{
		int x = 0, y = 0, width = 0, height = 0;
		heliosview_host_get_bounds(m_host, &x, &y, &width, &height);
		return Rect{x, y, width, height};
	}

	void setVisible(bool visible)
	{
		heliosview_host_set_visible(m_host, visible ? 1 : 0);
	}

	bool isVisible() const
	{
		return heliosview_host_is_visible(m_host) != 0;
	}

	/* ---- the UI subclass ---- */

	// The canvas the widget tree is drawn on (NULL when this host has no UI subclass).
	// Borrowed from the host: do not destroy it, and do not draw on it while the
	// tree is repainting.
	heliosview_canvas_t* canvas() const
	{
		return m_host != nullptr ? heliosview_host_ui_get_canvas(m_host) : nullptr;
	}

	// Repaint the widget tree into the viewport now. This is what a state change
	// calls when the tree does not repaint itself.
	void requestRepaint()
	{
		heliosview_host_ui_request_repaint(m_host);
	}

	// Present the current canvas contents to the viewport (the host does this after
	// its own paint cycle; call it directly only when drawing into canvas() yourself).
	void present()
	{
		heliosview_host_ui_present(m_host);
	}

	// Attach a widget tree as this host's root. The host keeps the shared_ptr alive
	// until it is replaced, cleared, or the host is destroyed, so a tree built inline
	// stays valid: pass the shared_ptr, not a temporary raw handle.
	//
	// Deliberately a template: UIHost and the widget classes are peers that both layer
	// on Window, so this header cannot name Widget beyond the forward declaration
	// above. Any type with handle() works, and an empty pointer detaches the root.
	template <class WidgetT>
	void setRootWidget(const std::shared_ptr<WidgetT>& root)
	{
		m_root = root;
		heliosview_host_ui_set_root(m_host, root ? root->handle() : nullptr);
	}

	// Detach the root widget. The tree itself lives on in the caller's shared_ptr.
	void clearRoot()
	{
		m_root.reset();
		heliosview_host_ui_set_root(m_host, nullptr);
	}

	// The shared_ptr this host is keeping alive, or nullptr. Widget is incomplete
	// here, so callers that touch the tree include <HeliosViewCore/UI/Widget.h>.
	std::shared_ptr<HeliosView::UI::Widget> root() const
	{
		return m_root;
	}

	// The root widget's raw C handle, or NULL
	heliosview_ui_widget_t* rootHandle() const
	{
		return m_host != nullptr ? heliosview_host_ui_get_root(m_host) : nullptr;
	}

	/* ---- adopting an existing C host ----
	 * These two exist for the factory functions above and for Window, whose inline
	 * definitions cannot reach a private member here; an application creates hosts
	 * through create() / createDetached() / createWebView() / Window::createUIHost(),
	 * never by adopting a raw handle.
	 */

	// Who destroys the native host: the UIHost object, or (for a host created through
	// Window::createUIHost) the window that registered it.
	enum class Owner {
		UiHost,
		Window,
	};

	// Adopt a C host the window registered (Window::ownHost): the window destroys it
	explicit UIHost(heliosview_host_t* host) : m_host(host), m_owner(Owner::Window) {}

	// Adopt a C host this object owns
	UIHost(heliosview_host_t* host, Owner owner) : m_host(host), m_owner(owner) {}

private:
	heliosview_host_t* m_host = nullptr;
	Owner m_owner = Owner::Window;
	std::shared_ptr<HeliosView::UI::Widget> m_root;
};

} // namespace helios
