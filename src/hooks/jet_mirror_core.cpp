// The jet controls' memory access (see jet_mirror.h), with AircraftObject's base given. SEH-guarded, no logging, so
// the tests can run it on a fake aircraft. Fixed offsets, July 2013 AircraftObject.dll only.
#include <windows.h>

#include <cstring>

#include "hooks/jet_layout.h"
#include "hooks/jet_mirror.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

// The engine's slot 22 (set running) if it is the function we read. SEH only.
void* engine_setter(const BYTE* eng, uintptr_t ao) {
    void** vt = *reinterpret_cast<void** const*>(eng);
    const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
    if (v < ao || v >= ao + jet::kAoSize) return nullptr;
    void* f = vt[jet::kEngSetRunningSlot];
    return reinterpret_cast<uintptr_t>(f) == ao + jet::kRvaEngSetRunning ? f : nullptr;
}

int engine_count(const BYTE* o) {
    int16_t n = 0;
    std::memcpy(&n, o + jet::kEngineCount, 2);
    return n < 0 ? 0 : n > 8 ? 8 : n;
}

}  // namespace

bool jet_read_controls_at(const void* obj, uintptr_t ao, JetControls* out) {
    if (!ao || !obj) return false;
    JetControls c;
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        const float* in = *reinterpret_cast<const float* const*>(o + jet::kInputs);
        if (!in) return false;
        c.throttle = in[jet::kInputThrottle];
        if (in[jet::kInputGun1] > 0.0f) c.guns |= 1;
        if (in[jet::kInputGun2] > 0.0f) c.guns |= 2;
        const int n = engine_count(o);
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

JetWriteResult jet_write_controls_at(void* obj, uintptr_t ao, const JetControls& c, bool inputs) {
    JetWriteResult r;
    if (!ao || !obj) return r;
    __try {
        BYTE* o = static_cast<BYTE*>(obj);
        if (inputs) {
            float* in = *reinterpret_cast<float* const*>(o + jet::kInputs);
            if (!in) return r;
            for (size_t k = jet::kInputThrottle; k <= jet::kInputThrottleLast; ++k) in[k] = c.throttle;
            in[jet::kInputGun1] = (c.guns & 1) ? 1.0f : 0.0f;
            in[jet::kInputGun2] = (c.guns & 2) ? 1.0f : 0.0f;
        }
        r.ok = true;
        const int n = engine_count(o) < c.engine_count ? engine_count(o) : c.engine_count;
        for (int i = 0; i < n; ++i) {
            BYTE* e = *reinterpret_cast<BYTE* const*>(o + jet::kEngines + 4 * i);
            if (!e) continue;
            void* set = engine_setter(e, ao);
            if (!set) {
                r.engines_refused |= static_cast<uint8_t>(1u << i);
                continue;
            }
            int16_t run = 0;
            int32_t failed = 0;
            std::memcpy(&run, e + jet::kEngRunning, 2);
            std::memcpy(&failed, e + jet::kEngFire, 4);
            const bool want = (c.engines >> i) & 1u;
            if ((run != 0) == want || (want && failed)) continue;  // a failed engine is not restarted
            reinterpret_cast<void(__fastcall*)(void*, void*, int)>(set)(e, nullptr, want ? 1 : 0);
            r.engines_set |= static_cast<uint8_t>(1u << i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

bool jet_takeover_at(void* obj, uintptr_t ao, JetControls* taken) {
    if (!jet_read_controls_at(obj, ao, taken)) return false;
    __try {
        float* in = *reinterpret_cast<float* const*>(static_cast<BYTE*>(obj) + jet::kInputs);
        in[jet::kInputGun1] = 0.0f;
        in[jet::kInputGun2] = 0.0f;
        in[jet::kInputFire] = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

}  // namespace LazyWorks::SF2Patch::hooks
