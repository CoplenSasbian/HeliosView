// HeliosView.dll - Windows notification (toast) backend.
//
// Unpackaged Win32 apps show Windows toasts through the WinRT
// ToastNotificationManager. The steps are:
//   1. RoInitialize (MTA) once,
//   2. SetCurrentProcessExplicitAppUserModelID + a Start Menu shortcut carrying
//      the AppUserModelID property (required for unpackaged apps),
//   3. RoGetActivationFactory for the ToastNotificationManager statics,
//   4. build a ToastText02 XML document, wrap it in a ToastNotification, Show().
//
// The implementation uses the raw WRL ABI headers that ship with the Windows SDK
// (roapi.h / wrl / windows.ui.notifications.h / windows.data.xml.dom.h) - no
// C++/WinRT, no NuGet package, nothing beyond the SDK itself.
//
// Unlike the rest of the library these functions are thread-agnostic: toasts are
// not tied to a window or the message-loop thread, so a worker thread can report
// a finished background task directly.
//
// THREADING: WinRT objects are apartment-bound, and an apartment dies with the
// thread that created it — a notifier created on a short-lived worker thread is
// released when that thread exits. So every notifier/toast lives on ONE
// dedicated thread (worker below) that stays alive for the process lifetime;
// the public functions post to it and wait for the result. This keeps the
// "call from any thread" contract without forcing an apartment model onto the
// caller (the message-loop thread must stay STA for WebView2).
// The permission and click callbacks therefore run on that thread — an
// unspecified thread, as documented in heliosview.h.

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h" /* hv_fail */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <wrl/client.h>
#include <wrl/event.h>      /* Callback<>: the Activated / Dismissed event handlers */
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.foundation.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <shlobj.h>   /* IShellLinkW / CLSID_ShellLink */
#include <propsys.h>  /* IPropertyStore / InitPropVariantFromString */
#include <propkey.h>  /* PKEY_AppUserModel_ID */
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

using ABI::Windows::UI::Notifications::IToastNotification;
using ABI::Windows::UI::Notifications::IToastNotificationFactory;
using ABI::Windows::UI::Notifications::IToastNotificationManagerStatics;
using ABI::Windows::UI::Notifications::IToastNotifier;
using ABI::Windows::UI::Notifications::IToastDismissedEventArgs;
using ABI::Windows::Data::Xml::Dom::IXmlDocument;
using ABI::Windows::Data::Xml::Dom::IXmlDocumentIO;

/* The concrete MIDL-specialized delegate types (the generic
 * ITypedEventHandler<...> template is not instantiable). */
using toast_activated_handler = ABI::Windows::Foundation::
    __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable_t;
using toast_dismissed_handler = ABI::Windows::Foundation::
    __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs_t;

namespace {

std::wstring utf8_to_wide(const char* s)
{
    if (!s || !*s)
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            s.data(), n, nullptr, nullptr);
    return s;
}

/* Escape XML text so user-provided titles/bodies cannot break the toast markup. */
std::wstring xml_escape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 16);
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  out += L"&amp;"; break;
        case L'<':  out += L"&lt;"; break;
        case L'>':  out += L"&gt;"; break;
        case L'"':  out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default:    out += c; break;
        }
    }
    return out;
}

std::wstring exe_path()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    return std::wstring(buf, n);
}

/* Sanitize an AppUserModelID into a safe .lnk file base name. */
std::wstring shortcut_base(const std::wstring& app_id)
{
    std::wstring base = app_id;
    for (wchar_t& c : base)
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?'
            || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    return base;
}

/* Worker-thread state. Only the notification thread touches `notifier`,
 * `app_id`, `init_result` and `initialized`; `permission` is atomic because it
 * is read from any thread. */
struct toast_state {
    bool initialized = false;
    HRESULT init_result = E_FAIL;
    ComPtr<IToastNotifier> notifier;
    std::wstring app_id;
    std::atomic<heliosview_notification_permission_t> permission{
        HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN};
};

toast_state g_state;

/* Click callback + the toasts kept alive to receive it. A shown toast is
 * referenced by the notifier, but activation is delivered to our event handler
 * only while our toast object is alive, so live toasts are retained here and
 * dropped on Dismissed. */
std::mutex g_toast_mutex;
heliosview_notification_click_cb g_click_cb = nullptr;
void* g_click_cb_userdata = nullptr;
std::vector<std::shared_ptr<struct toast_entry>> g_live_toasts;

struct toast_entry {
    ComPtr<IToastNotification> toast;
    std::wstring title;
    std::wstring body;
    EventRegistrationToken activated{};
    EventRegistrationToken dismissed{};
};

/* Ensure WinRT/COM is available on the calling thread WITHOUT changing the
 * thread's apartment model. The message-loop thread must stay an STA (WebView2
 * requires it), so a thread that already has an apartment (STA or MTA) is left
 * untouched; only a thread with no apartment yet (e.g. a fresh worker thread)
 * is initialized, as MTA. */
void ensure_com_mta()
{
    thread_local bool done = false;
    if (done)
        return;
    APTTYPE apt{};
    APTTYPEQUALIFIER qual{};
    if (FAILED(CoGetApartmentType(&apt, &qual)))
        RoInitialize(RO_INIT_MULTITHREADED); /* fresh thread: MTA */
    done = true;
}

/* The single notification thread. The WinRT notifier and every live toast are
 * apartment-bound and must outlive the (possibly short-lived) calling thread,
 * so they are created and used here and nowhere else. run() executes a task on
 * that thread and returns its result; when called from the thread itself it
 * runs inline (no deadlock). */
class notification_worker {
public:
    static notification_worker& instance()
    {
        static notification_worker worker; /* created on first use, lives to process exit */
        return worker;
    }

    template <class F>
    auto run(F&& task) -> decltype(task())
    {
        using result_t = decltype(task());
        if (std::this_thread::get_id() == m_thread.get_id())
            return task();
        auto promise = std::make_shared<std::promise<result_t>>();
        std::future<result_t> future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.emplace_back([promise, task = std::forward<F>(task)]() mutable {
                try {
                    promise->set_value(task());
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        }
        m_cv.notify_one();
        return future.get();
    }

private:
    notification_worker() : m_thread([this] { main(); }) {}

    ~notification_worker()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    void main()
    {
        ensure_com_mta(); /* this thread's apartment lives exactly as long as it does */
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            while (!m_queue.empty()) {
                std::function<void()> task = std::move(m_queue.front());
                m_queue.pop_front();
                lock.unlock();
                task();
                lock.lock();
            }
            if (m_stop)
                return;
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    bool m_stop = false;
};

/* Create the Start Menu shortcut carrying the AppUserModelID. Returns S_OK when
 * the shortcut exists (already there or just created). */
HRESULT ensure_shortcut(const std::wstring& app_id)
{
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH))
        return E_FAIL;
    const std::filesystem::path path = std::filesystem::path(appdata)
        / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs"
        / (shortcut_base(app_id) + L".lnk");
    if (std::filesystem::exists(path))
        return S_OK;

    const std::wstring exe = exe_path();
    if (exe.empty())
        return E_FAIL;

    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = link->SetPath(exe.c_str())))
        return hr;

    ComPtr<IPropertyStore> store;
    if (FAILED(hr = link.As(&store)))
        return hr;
    PROPVARIANT pv{};
    hr = InitPropVariantFromString(app_id.c_str(), &pv);
    if (FAILED(hr))
        return hr;
    store->SetValue(PKEY_AppUserModel_ID, pv);
    store->Commit();
    PropVariantClear(&pv);

    ComPtr<IPersistFile> persist;
    if (FAILED(hr = link.As(&persist)))
        return hr;
    return persist->Save(path.c_str(), TRUE);
}

/* Map the OS toast setting to the portable permission state. */
heliosview_notification_permission_t permission_from_setting(
    ABI::Windows::UI::Notifications::NotificationSetting setting)
{
    using ABI::Windows::UI::Notifications::NotificationSetting_Enabled;
    return setting == NotificationSetting_Enabled ? HELIOSVIEW_NOTIFICATION_PERMISSION_GRANTED
                                                  : HELIOSVIEW_NOTIFICATION_PERMISSION_DENIED;
}

/* Initialize once (idempotent). Runs on the first caller's thread. */
HRESULT initialize(const char* app_id_arg)
{
    ensure_com_mta();

    std::wstring app_id = utf8_to_wide(app_id_arg);
    if (app_id.empty())
        app_id = utf8_to_wide(heliosview_app_id()); /* heliosview_app_init */
    if (app_id.empty()) {
        const std::wstring exe = exe_path();
        const size_t slash = exe.find_last_of(L"\\/");
        const size_t dot = exe.find_last_of(L'.');
        if (slash != std::wstring::npos) {
            const size_t start = slash + 1;
            app_id = exe.substr(start, (dot != std::wstring::npos && dot > start) ? dot - start
                                                                                   : std::wstring::npos);
        }
    }
    if (app_id.empty())
        return E_INVALIDARG;

    SetCurrentProcessExplicitAppUserModelID(app_id.c_str());
    HRESULT hr = ensure_shortcut(app_id);
    if (FAILED(hr))
        return hr;

    ComPtr<IToastNotificationManagerStatics> mgr;
    hr = RoGetActivationFactory(
        HStringReference(L"Windows.UI.Notifications.ToastNotificationManager").Get(),
        IID_PPV_ARGS(&mgr));
    if (FAILED(hr))
        return hr;

    hr = mgr->CreateToastNotifierWithId(HStringReference(app_id.c_str()).Get(),
                                        g_state.notifier.GetAddressOf());
    if (FAILED(hr))
        return hr;
    g_state.app_id = app_id;
    return S_OK;
}

/* Read the OS setting into g_state.permission (no prompt on Windows).
 *
 * On the FIRST run of a freshly registered AppUserModelID the notification
 * platform has not picked the registration up yet and get_Setting fails for the
 * whole run; that is reported as UNKNOWN (see heliosview.h) rather than as an
 * error. Runs on the notification thread. */
heliosview_notification_permission_t refresh_permission()
{
    if (!g_state.notifier) {
        g_state.permission.store(HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN);
        return HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN;
    }
    ensure_com_mta();
    ABI::Windows::UI::Notifications::NotificationSetting setting{};
    const HRESULT hr = g_state.notifier->get_Setting(&setting);
    if (FAILED(hr)) {
        hv_fail(-1, "get_Setting failed — the AppUserModelID is not registered yet (first run?)");
        g_state.permission.store(HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN);
        return HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN;
    }
    const heliosview_notification_permission_t state = permission_from_setting(setting);
    g_state.permission.store(state);
    return state;
}

} // namespace

int heliosview_notification_init(const char* app_id)
{
    /* The notifier must be created on the notification thread (its apartment
     * dies with the thread that created it); the caller's apartment is never
     * touched, so the message-loop thread can stay an STA for WebView2. */
    return notification_worker::instance().run([app_id]() -> int {
        if (!g_state.initialized) {
            g_state.init_result = initialize(app_id);
            g_state.initialized = true;
        }
        if (FAILED(g_state.init_result))
            return -1;
        refresh_permission();
        return 0;
    });
}

heliosview_notification_permission_t heliosview_notification_permission_state(void)
{
    return g_state.permission.load();
}

int heliosview_notification_request_permission(heliosview_notification_permission_cb callback,
                                               void* userdata)
{
    return notification_worker::instance().run([callback, userdata]() -> int {
        if (!g_state.notifier)
            return hv_fail(-1, "heliosview_notification_init was not called");
        /* Windows has no permission prompt: report the current OS setting. */
        const heliosview_notification_permission_t state = refresh_permission();
        if (callback)
            callback(state, userdata);
        return 0;
    });
}

int heliosview_notification_set_click_callback(heliosview_notification_click_cb callback, void* userdata)
{
    std::lock_guard lock(g_toast_mutex);
    g_click_cb = callback;
    g_click_cb_userdata = userdata;
    return 0;
}

int heliosview_notification_show(const char* title, const char* body)
{
    /* Toast objects are apartment-bound too: build and show them on the
     * notification thread. */
    const std::string title_copy = title ? title : "";
    const std::string body_copy = body ? body : "";
    return notification_worker::instance().run([title_copy, body_copy]() -> int {
        if (!g_state.notifier)
            return -1;

        /* Build the toast XML from the ToastText02 template (escaped text). */
        const std::wstring wtitle = utf8_to_wide(title_copy.c_str());
        const std::wstring wbody = utf8_to_wide(body_copy.c_str());
        const std::wstring xml = L"<toast>"
            L"<visual><binding template='ToastText02'>"
            L"<text id='1'>" + xml_escape(wtitle) + L"</text>"
            L"<text id='2'>" + xml_escape(wbody) + L"</text>"
            L"</binding></visual></toast>";

        ComPtr<IXmlDocument> doc;
        {
            ComPtr<IInspectable> insp;
            HRESULT hr = RoActivateInstance(HStringReference(L"Windows.Data.Xml.Dom.XmlDocument").Get(),
                                            &insp);
            if (FAILED(hr))
                return -1;
            hr = insp.As(&doc);
            if (FAILED(hr))
                return -1;
        }

        ComPtr<IXmlDocumentIO> doc_io;
        if (FAILED(doc.As(&doc_io)))
            return -1;
        if (FAILED(doc_io->LoadXml(HStringReference(xml.c_str()).Get())))
            return -1;

        ComPtr<IToastNotificationFactory> factory;
        HRESULT hr = RoGetActivationFactory(
            HStringReference(L"Windows.UI.Notifications.ToastNotification").Get(),
            IID_PPV_ARGS(&factory));
        if (FAILED(hr))
            return -1;

        ComPtr<IToastNotification> toast;
        if (FAILED(factory->CreateToastNotification(doc.Get(), &toast)))
            return -1;

        /* Activation: keep the toast alive and route Activated/Dismissed so a
         * click reaches heliosview_notification_set_click_callback. */
        heliosview_notification_click_cb click_cb = nullptr;
        void* click_ud = nullptr;
        {
            std::lock_guard lock(g_toast_mutex);
            click_cb = g_click_cb;
            click_ud = g_click_cb_userdata;
        }
        if (click_cb) {
            auto entry = std::make_shared<toast_entry>();
            entry->toast = toast;
            entry->title = wtitle;
            entry->body = wbody;
            auto activated = Callback<toast_activated_handler>(
                [entry, click_ud](IToastNotification*, IInspectable*) -> HRESULT {
                    std::lock_guard lock(g_toast_mutex);
                    heliosview_notification_click_cb cb = g_click_cb;
                    if (cb) {
                        const std::string title_utf8 = wide_to_utf8(entry->title);
                        const std::string body_utf8 = wide_to_utf8(entry->body);
                        cb(title_utf8.c_str(), body_utf8.c_str(), click_ud);
                    }
                    return S_OK;
                });
            auto dismissed = Callback<toast_dismissed_handler>(
                [entry](IToastNotification*, IToastDismissedEventArgs*) -> HRESULT {
                    std::lock_guard lock(g_toast_mutex);
                    g_live_toasts.erase(std::remove(g_live_toasts.begin(), g_live_toasts.end(), entry),
                                        g_live_toasts.end());
                    return S_OK;
                });
            if (activated && dismissed
                && SUCCEEDED(toast->add_Activated(activated.Get(), &entry->activated))
                && SUCCEEDED(toast->add_Dismissed(dismissed.Get(), &entry->dismissed))) {
                std::lock_guard lock(g_toast_mutex);
                g_live_toasts.push_back(entry);
            }
        }

        hr = g_state.notifier->Show(toast.Get());
        return SUCCEEDED(hr) ? 0 : -1;
    });
}
