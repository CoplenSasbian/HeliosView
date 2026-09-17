#pragma once

#include <HeliosViewCore/UI/TextInputClient.h>

namespace HeliosView {

/**
 * PlatformInputContext: Abstract platform input method mediator.
 *
 * Sits between the host window / operating system input subsystem (Imm32, TSF,
 * Wayland text-input, macOS NSTextInputClient) and the active UI TextInputClient.
 *
 * Responsibilities:
 *  1. Attaching/detaching the active TextInputClient upon focus transitions.
 *  2. Relaying IME composition and commit events to the client.
 *  3. Querying the client's caret box and updating candidate window positions.
 *  4. Resetting/clearing OS composition state when focus is lost or cancelled.
 */
class PlatformInputContext {
public:
    virtual ~PlatformInputContext() = default;

    /**
     * Attach an active text editing client when it gains focus.
     */
    virtual void attachClient(UI::TextInputClient* client) = 0;

    /**
     * Detach the current client when it loses focus.
     */
    virtual void detachClient() = 0;

    /**
     * The currently attached client, or nullptr if none.
     */
    virtual UI::TextInputClient* activeClient() const noexcept = 0;

    /**
     * Notify the platform that the active client's caret box, selection,
     * or composition text has changed, so candidate lists can be repositioned.
     */
    virtual void updateCaretRect() = 0;

    /**
     * Force reset/cancel any in-flight composition session in the OS IME.
     */
    virtual void reset() = 0;

    /**
     * Whether an IME composition session is currently in progress.
     */
    virtual bool isComposing() const noexcept = 0;
};

} // namespace HeliosView
