// Timed RenderDoc capture trigger, driven by the `renderdoc_capture_at` cvar.

#pragma once

// Starts a thread that triggers RenderDoc captures at the configured times
// (seconds since this call). Does nothing when the cvar is empty, RenderDoc is
// not attached, or on non-Windows platforms.
void StartRenderDocTrigger();
