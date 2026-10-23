// Scripted virtual Xbox 360 gamepad, driven by the `input_script` cvar.

#pragma once

#include <rex/runtime.h>

// Wraps config.input_factory so the scripted pad is added when input_script
// is non-empty. Does nothing otherwise.
void InstallInputScript(rex::RuntimeConfig& config);
