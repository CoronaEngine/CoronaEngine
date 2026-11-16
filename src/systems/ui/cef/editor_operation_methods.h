#pragma once

// The operation enums the Web surface is named by, in one CEF-free header.
//
// `editor_operation_table.h` declares every name `window.corona` publishes, and a row names
// its operation with one of these enums. Each enum used to sit next to its own operations -
// window, script input, the bridge, the window events - so the table could not name them
// without pulling the CEF host into a header the argument readers cannot live in. The
// declarations live here instead, and their owners include this header.

namespace Corona::Systems::UI {

// Web names the surface it wants and the identity it means (CreatePanel + kind,
// Close + identity, SetWindowPresentation); the native host keeps the specialised
// transitions behind those names.
enum class WindowMethod { Close, Focus, CreatePanel, CreateCameraView,
                          SetWindowPresentation, ConfigureViewport, CloseProcess };

// Input is its own surface: the camera is the target, not the namespace.
enum class ScriptInputMethod { Dispatch };

/// The one Web -> Python entry.
enum class BridgeMethod { Call };

/// The window events a page can subscribe to; `onClosed` / `onCloseRequested` name these.
enum class NativeEventKind { Closed, CloseRequested };

}  // namespace Corona::Systems::UI
