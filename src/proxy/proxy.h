#pragma once

#include <windows.h>

namespace lazysf2::proxy {

// Loads the real 32-bit system winmm.dll and fills the export jump table.
// Returns false if the real DLL could not be loaded; every stub would then crash, so the caller
// should fail DLL load.
bool init();
void shutdown();
// The real system winmm.dll that init() loaded, or nullptr before init(). Never freed. Our own DLL is named
// winmm.dll too, so code inside it must not import from winmm.dll: it looks functions up here instead.
HMODULE real_module();

}  // namespace lazysf2::proxy
