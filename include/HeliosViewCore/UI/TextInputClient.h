#pragma once

#include <string_view>
#include <cstddef>
#include <cstdint>
#include <HeliosViewCore/Canvas.h>

namespace HeliosView::UI {

/**
 * Text input semantic type, used by platform input contexts to configure
 * input method behavior (e.g. disabling dictionary learning for passwords).
 */
enum class TextInputType {
    Text,        // Standard single-line or normal text
    Password,    // Password / sensitive input (suppress IME prediction and logging)
    Number,      // Numeric input (digits, decimal, signs)
    Multiline,   // Multiline editing text area
};

/**
 * Character range within a text buffer, measured in byte or code-point offsets.
 */
struct TextRange {
    size_t start = 0;
    size_t end = 0;

    constexpr bool empty() const noexcept { return start == end; }
    constexpr size_t length() const noexcept {
        return end >= start ? (end - start) : (start - end);
    }
};

/**
 * TextInputClient: Platform-independent abstraction for any UI element
 * capable of receiving text input and participating in IME composition sessions.
 *
 * Implements a bidirectional contract between the editing widget and the platform:
 *  - Downstream: Platform delivers committed text, composition updates, or edits.
 *  - Upstream (Pull): Platform queries surrounding text, selections, and pixel bounding boxes.
 */
class TextInputClient {
public:
    virtual ~TextInputClient() = default;

    // ---- Downstream (Platform -> Client mutations) ----

    /**
     * Insert committed text (from physical typing, clipboard paste, or IME candidate selection).
     */
    virtual void insertText(std::string_view utf8) = 0;

    /**
     * Update the active inline composition (pre-edit) text.
     * @param utf8 The composition string to preview at the caret. Empty to clear.
     * @param cursorInComp Caret position within the composition preview, or -1 for default.
     */
    virtual void setComposition(std::string_view utf8, int cursorInComp = -1) = 0;

    /**
     * Confirm the current composition string, committing it into the value.
     */
    virtual void confirmComposition() = 0;

    /**
     * Cancel and discard the current composition without committing it.
     */
    virtual void cancelComposition() = 0;

    /**
     * Delete surrounding text around the cursor (used by smart IMEs for auto-correction).
     */
    virtual void deleteSurroundingText(size_t beforeChars, size_t afterChars) = 0;

    // ---- Upstream (Client -> Platform queries) ----

    /**
     * The full current text content.
     */
    virtual const std::string& text() const = 0;

    /**
     * Current selection or cursor position (start == end means collapsed caret).
     */
    virtual TextRange selection() const = 0;

    /**
     * The caret's bounding box in host window client coordinates (pixels).
     * Used by the OS IME to position its candidate window adjacent to the caret.
     */
    virtual helios::Rect caretHostRect() const = 0;

    /**
     * Return the input semantic type of this client.
     */
    virtual TextInputType inputType() const { return TextInputType::Text; }
};

} // namespace HeliosView::UI
