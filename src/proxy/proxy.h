#pragma once

#include <windows.h>

namespace LazyWorks::SF2Patch::proxy {

/// <summary>
/// Loads the real 32-bit system winmm.dll and fills the export jump table.
/// Returns false if the real DLL could not be loaded; every stub would then crash, so the caller
/// should fail DLL load.
/// </summary>
bool init();
void shutdown();
/// <summary>
/// The real system winmm.dll that init() loaded, or nullptr before init(). Never freed. Our own DLL is named
/// winmm.dll too, so code inside it must not import from winmm.dll: it looks functions up here instead.
/// </summary>
HMODULE real_module();

}  // namespace LazyWorks::SF2Patch::proxy
