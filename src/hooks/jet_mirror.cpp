// The players' jet controls (Phase 5): see jet_mirror.h. Fixed offsets, July 2013 AircraftObject.dll only.
#include "hooks/jet_mirror.h"

#include <windows.h>

#include <cstring>

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

// The engine's slot 22 (set running) if it is the function we read. SEH only.
void* engine_setter(const BYTE* eng, uintptr_t ao) {
    void** vt = *reinterpret_cast<void** const*>(eng);
    const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
    if (v < ao || v >= ao + jet::kAoSize) return nullptr;
    void* f = vt[jet::kEngSetRunningSlot];
    return reinterpret_cast<uintptr_t>(f) == ao + jet::kRvaEngSetRunning ? f : nullptr;
}

}  // namespace

bool jet_read_controls(const void* obj, JetControls* out) {
    const uintptr_t ao = ao_base();
    if (!ao || !obj) return false;
    JetControls c;
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        const float* in = *reinterpret_cast<const float* const*>(o + jet::kInputs);
        if (!in) return false;
        c.throttle = in[jet::kInputThrottle];
        int16_t n = 0;
        std::memcpy(&n, o + jet::kEngineCount, 2);
        if (n < 0) n = 0;
        if (n > 8) n = 8;
        c.engine_count = static_cast<uint8_t>(n);
        for (int i = 0; i < n; ++i) {
            const BYTE* e = *reinterpret_cast<const BYTE* const*>(o + jet::kEngines + 4 * i);
            int16_t run = 1;  // an engine we cannot read is reported running, so no avatar switches it off
            if (e && engine_setter(e, ao)) std::memcpy(&run, e + jet::kEngRunning, 2);
            if (run) c.engines |= static_cast<uint8_t>(1u << i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *out = c;
    return true;
}

JetWriteResult jet_write_controls(void* obj, const JetControls& c) {
    JetWriteResult r;
    const uintptr_t ao = ao_base();
    if (!ao || !obj) return r;
    __try {
        BYTE* o = static_cast<BYTE*>(obj);
        float* in = *reinterpret_cast<float* const*>(o + jet::kInputs);
        if (!in) return r;
        in[jet::kInputThrottle] = c.throttle;
        r.ok = true;
        int16_t n = 0;
        std::memcpy(&n, o + jet::kEngineCount, 2);
        for (int i = 0; i < n && i < 8; ++i) {
            BYTE* e = *reinterpret_cast<BYTE* const*>(o + jet::kEngines + 4 * i);
            if (!e) continue;
            void* set = engine_setter(e, ao);
            if (!set) {
                r.engines_refused |= static_cast<uint8_t>(1u << i);
                continue;
            }
            int16_t run = 0;
            std::memcpy(&run, e + jet::kEngRunning, 2);
            const bool want = (c.engines >> i) & 1u;
            if ((run != 0) == want) continue;
            reinterpret_cast<void(__fastcall*)(void*, void*, int)>(set)(e, nullptr, want ? 1 : 0);
            r.engines_set |= static_cast<uint8_t>(1u << i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

void jet_takeover(void* obj) {
    if (!ao_base() || !obj) return;
    __try {
        float* in = *reinterpret_cast<float* const*>(static_cast<BYTE*>(obj) + jet::kInputs);
        if (!in) return;
        in[jet::kInputGun1] = 0.0f;
        in[jet::kInputGun2] = 0.0f;
        in[jet::kInputFire] = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
