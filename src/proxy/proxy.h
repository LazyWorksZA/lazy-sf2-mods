#pragma once

namespace lazysf2::proxy {

// Loads the real 32-bit system winmm.dll and fills the export jump table.
// Returns false if the real DLL could not be loaded; every stub would then crash, so the caller
// should fail DLL load.
bool init();
void shutdown();

}  // namespace lazysf2::proxy
