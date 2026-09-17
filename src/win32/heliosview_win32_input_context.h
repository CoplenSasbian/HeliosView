#pragma once

#include <HeliosViewCore/UI/PlatformInputContext.h>
#include <HeliosView/heliosview_ui.h>
#include "heliosview_win32_internal.h"
#include <imm.h>
#include <string>
#include <string_view>
#include <vector>

namespace hv::win32 {

/**
 * Win32InputContext: Windows platform implementation of PlatformInputContext.
 *
 * Encapsulates the Win32 Imm32 (Input Method Manager) subsystem:
 *  - Handles WM_IME_* message processing and composition state transitions.
 *  - Manages composition strings (GCS_COMPSTR) and commit results (GCS_RESULTSTR).
 *  - Positions IME composition and candidate windows at the active client's caret box.
 *  - Filters and normalizes WM_CHAR / WM_IME_CHAR dual-delivery streams.
 */
class Win32InputContext : public HeliosView::PlatformInputContext {
public:
    explicit Win32InputContext(heliosview_host_t* host, HWND hwnd)
        : m_host(host), m_hwnd(hwnd) {}

    ~Win32InputContext() override {
        detachClient();
    }

    // ---- PlatformInputContext interface ----

    void attachClient(HeliosView::UI::TextInputClient* client) override {
        if (m_client == client) return;

        if (m_client && m_isComposing) {
            reset();
        }

        m_client = client;

        if (m_client) {
            updateCaretRect();
        }
    }

    void detachClient() override {
        if (!m_client) return;

        if (m_isComposing) {
            reset();
        }
        m_client = nullptr;
    }

    HeliosView::UI::TextInputClient* activeClient() const noexcept override {
        return m_client;
    }

    void updateCaretRect() override {
        if (!m_hwnd) return;

        int x = 0;
        int y = 0;
        int height = 18;

        if (m_client) {
            const helios::Rect r = m_client->caretHostRect();
            x = r.x;
            y = r.y;
            if (r.height > 0) height = r.height;
        } else {
            x = m_fallbackX;
            y = m_fallbackY;
            if (m_fallbackHeight > 0) height = m_fallbackHeight;
        }

        placeIme(x, y, height);
    }

    void setFallbackCaret(int x, int y, int height) {
        m_fallbackX = x;
        m_fallbackY = y;
        m_fallbackHeight = height;
        placeIme(x, y, height);
    }

    void reset() override {
        if (m_hwnd) {
            HIMC imc = ImmGetContext(m_hwnd);
            if (imc) {
                ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(m_hwnd, imc);
            }
        }
        m_isComposing = false;
        if (m_client) {
            m_client->cancelComposition();
        }
        if (m_host) {
            heliosview_host_ui_dispatch_composition(m_host, "");
        }
    }

    bool isComposing() const noexcept override {
        return m_isComposing;
    }

    // ---- Win32 message hook for UiSubclassProc ----

    bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM& lp, LRESULT* outResult) {
        switch (msg) {
        case WM_CHAR: {
            const wchar_t unit = static_cast<wchar_t>(wp);
            if (!acceptsChar(unit)) {
                *outResult = 0;
                return true;
            }
            deliverChar(unit, 0);
            *outResult = 0;
            return true;
        }

        case WM_IME_CHAR: {
            const wchar_t unit = static_cast<wchar_t>(wp);
            if (!acceptsChar(unit)) {
                *outResult = 0;
                return true;
            }
            deliverChar(unit, 1);
            *outResult = 0;
            return true;
        }

        case WM_IME_SETCONTEXT: {
            if (wp) {
                // Widget draws inline composition itself, so suppress OS composition window.
                // Leave candidate window alone so IME candidate choices are displayed.
                lp &= ~ISC_SHOWUICOMPOSITIONWINDOW;
            }
            return false; // let DefSubclassProc proceed with modified lp
        }

        case WM_IME_STARTCOMPOSITION: {
            m_isComposing = true;
            updateCaretRect();
            *outResult = 0;
            return true;
        }

        case WM_IME_COMPOSITION: {
            HIMC imc = ImmGetContext(hwnd);
            if (!imc) {
                *outResult = 0;
                return true;
            }

            bool committed = false;
            if ((lp & GCS_RESULTSTR) != 0) {
                const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
                if (bytes > 0) {
                    std::wstring wide(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(imc, GCS_RESULTSTR, wide.data(), static_cast<DWORD>(bytes));
                    for (wchar_t unit : wide) {
                        deliverChar(unit, 1);
                    }
                    committed = true;
                    m_isComposing = false;
                }
            }

            if (committed) {
                if (m_host) {
                    heliosview_host_ui_dispatch_composition(m_host, "");
                } else if (m_client) {
                    m_client->setComposition("", 0);
                }
            } else if ((lp & GCS_COMPSTR) != 0) {
                const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                if (bytes >= 0) {
                    std::wstring wide(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                    if (bytes > 0) {
                        ImmGetCompositionStringW(imc, GCS_COMPSTR, wide.data(), static_cast<DWORD>(bytes));
                    }

                    std::string utf8;
                    utf8.reserve(wide.size() * 3 + 1);
                    for (size_t i = 0; i < wide.size(); ++i) {
                        char buf[8] = {};
                        wchar_t high = 0;
                        int n = utf8FromUnit(wide[i], &high, buf);
                        if (n == 0 && high != 0 && i + 1 < wide.size()) {
                            n = utf8FromUnit(wide[++i], &high, buf);
                        }
                        if (n > 0) utf8.append(buf, static_cast<size_t>(n));
                    }

                    LONG cursorVal = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
                    int cursorInComp = (cursorVal >= 0) ? static_cast<int>(cursorVal) : -1;

                    if (m_host) {
                        heliosview_host_ui_dispatch_composition(m_host, utf8.c_str());
                    } else if (m_client) {
                        m_client->setComposition(utf8, cursorInComp);
                    }
                }
            }

            ImmReleaseContext(hwnd, imc);
            *outResult = 0;
            return true;
        }

        case WM_IME_ENDCOMPOSITION: {
            m_isComposing = false;
            if (m_host) {
                heliosview_host_ui_dispatch_composition(m_host, "");
            } else if (m_client) {
                m_client->setComposition("", 0);
            }
            *outResult = 0;
            return true;
        }


        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (m_isComposing) {
                *outResult = 0;
                return true; // While composing, keys belong strictly to the IME
            }
            return false;
        }

        default:
            break;
        }

        return false;
    }

private:
    static bool acceptsChar(wchar_t unit) {
        return unit >= 0x20 && unit != 0x7F;
    }

    static int utf8FromUnit(wchar_t unit, wchar_t* pending_high, char* out) {
        auto encode = [](uint32_t cp, char* dst) -> int {
            if (cp < 0x80) {
                dst[0] = static_cast<char>(cp);
                return 1;
            }
            if (cp < 0x800) {
                dst[0] = static_cast<char>(0xC0 | (cp >> 6));
                dst[1] = static_cast<char>(0x80 | (cp & 0x3F));
                return 2;
            }
            if (cp < 0x10000) {
                dst[0] = static_cast<char>(0xE0 | (cp >> 12));
                dst[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                dst[2] = static_cast<char>(0x80 | (cp & 0x3F));
                return 3;
            }
            dst[0] = static_cast<char>(0xF0 | (cp >> 18));
            dst[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            dst[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            dst[3] = static_cast<char>(0x80 | (cp & 0x3F));
            return 4;
        };

        if (*pending_high != 0) {
            const wchar_t high = *pending_high;
            *pending_high = 0;
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                const uint32_t cp = 0x10000u + ((static_cast<uint32_t>(high) - 0xD800u) << 10)
                                              + (static_cast<uint32_t>(unit) - 0xDC00u);
                return encode(cp, out);
            }
            encode(0xFFFD, out);
            return 0;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            *pending_high = unit;
            return 0;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF)
            return encode(0xFFFD, out);
        return encode(static_cast<uint32_t>(unit), out);
    }

    void deliverChar(wchar_t unit, int channel) {
        if (m_lastChar[1 - channel] == unit) {
            m_lastChar[1 - channel] = 0;
            return;
        }
        m_lastChar[channel] = unit;
        m_lastChar[1 - channel] = 0;

        char utf8[8] = {};
        const int len = utf8FromUnit(unit, &m_pendingHigh, utf8);
        if (len > 0) {
            utf8[len] = '\0';
            if (m_host) {
                heliosview_host_ui_dispatch_text(m_host, utf8);
            } else if (m_client) {
                m_client->insertText(utf8);
            }
            return;
        }
        if (m_pendingHigh == 0 && unit >= 0xDC00 && unit <= 0xDFFF) {
            if (m_host) {
                heliosview_host_ui_dispatch_text(m_host, "\xEF\xBF\xBD");
            } else if (m_client) {
                m_client->insertText("\xEF\xBF\xBD");
            }
        }
    }


    void placeIme(int x, int y, int height) {
        if (!m_hwnd) return;
        HIMC imc = ImmGetContext(m_hwnd);
        if (!imc) return;

        if (height <= 0) height = 18;
        const int base = (y >= 0 && height > 0) ? y + height : 0;

        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = x;
        cf.ptCurrentPos.y = base;
        ImmSetCompositionWindow(imc, &cf);

        CANDIDATEFORM cand{};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos.x = x;
        cand.ptCurrentPos.y = base;
        cand.rcArea.left = x;
        cand.rcArea.top = 0;
        cand.rcArea.right = x;
        cand.rcArea.bottom = base;
        ImmSetCandidateWindow(imc, &cand);

        ImmReleaseContext(m_hwnd, imc);
    }

    heliosview_host_t* m_host = nullptr;
    HWND m_hwnd = nullptr;
    HeliosView::UI::TextInputClient* m_client = nullptr;

    bool m_isComposing = false;
    wchar_t m_pendingHigh = 0;
    wchar_t m_lastChar[2] = {0, 0};

    int m_fallbackX = 0;
    int m_fallbackY = 0;
    int m_fallbackHeight = 18;
};

} // namespace hv::win32
