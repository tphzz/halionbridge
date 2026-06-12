#pragma once

#include "halionbridge/halionbridge_export.h"

namespace halionbridge
{

HALIONBRIDGE_EXPORT void installCrashDiagnostics();

// Stores a phase breadcrumb for crash dumps. The pointer must remain valid for
// the life of the process; pass a string literal or another immortal string.
HALIONBRIDGE_EXPORT void setCrashDiagnosticPhase(const char* phase);

} // namespace halionbridge
