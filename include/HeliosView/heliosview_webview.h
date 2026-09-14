#ifndef HELIOSVIEW_HELIOSVIEW_WEBVIEW_H
#define HELIOSVIEW_HELIOSVIEW_WEBVIEW_H

/**
 * HeliosView C API -- WebView (Windows: WebView2)
 *
 * The embedded web engine: creation and navigation, the JS <-> native bridge,
 * injected scripts, events, local resources, background and inset control, zoom,
 * right-click interception, cookies and DevTools.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= WebView (Windows: WebView2) =================
 *
 * A WebView handle independent of the window: it attaches to a parent window at
 * creation; afterwards operations involve only the webview itself, not
 * heliosview_window_t. Initialization is asynchronous; navigation requests made
 * before it completes are queued automatically (the last one wins).
 * Note: destroy the webview before the window; the window must not be destroyed
 * before initialization completes (usually a few milliseconds).
 *
 * Portability: the API is engine-neutral (Windows: WebView2; macOS: WKWebView;
 * Linux: WebKitGTK). Navigation, HTML loading, script evaluation, injected
 * document-start scripts, the JS bridge (bind/resolve/reject/subscribe/broadcast),
 * navigation callbacks, opaque background colors, page zoom, the cookie store,
 * local-folder mapping, insets and the native-handle accessor map to every engine.
 * Where the engines disagree, the library flattens instead of leaking: debugging is
 * one switch on every platform (see heliosview_webview_set_devtools), an engine's
 * own browser shortcuts are forced off wherever the engine has them, and
 * capabilities only one engine offers are not exposed at all. A function an
 * engine cannot serve returns HELIOSVIEW_ERROR_UNSUPPORTED (-4) with the reason
 * in heliosview_last_error_string; the README carries the per-platform table.
 * Creation-time options that do not apply to an engine are ignored.
 */

typedef struct heliosview_webview heliosview_webview_t;

/* Why a navigation failed (heliosview_webview_navigation_cb). 0 = success; the
 * other values are portable — the engine's own code is available through
 * heliosview_webview_last_native_error. */
typedef enum heliosview_webview_error {
    HELIOSVIEW_WEBVIEW_OK = 0,
    HELIOSVIEW_WEBVIEW_ERROR_CANCELLED = 1,          /* navigation cancelled / aborted by the app */
    HELIOSVIEW_WEBVIEW_ERROR_HOST_NOT_FOUND = 2,     /* DNS / name resolution failed */
    HELIOSVIEW_WEBVIEW_ERROR_CONNECTION_FAILED = 3,  /* could not connect, disconnected, reset */
    HELIOSVIEW_WEBVIEW_ERROR_TIMEOUT = 4,
    HELIOSVIEW_WEBVIEW_ERROR_TLS = 5,                /* certificate / TLS problem */
    HELIOSVIEW_WEBVIEW_ERROR_HTTP = 6,               /* server returned an invalid response */
    HELIOSVIEW_WEBVIEW_ERROR_OTHER = 7,
} heliosview_webview_error_t;

/* Creation-time WebView2 environment options: the fields below are read when
 * the WebView2 environment is created and cannot be changed afterwards, so they
 * are only honored via heliosview_webview_create_ex. Zero-initialize the struct
 * (or pass NULL) for the pure runtime defaults and set only what you need.
 * Fields marked [Windows] have no equivalent on the other engines and are
 * ignored there.
 * String fields are UTF-8 and copied by the library; they may point at
 * temporary storage. Boolean fields are 0/1: 0 (the zero-initialized default)
 * = leave at the runtime default, 1 = enable. */
typedef struct heliosview_webview_env_opts {
    /* WebView2 browser data folder (profile, cache, cookies — the
     * <exe>.WebView2 folder). UTF-8 absolute path; NULL/"" = the default next
     * to the executable. Created by WebView2 if missing. */
    const char* user_data_folder;
    /* [Windows] Folder of a fixed WebView2 runtime (the directory that holds
     * msedgewebview2.exe). UTF-8; NULL = the system WebView2 Runtime. */
    const char* browser_executable_folder;
    /* Default page language / Accept-Language, e.g. "zh-CN". UTF-8;
     * NULL = the system default. */
    const char* language;
    /* [Windows] Extra Chromium command-line switches, e.g. "--disable-gpu".
     * UTF-8; NULL = none. */
    const char* additional_browser_arguments;
    /* [Windows] Target compatible browser version (used with
     * browser_executable_folder), e.g. "95.*"; NULL/"" = the latest available
     * on that runtime. UTF-8. */
    const char* target_compatible_browser_version;
    /* [Windows] Use the OS primary account for single sign-on (0/1). */
    int allow_sso_with_os_primary_account;
    /* [Windows] Exclusive access to the user data folder, so no other process
     * can share it (0/1). */
    int exclusive_user_data_folder_access;
    /* Tracking prevention (on by default in WebView2): 1 = turn it off,
     * 0 = keep it enabled. */
    int disable_tracking_prevention;
    /* [Windows] Browser extensions (e.g. ad blockers) enabled (0/1). */
    int are_browser_extensions_enabled;
} heliosview_webview_env_opts_t;

/* ================= WebView engine availability =================
 *
 * The WebView backend is the platform's web engine. On Windows that is the
 * WebView2 Runtime — a separate deploy-time component (preinstalled on Windows
 * 11, present on most but not all Windows 10 devices) — so an app may need to
 * know, before it creates a window or a WebView, whether an engine is available
 * and which version it is. The same call answers that on every platform:
 *
 *   Windows   WebView2 Runtime version, e.g. "131.0.2903.86". Empty = the
 *             Runtime is not installed; heliosview_webview_create/_ex then fail
 *             with HELIOSVIEW_ERROR_UNSUPPORTED (-4) and the window is left
 *             untouched, so an app can fall back to a non-WebView UI.
 *   macOS     The system WebKit version (part of the OS, always available; read
 *             from the WebKit framework's bundle - WebKit exposes no version
 *             API of its own).
 *   Linux     The WebKitGTK version, e.g. "2.44.3"; empty = the backend's web
 *             engine library is not present at runtime.
 *   other     Empty (no WebView backend on this platform yet).
 *
 * Writes a UTF-8 version string into buf (always NUL-terminated, truncated to
 * fit `size`; "" when no engine is available) and returns 0. Negative = invalid
 * arguments (buf == NULL or size == 0). */
HELIOSVIEW_API int heliosview_webview_engine_version(char* buf, size_t size);

/* Create a WebView in the parent window's client area (async initialization)
 * with creation-time WebView2 environment options (see
 * heliosview_webview_env_opts_t; NULL = all runtime defaults).
 * Returns NULL on failure: HELIOSVIEW_ERROR_UNSUPPORTED (-4) when no web engine
 * is available (see heliosview_webview_engine_version). */
HELIOSVIEW_API heliosview_webview_t* heliosview_webview_create_ex(
    heliosview_window_t* parent, const heliosview_webview_env_opts_t* opts);

/* Create a WebView in the parent window's client area (async initialization;
 * all WebView2 environment options at their runtime defaults).
 * Returns NULL on failure (see heliosview_webview_create_ex). */
HELIOSVIEW_API heliosview_webview_t* heliosview_webview_create(heliosview_window_t* parent);

/* Destroy the WebView (must be called before destroying the parent window) */
HELIOSVIEW_API void heliosview_webview_destroy(heliosview_webview_t* webview);

/* The engine object behind the WebView - a native escape hatch, so an app that
 * needs something the C API does not cover is not blocked by the library. The
 * pointer types are the platform's own:
 *
 *   kind         Windows                 macOS          Linux
 *   WINDOW       HWND (the parent)       NSWindow*      GtkWindow*
 *   WIDGET       HWND (the parent)       NSView*        GtkWidget*
 *   CONTROLLER   ICoreWebView2Controller* WKWebView*    WebKitWebView*
 *
 * (On Windows the WebView2 lives inside the parent HWND, so WINDOW and WIDGET
 * are the same handle and the engine object is the controller - ask it for
 * get_CoreWebView2. On WebKitGTK the widget and the engine are the same object.)
 *
 * The handle is borrowed: the library keeps owning it, and it stays valid only
 * while the WebView exists, on the message-loop/UI thread. NULL is returned for
 * an unknown kind, an uninitialized WebView (CONTROLLER), or a destroyed one.
 * No error code - a NULL handle is the answer. Message-loop thread. */
typedef enum heliosview_webview_handle_kind {
    HELIOSVIEW_WEBVIEW_HANDLE_WINDOW = 0,     /* the window hosting the WebView */
    HELIOSVIEW_WEBVIEW_HANDLE_WIDGET = 1,     /* the WebView as a native view */
    HELIOSVIEW_WEBVIEW_HANDLE_CONTROLLER = 2  /* the engine's own browser object */
} heliosview_webview_handle_kind_t;

HELIOSVIEW_API void* heliosview_webview_native_handle(heliosview_webview_t* webview,
                                                      heliosview_webview_handle_kind_t kind);

/* Navigate to a URL (queued if initialization is not complete) */
HELIOSVIEW_API int heliosview_webview_navigate(heliosview_webview_t* webview, const char* url);

HELIOSVIEW_API int heliosview_webview_navigate_html(heliosview_webview_t* webview, const char* html);

/* ================= WebView low-footprint mode =================
 *
 * One switch that asks the engine to give back as much as it can while the page
 * stays alive (no navigation is lost, no state is reset). "Low footprint" means
 * something different inside each engine, so the name stays deliberately vague
 * and the call means the same thing everywhere:
 *
 *   Windows   WebView2 TrySuspend / Resume: applied immediately, but the engine
 *             may decline (it does not suspend while the WebView is visible or
 *             busy, e.g. audio playing) - the callback reports what happened.
 *   macOS 14+ WKPreferences.inactiveSchedulingPolicy: turning the mode on makes
 *             the engine suspend the WebView by itself once it is detached from
 *             the view hierarchy and idle (not loading, no media); turning it off
 *             restores normal scheduling. Nothing suspends at the moment of the
 *             call.
 *   macOS <14, other platforms: no equivalent - returns
 *             HELIOSVIEW_ERROR_UNSUPPORTED (-4). An app that needs the memory
 *             back can hide the webview, or destroy and recreate it.
 *
 * Typical use: on when the window is hidden or minimized, off when it returns.
 */

/* Completion callback for heliosview_webview_set_low_footprint: error is 0 on
 * success; active is 1 when low-footprint mode is actually in effect (Windows:
 * TrySuspend really suspended the WebView; macOS: the policy is now enabled).
 * Runs on the UI thread. */
typedef void (*heliosview_webview_low_footprint_cb)(int error, int active, void* userdata);

/* Turn low-footprint mode on (enabled != 0) or off (async; completion via
 * callback, may be NULL for fire-and-forget). A request made before
 * initialization is recorded and applied when the core becomes ready (after any
 * queued navigation/scripts). 0 = success (request accepted), negative = error
 * code. */
HELIOSVIEW_API int heliosview_webview_set_low_footprint(
    heliosview_webview_t* webview, int enabled,
    heliosview_webview_low_footprint_cb callback, void* userdata);

/* Read whether low-footprint mode is on (1) or off (0) - the mode the app asked
 * for, not "is the engine suspended at this instant" (no engine can answer that
 * portably). 1/0 is written to out_enabled. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_webview_is_low_footprint(heliosview_webview_t* webview,
                                                       int* out_enabled);

/* ================= WebView background color =================
 *
 * WebView2 DefaultBackgroundColor (ICoreWebView2Controller2): the color the
 * WebView paints behind the page content (default: opaque white). An alpha of
 * 0 makes the background transparent — the parent window's own content shows
 * through the WebView (to see the desktop through it, the parent window must
 * itself be transparent, e.g. a layered window). Channels are (red, green,
 * blue, alpha), each 0-255.
 *
 *   macOS    Only opaque colors. WKWebView's public API cannot make a WebView
 *            truly transparent (that needs private API), so an alpha below 255 —
 *            and heliosview_webview_set_transparent_background(1) — returns
 *            HELIOSVIEW_ERROR_UNSUPPORTED (-4) there; the RGB color itself is
 *            accepted (WKWebView.underPageBackgroundColor, macOS 12+).
 */

/* Set the WebView's default background color. Applies immediately when the
 * core is initialized, otherwise when it becomes ready. 0 = success,
 * negative = error code. */
HELIOSVIEW_API int heliosview_webview_set_background_color(heliosview_webview_t* webview,
                                                           uint8_t red, uint8_t green,
                                                           uint8_t blue, uint8_t alpha);

/* Convenience: make the WebView background transparent (transparent != 0) or
 * restore the default opaque white. Returns HELIOSVIEW_ERROR_UNSUPPORTED (-4)
 * where the engine cannot do transparency (macOS — see above). 0 = success,
 * negative = error code. */
HELIOSVIEW_API int heliosview_webview_set_transparent_background(heliosview_webview_t* webview,
                                                                 int transparent);

/* ================= WebView native bindings (JS <-> native bridge) =================
 *
 * Each WebView runs a small shim (injected automatically) that exposes:
 *   - window.helios.call(name, ...args) -> Promise   invoke a bound native function
 *   - window.BroadcastChannel(name)                  receive native broadcasts
 *
 * Native <-> JS messages are JSON strings with a "__hv":1 envelope:
 *   JS -> native : { "__hv":1, "kind":"call", "id":N, "name":"...", "args":[...] }
 *   native -> JS : { "__hv":1, "kind":"resolve",  "id":N, "result":<json> }
 *                  { "__hv":1, "kind":"reject",   "id":N, "error":<json> }
 *                  { "__hv":1, "kind":"broadcast", "name":"...", "data":<json> }
 *
 * Threading: every WebView API is a UI-thread call — resolve / reject /
 * broadcast included (they do not marshal). eval / eval_async are queued while
 * the WebView initializes.
 * Lifetime: destroy the WebView only when no asynchronous calls are in flight
 * (a bind handler still running, or an eval_async not yet completed).
 */

/* Destructor for a binding's userdata; called when the binding is replaced or the
 * WebView is destroyed. May be NULL. */
typedef void (*heliosview_webview_userdata_dtor)(void* userdata);

/* Callback for a bound native function. args_json is the JSON array of the JS
 * call's arguments ("" when none). Reply via heliosview_webview_resolve/reject
 * with the same call_id. Runs on the UI thread. */
typedef void (*heliosview_webview_bind_cb)(heliosview_webview_t* webview,
                                           uint64_t call_id, const char* name,
                                           const char* args_json, void* userdata);

/* Callback for heliosview_webview_eval_async. On success (error == 0),
 * result_json is the JSON encoding of the script's completion value. On failure,
 * error is negative and result_json carries the script's error message text
 * (not JSON). Runs on the UI thread. */
typedef void (*heliosview_webview_eval_cb)(int error, const char* result_json, void* userdata);

/* Callback for a broadcast subscription: fires when the page posts a message to
 * its BroadcastChannel(name) instance(s). data_json is the posted value, which
 * may be any JSON type ("" when the message had no data). Runs on the UI thread. */
typedef void (*heliosview_webview_subscribe_cb)(heliosview_webview_t* webview,
                                                const char* name, const char* data_json,
                                                void* userdata);

/* Callback for navigation events: fires when a navigation completes (page fully
 * loaded) or fails. error is HELIOSVIEW_WEBVIEW_OK (0) on success, else a
 * portable heliosview_webview_error_t value; the engine's own error code is
 * available through heliosview_webview_last_native_error.
 * Runs on the UI thread. Only one callback may be registered; setting a new one
 * replaces the previous (running its dtor). */
typedef void (*heliosview_webview_navigation_cb)(heliosview_webview_t* webview,
                                                 int error, void* userdata);

/* The engine's own code for the last completed navigation: a
 * COREWEBVIEW2_WEB_ERROR_STATUS_* value on Windows, an NSURLError code on
 * macOS, a GError code on Linux. 0 = success, or the engine reported no specific
 * code — use the navigation callback's portable error to tell the two apart.
 * Diagnostic. */
HELIOSVIEW_API int heliosview_webview_last_native_error(heliosview_webview_t* webview);

/* Register a native function under `name`, callable from JS via
 * window.helios.call(name, ...). Rebinding a name replaces the previous binding
 * and calls its dtor (if any). dtor(userdata) also runs when the WebView is
 * destroyed. UI-thread call.
 * Internal names: the library's built-in bridge uses "__hv."-prefixed names
 * (__hv.control / __hv.state / __hv.drag, called by the injected
 * <helios-window-controls> / <helios-window-title-bar> components). They contain
 * a dot, so they are not valid C identifiers and applications cannot bind (or
 * subscribe) them — the call fails with -2 like any invalid name. */
HELIOSVIEW_API int heliosview_webview_bind(heliosview_webview_t* webview, const char* name,
                                           heliosview_webview_bind_cb callback, void* userdata,
                                           heliosview_webview_userdata_dtor dtor);

/* The WebView instance no longer exists: returned by the bridge calls
 * (heliosview_webview_resolve / _reject / _broadcast) when the WebView
 * was already destroyed — e.g. destroyWebView/heliosview_webview_destroy ran
 * while this asynchronous call was still in flight. The call then never touches
 * the freed instance. */
#define HELIOSVIEW_WEBVIEW_DESTROYED (-3)

/* Resolve a pending JS Promise: result_json is any valid JSON value. UI-thread call.
 * Returns -3 (HELIOSVIEW_WEBVIEW_DESTROYED) when the WebView instance no longer
 * exists — e.g. destroyWebView/heliosview_webview_destroy ran while this
 * asynchronous call was still in flight. The call then never touches the freed
 * instance, so the misuse fails with a clear error code instead of a crash. */
HELIOSVIEW_API int heliosview_webview_resolve(heliosview_webview_t* webview,
                                              uint64_t call_id, const char* result_json);

/* Reject a pending JS Promise: error_json is any valid JSON value. UI-thread call.
 * Same stale-instance guard as resolve: returns -3 when the WebView was already
 * destroyed. */
HELIOSVIEW_API int heliosview_webview_reject(heliosview_webview_t* webview,
                                             uint64_t call_id, const char* error_json);

/* Run a JavaScript string (fire-and-forget). UI-thread call; queued while the
 * WebView is still initializing. */
HELIOSVIEW_API int heliosview_webview_eval(heliosview_webview_t* webview, const char* script);

/* Run a JavaScript string and get its JSON completion value. UI-thread call; queued
 * while the WebView is still initializing. The callback fires exactly once.
 * A returned Promise is awaited, so "fetch(...).then(r => r.json())" resolves to
 * the JSON value (the engine's own script evaluation returns the promise object
 * instead). A script that throws, or a rejected promise, reports a negative
 * error and the message text. */
HELIOSVIEW_API int heliosview_webview_eval_async(heliosview_webview_t* webview, const char* script,
                                                 heliosview_webview_eval_cb callback, void* userdata);

/* ================= Injected scripts (document start) =================
 *
 * Scripts that run in every document before the page's own scripts do - the
 * place for a bridge shim, a polyfill, or an app-wide `window.__CONFIG`. They
 * are stored on the WebView, so they also apply to pages loaded later and
 * survive a queue-until-ready creation (they are registered as soon as the core
 * is up, before any queued navigation).
 *
 * One world on every platform: WebView2 has no isolated world, so an injected
 * script shares the page's global scope - do not rely on hiding anything from
 * the page (WKWebView's WKContentWorld and WebKitGTK's script worlds exist, but
 * offering them only there would make the same call behave differently per
 * platform - exactly what the library avoids).
 *
 * Message-loop thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_add_init_script(heliosview_webview_t* webview,
                                                      const char* script);

/* Remove every injected script (registered by add_init_script); scripts already
 * run in the current document are unaffected. Message-loop thread. 0 = success,
 * negative = error. */
HELIOSVIEW_API int heliosview_webview_clear_init_scripts(heliosview_webview_t* webview);

/* Broadcast a JSON value to the JS page's BroadcastChannel(name) instances; the
 * page receives it as a standard 'message' event. UI-thread call. Same
 * stale-instance guard as resolve: returns -3 when the WebView was already
 * destroyed. */
HELIOSVIEW_API int heliosview_webview_broadcast(heliosview_webview_t* webview,
                                                const char* name, const char* data_json);

/* Subscribe to broadcasts the page posts via its BroadcastChannel(name) instances:
 * callback(name, data_json, userdata) fires on the UI thread for every postMessage
 * to a channel of that name. Subscribing to a name replaces the previous
 * subscription (calling its dtor). dtor(userdata) also runs when the WebView is
 * destroyed. UI-thread call. The internal "__hv.*" bridge names (see
 * heliosview_webview_bind) are not valid identifiers and cannot be subscribed. */
HELIOSVIEW_API int heliosview_webview_subscribe(heliosview_webview_t* webview, const char* name,
                                                heliosview_webview_subscribe_cb callback,
                                                void* userdata,
                                                heliosview_webview_userdata_dtor dtor);

/* Remove the subscription for `name` (calling its dtor). UI-thread call. */
HELIOSVIEW_API int heliosview_webview_unsubscribe(heliosview_webview_t* webview, const char* name);

/* ================= WebView events & local resources ================= */

/* Callback for navigation-start events: fires on the UI thread when a new
 * navigation begins (the initial load, links, programmatic navigate, browser
 * back/forward, and redirects). uri is the target URI (UTF-8, valid for the
 * duration of the call). is_redirected / is_user_initiated follow WebView2's
 * NavigationStarting semantics (1/0). The callback's return value cancels the
 * navigation when non-zero (0 = let it proceed). */
typedef int (*heliosview_webview_navigation_starting_cb)(heliosview_webview_t* webview,
                                                         const char* uri,
                                                         int is_redirected,
                                                         int is_user_initiated,
                                                         void* userdata);

/* Callback for source-changed (URL-changed) events: fires on the UI thread when
 * the WebView's Source (current URL) property changes. uri is the new source URI
 * (UTF-8, valid for the duration of the call); is_new_document is 1 when the
 * source change is due to a new document load, 0 for an in-document change. */
typedef void (*heliosview_webview_source_changed_cb)(heliosview_webview_t* webview,
                                                     const char* uri,
                                                     int is_new_document,
                                                     void* userdata);

/* Callback for document-title events: fires on the UI thread when the page's
 * title changes. title is the new document title (UTF-8, valid for the duration
 * of the call). */
typedef void (*heliosview_webview_title_changed_cb)(heliosview_webview_t* webview,
                                                    const char* title,
                                                    void* userdata);

/* Register a navigation-completed callback (replacing any previous one and
 * running its dtor). The callback fires on the UI thread when a navigation
 * completes or fails; it is not called for navigations that never finish
 * (e.g. aborted). UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_navigation_callback(heliosview_webview_t* webview,
                                                              heliosview_webview_navigation_cb callback,
                                                              void* userdata,
                                                              heliosview_webview_userdata_dtor dtor);

/* Register a navigation-starting callback (replacing any previous one and
 * running its dtor). Fires on the UI thread just before a navigation begins;
 * returning non-zero cancels it (e.g. to block cross-origin or external links).
 * UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_navigation_starting_callback(
    heliosview_webview_t* webview,
    heliosview_webview_navigation_starting_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Register a source-changed (URL-changed) callback (replacing any previous one
 * and running its dtor). Fires on the UI thread whenever the WebView's current
 * URL changes. UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_source_changed_callback(
    heliosview_webview_t* webview,
    heliosview_webview_source_changed_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Register a document-title-changed callback (replacing any previous one and
 * running its dtor). Fires on the UI thread when the page title changes.
 * UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_title_changed_callback(
    heliosview_webview_t* webview,
    heliosview_webview_title_changed_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Map a local folder to a virtual host name so the page can load files from it
 * through heliosview_webview_local_url(). Used to serve images or other local
 * assets that are not part of the packaged frontend (game banners, avatars, ...).
 * Call before navigating, or the page must be reloaded for new mappings to take
 * effect.
 * Windows (WebView2) restricts mappings to the "trusted origin" host suffix
 * .local, e.g. "assets.local".
 * Returns 0 = success, negative = failure. */
HELIOSVIEW_API int heliosview_webview_map_local_folder(heliosview_webview_t* webview,
                                                       const char* host_name,
                                                       const char* folder_path);

/* Build the URL that serves `path` from the folder mapped to `host_name` (see
 * heliosview_webview_map_local_folder). The URL shape is engine-defined:
 * Windows produces "https://<host>/<path>", other engines register a custom
 * scheme instead — this helper is what keeps the difference out of application
 * code, so never build the URL by hand.
 * Writes a NUL-terminated URL into buf (truncated to fit `size`); `path` is
 * appended as given (percent-encode it yourself if it contains special
 * characters). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_webview_local_url(heliosview_webview_t* webview,
                                                const char* host_name, const char* path,
                                                char* buf, size_t size);

/* Keep the given insets (client pixels) clear around the WebView: the WebView
 * occupies the parent client area minus these insets on each side, and the
 * cleared strips remain the parent window's own surface. Re-applied on every
 * window resize. Useful when the window keeps native chrome of its own around
 * the WebView (e.g. a header strip drawn by the app). Zero insets restore the
 * default (WebView fills the client area). Applies immediately when the WebView
 * is initialized; when called during initialization the bounds are applied when
 * it becomes ready. Negative insets are clamped to 0. Message-loop thread.
 * 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_insets(heliosview_webview_t* webview,
                                                 int32_t top, int32_t right,
                                                 int32_t bottom, int32_t left);

/* ================= Page zoom =================
 *
 * The page's zoom factor: 1.0 = 100% (the default), 1.5 = 150%, 0.5 = 50%.
 * It scales the page content, not the window, and the engine keeps it across
 * navigations. Per platform: WebView2 ICoreWebView2Controller::put_ZoomFactor,
 * WebKitGTK webkit_web_view_set_zoom_level, macOS WKWebView.pageZoom
 * (macOS 11+; older macOS returns HELIOSVIEW_ERROR_UNSUPPORTED).
 *
 * The value is stored on the WebView, so a call made during initialization is
 * applied when the core becomes ready (a queued navigation keeps it too).
 * Message-loop thread. 0 = success, negative = error. A factor <= 0 is
 * rejected with HELIOSVIEW_ERROR_INVALID_ARGUMENT (-2). */
HELIOSVIEW_API int heliosview_webview_set_zoom(heliosview_webview_t* webview, double factor);

/* Read the current zoom factor (1.0 = 100%) into out_factor; before the core is
 * ready this reports the value that will be applied. 0 = success,
 * negative = error. */
HELIOSVIEW_API int heliosview_webview_zoom(heliosview_webview_t* webview, double* out_factor);

/* Show (enabled != 0, the default) or suppress the WebView2 default right-click
 * context menu (copy/paste/inspect etc.). This is the one switch that decides
 * whether the engine's own menu may open; who else reacts to the right click is
 * independent of it (see the interception section below).
 *
 * Suppressing needs the engine's context-menu hook: turning it off reports
 * HELIOSVIEW_ERROR_UNSUPPORTED (-4) when the engine has none - [Windows] a WebView2
 * runtime older than 100 (ICoreWebView2_11); [macOS] always, because WKWebView
 * exposes no public context-menu API - so the app can fall back to a menu drawn
 * in the page. Turning it back on always succeeds (it asks for nothing).
 *
 * Applies immediately when the WebView is initialized; when called during
 * initialization the check (and the setting) is applied when it becomes ready.
 * Message-loop thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_context_menu(heliosview_webview_t* webview,
                                                       int enabled);

/* ================= Right-click interception =================
 *
 * A right click reaches the application through two independent doors - take
 * either, both, or neither:
 *
 *   1. The page: its DOM 'contextmenu' event fires as usual. preventDefault() plus
 *      a menu drawn in HTML is the fully portable answer, and the only one that
 *      works on an engine without a native hook.
 *   2. The native side: the callback below. It is consulted for every right click
 *      before the engine opens its own menu and reports what the click hit (link,
 *      selection, image, media, editable field, page, position).
 *
 * What happens to the engine's menu is the single switch above:
 *   - the callback returns non-zero  -> that click is intercepted (the engine's
 *     menu stays closed, even when it is enabled);
 *   - the callback returns 0 (or no callback is registered) -> the switch decides:
 *     enabled opens the engine's menu, suppressed opens nothing.
 * So one callback can serve some targets itself and leave the rest to the engine.
 *
 * The library does not choose what to show: build a heliosview_menu_t, draw the
 * menu in the page, or show nothing at all. Note that a native menu runs a modal
 * message loop - open it after returning (e.g. on the next loop turn via
 * heliosview_post_event / App::postTask), not inside the callback.
 *
 * Portable contract: the vocabulary here is engine-neutral (the switch plus a
 * target bitmask and UTF-8 strings), so another backend maps it onto whatever its
 * engine offers - WebView2's ContextMenuRequested event, WebKitGTK's
 * WebView::context-menu signal (a WebKitContextMenu + WebKitHitTestResult), CEF's
 * CefContextMenuHandler::OnBeforeContextMenu + CefContextMenuParams - and fills
 * only the fields its engine can report (the rest stay ""). Engines without a
 * public hook report that from the switch call and never invoke the callback;
 * macOS is the standing example (WKWebView exposes its context menu only through
 * private SPI), so there the page-drawn route - a DOM contextmenu listener that
 * calls preventDefault() - is the portable answer.
 * Message-loop thread. */

/* What the right-click hit. Bit flags, so a click can combine several (e.g. a
 * link inside an editable field, or an image with a selection). */
#define HELIOSVIEW_CONTEXT_MENU_TARGET_PAGE      0x01u /* plain page content */
#define HELIOSVIEW_CONTEXT_MENU_TARGET_SELECTION 0x02u /* text is selected (see selection_text) */
#define HELIOSVIEW_CONTEXT_MENU_TARGET_LINK      0x04u /* over a link (see link_url) */
#define HELIOSVIEW_CONTEXT_MENU_TARGET_IMAGE     0x08u /* over an image */
#define HELIOSVIEW_CONTEXT_MENU_TARGET_MEDIA     0x10u /* over audio/video content */
#define HELIOSVIEW_CONTEXT_MENU_TARGET_EDITABLE  0x20u /* inside an editable field */

/* The right-click request handed to the callback.
 *
 * Every string is UTF-8, never NULL ("" when the field does not apply) and valid
 * only for the duration of the callback — copy what you keep. */
typedef struct heliosview_context_menu_info {
    uint32_t target;            /* HELIOSVIEW_CONTEXT_MENU_TARGET_* bits */
    int32_t x;                  /* request position relative to the WebView's
                                 * top-left corner (the same space the WebView
                                 * bounds use); a native menu opened afterwards pops
                                 * at the current cursor anyway */
    int32_t y;
    const char* link_url;       /* the link under the cursor ("" when none) */
    const char* link_text;      /* that link's text ("" when none) */
    const char* selection_text; /* the selected text ("" when none) */
    const char* page_url;       /* the document's URL ("" when unknown) */
} heliosview_context_menu_info_t;

/* The right-click callback. Runs on the message-loop thread, just before the
 * engine would open its own menu. Return non-zero to intercept this click (the
 * engine's menu stays closed); return 0 to fall through to
 * heliosview_webview_set_context_menu. */
typedef int (*heliosview_webview_context_menu_cb)(heliosview_webview_t* webview,
                                                  const heliosview_context_menu_info_t* info,
                                                  void* userdata);

/* Register the interception callback (replacing any previous one and running its
 * dtor). Pass NULL to clear it. Registering always succeeds: on an engine without a
 * context-menu hook the callback is simply never invoked (the switch call is where
 * such a port reports that). UI-thread call. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_context_menu_callback(
    heliosview_webview_t* webview,
    heliosview_webview_context_menu_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Enable (enabled != 0) or disable the engine's debugging tools - one switch
 * that says "debugging is allowed", deliberately not "open" or "attach":
 *
 *   Windows   WebView2 AreDevToolsEnabled (right-click -> Inspect; F12 is off
 *             either way, see open_devtools).
 *   Linux     WebKitGTK enable-developer-extras.
 *   macOS     WKWebView.isInspectable (macOS 13.3+): whether Safari's Develop
 *             menu may attach to this WebView. Older macOS returns -4.
 *
 * Enabled by default. When disabled, DevTools cannot be opened and an
 * already-open DevTools window is closed. Applies immediately when the WebView
 * is initialized; when called during initialization the setting is applied when
 * it becomes ready. Message-loop thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_devtools(heliosview_webview_t* webview,
                                                   int enabled);

/* Open the engine's DevTools window programmatically (like a menu item in
 * VS Code). This is the only way in: the library never leaves the engine's own
 * browser shortcuts (F12, Ctrl+Shift+I, Ctrl+P, F5, ...) available - see the
 * platform notes in the README. Windows (OpenDevToolsWindow) and Linux
 * (WebKitWebInspector) implement it; macOS has no public API to open the
 * inspector and returns -4 - there it is reached from Safari's Develop menu (see
 * heliosview_webview_set_devtools). Fails (negative) when DevTools are disabled
 * or the WebView is not initialized yet. Message-loop thread. 0 = success,
 * negative = error. */
HELIOSVIEW_API int heliosview_webview_open_devtools(heliosview_webview_t* webview);

/* ================= Cookie store =================
 *
 * The WebView's own cookie jar (the WebView2 user-data folder / WebKit data
 * store), not the HTTP client's: logins the page performs land here, and these
 * calls are how an app reads them, seeds them, or clears them. Every call is
 * asynchronous; the callback runs on the message-loop thread. Per platform:
 * WebView2 ICoreWebView2CookieManager, macOS WKHTTPCookieStore, WebKitGTK
 * WebKitCookieManager (all public, all three platforms).
 *
 * The `url` scopes the operation: "" or NULL means "every cookie in the store"
 * for get_cookies, and the library-wide default for delete_cookie. */
typedef struct heliosview_webview_cookie {
    const char* name;         /* never NULL */
    const char* value;        /* never NULL ("" for a valueless cookie) */
    const char* domain;       /* never NULL ("" when the engine reports none) */
    const char* path;         /* never NULL ("" when the engine reports none) */
    int is_secure;
    int is_http_only;
    int is_session;           /* 1 = session cookie (expires_unix is then 0) */
    double expires_unix;      /* seconds since the Unix epoch; 0 = session */
} heliosview_webview_cookie_t;

/* Completion of get_cookies: the array and every string in it are owned by the
 * library and valid only for the duration of the call - copy what you keep.
 * error != 0 means the array is empty. */
typedef void (*heliosview_webview_cookies_cb)(int error,
                                              const heliosview_webview_cookie_t* cookies,
                                              size_t count, void* userdata);

/* Completion of a set / delete / clear: error is 0 when the store accepted it. */
typedef void (*heliosview_webview_cookie_op_cb)(int error, void* userdata);

/* Read the cookies matching url ("" / NULL = all of them).
 * 0 = success (the request was accepted), negative = error. */
HELIOSVIEW_API int heliosview_webview_get_cookies(heliosview_webview_t* webview, const char* url,
                                                  heliosview_webview_cookies_cb callback,
                                                  void* userdata);

/* Add or update one cookie. `url` is the document URL the cookie belongs to
 * (its host is the default when cookie->domain is ""); name and value come from
 * cookie. expires_unix == 0 && is_session == 0 means "keep it for the session".
 * 0 = success (request accepted), negative = error. */
HELIOSVIEW_API int heliosview_webview_set_cookie(heliosview_webview_t* webview, const char* url,
                                                 const heliosview_webview_cookie_t* cookie,
                                                 heliosview_webview_cookie_op_cb callback,
                                                 void* userdata);

/* Delete every cookie with this name in url's scope ("" / NULL url = the whole
 * store). 0 = success (request accepted), negative = error. */
HELIOSVIEW_API int heliosview_webview_delete_cookie(heliosview_webview_t* webview, const char* name,
                                                    const char* url,
                                                    heliosview_webview_cookie_op_cb callback,
                                                    void* userdata);

/* Delete every cookie in the store (a logout that must not leave a session
 * behind). 0 = success (request accepted), negative = error. */
HELIOSVIEW_API int heliosview_webview_clear_cookies(heliosview_webview_t* webview,
                                                    heliosview_webview_cookie_op_cb callback,
                                                    void* userdata);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_WEBVIEW_H */
