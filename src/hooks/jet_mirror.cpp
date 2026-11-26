// The players' jet controls (Phase 5): see jet_mirror.h. The memory access is in jet_mirror_core.cpp.
#include "hooks/jet_mirror.h"

#include <windows.h>

#include "hooks/hook_util.h"
#include "hooks/jet_layout.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

// Game thread. The build is checked (and a mismatch logged) once per loaded image.
uintptr_t ao_base() {
    static HMODULE checked = nullptr;
    static uintptr_t base = 0;
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (m != checked) {
        checked = m;
        base = m && is_expected_build(m, jet::kAoStamp, jet::kAoSize, "AircraftObject.dll (jet controls)")
                   ? reinterpret_cast<uintptr_t>(m)
                   : 0;
    }
    return base;
}

}  // namespace

bool jet_read_controls(const void* obj, JetControls* out) { return jet_read_controls_at(obj, ao_base(), out); }

JetWriteResult jet_write_controls(void* obj, const JetControls& c) {
    return jet_write_controls_at(obj, ao_base(), c, true);
}

bool jet_takeover(void* obj, JetControls* taken) { return jet_takeover_at(obj, ao_base(), taken); }

JetWriteResult jet_restore_engines(void* obj, uint8_t engines, uint8_t count) {
    JetControls c;
    c.engines = engines;
    c.engine_count = count;
    return jet_write_controls_at(obj, ao_base(), c, false);
}

}  // namespace LazyWorks::SF2Patch::hooks
