#pragma once

/**
 * HeliosView.Core -- WebViewWindow compatibility header.
 *
 * WebView and Viewport Host capabilities have been unified directly into helios::Window.
 * helios::WebViewWindow is an alias for helios::Window for backwards compatibility.
 */

#include <HeliosViewCore/Window.h>

namespace helios {

// Backwards-compatibility alias: Window now natively encapsulates WebView capabilities.
using WebViewWindow = Window;

} // namespace helios