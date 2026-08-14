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

#include <HeliosView/heliosview.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <shlobj.h>   /* IShellLinkW / CLSID_ShellLink */
#include <propsys.h>  /* IPropertyStore / InitPropVariantFromString */
#include <propkey.h>  /* PKEY_AppUserModel_ID */
#include <propvarutil.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

using ABI::Windows::UI::Notifications::IToastNotification;
using ABI::Windows::UI::Notifications::IToastNotificationFactory;
using ABI::Windows::UI::Notifications::IToastNotificationManagerStatics;
using ABI::Windows::UI::Notifications::IToastNotifier;
using ABI::Windows::Data::Xml::Dom::IXmlDocument;
using ABI::Windows::Data::Xml::Dom::IXmlDocumentIO;

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

struct toast_state {
    std::once_flag once;
    HRESULT init_result = E_FAIL;
    ComPtr<IToastNotifier> notifier;
    std::wstring app_id;
};

toast_state g_state;

/* Ensure COM is initialized on the calling thread (MTA). */
void ensure_com_mta()
{
    thread_local bool done = false;
    if (!done) {
        RoInitialize(RO_INIT_MULTITHREADED);
        done = true;
    }
}

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

/* Initialize once (idempotent). Runs on the first caller's thread. */
HRESULT initialize(const char* app_user_model_id)
{
    ensure_com_mta();

    std::wstring app_id = utf8_to_wide(app_user_model_id);
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

    hr = mgr->CreateToastNotifier(&g_state.notifier);
    if (FAILED(hr))
        return hr;
    g_state.app_id = app_id;
    return S_OK;
}

} // namespace

int heliosview_notification_init(const char* app_user_model_id)
{
    std::call_once(g_state.once, [&] { g_state.init_result = initialize(app_user_model_id); });
    return SUCCEEDED(g_state.init_result) ? 0 : -1;
}

int heliosview_notification_show(const char* title, const char* body)
{
    if (!g_state.notifier)
        return -1;
    ensure_com_mta();

    /* Build the toast XML from the ToastText02 template (escaped text). */
    const std::wstring xml = L"<toast>"
        L"<visual><binding template='ToastText02'>"
        L"<text id='1'>" + xml_escape(utf8_to_wide(title)) + L"</text>"
        L"<text id='2'>" + xml_escape(utf8_to_wide(body)) + L"</text>"
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

    hr = g_state.notifier->Show(toast.Get());
    return SUCCEEDED(hr) ? 0 : -1;
}
