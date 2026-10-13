// Tests for daisy/failsafe.h (the pedal's health check and load shedding).
//   make failsafe_test && ./failsafe_test
#include "../daisy/failsafe.h"
#include <cstdio>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

int main() {
    const float sr = 48000.f; const size_t n = 48;
    std::vector<float> in(n), L(n), R(n);
    auto fill = [&](float inAmp, float outAmp) { for (size_t i = 0; i < n; i++) { in[i] = inAmp * std::sin(0.05f * i); L[i] = R[i] = outAmp * std::sin(0.05f * i); } };

    rasp_pedal::HealthCheck h; h.init(sr, n);
    for (int b = 0; b < 2000; b++) { fill(0.1f, 0.3f); h.check(in.data(), L.data(), R.data(), n, false); }
    check(h.healthy(), "normal audio stays healthy");

    fill(0.1f, 0.3f); L[7] = NAN; h.check(in.data(), L.data(), R.data(), n, false);
    check(!h.healthy(), "a NaN faults at once");
    bool muted = true; for (float v : R) muted = muted && v == 0.f;
    check(muted, "the faulted block is muted");
    h.recovered(); check(h.healthy() && h.faultCount() == 1, "recovered() clears it and counts it");

    for (int b = 0; b < 900; b++) { fill(0.1f, 0.f); h.check(in.data(), L.data(), R.data(), n, false); }
    check(h.healthy(), "0.9 s of silent output under a voice: still healthy");
    for (int b = 0; b < 200; b++) { fill(0.1f, 0.f); h.check(in.data(), L.data(), R.data(), n, false); }
    check(!h.healthy(), "over 1 s of silent output under a voice: fault");
    h.recovered();
    for (int b = 0; b < 5000; b++) { fill(0.1f, 0.f); h.check(in.data(), L.data(), R.data(), n, true); }
    check(h.healthy(), "silence is fine while a gate / tape stop is held");
    for (int b = 0; b < 5000; b++) { fill(0.f, 0.f); h.check(in.data(), L.data(), R.data(), n, false); }
    check(h.healthy(), "silence is fine when nobody sings");

    rasp_pedal::LoadShedder s; uint32_t t = 0;
    rasp::Params p; p.harm3Level = 0.5f; p.octDown = 0.5f; p.freeze = true;
    for (; t < 400; t += 10) s.update(0.95f, t);
    check(s.level() == 0, "a 0.4 s load spike sheds nothing");
    for (; t < 2000; t += 10) s.update(0.95f, t);
    check(s.level() == 3, "1.6 s at 95% sheds all three steps");
    rasp::Params q = s.apply(p);
    check(q.harm3Level == 0.f && q.octDown == 0.f && !q.freeze && !q.thickenOn, "shed layers are off in the applied params");
    check(p.harm3Level == 0.5f, "the stored params are untouched");
    for (; t < 6000; t += 10) s.update(0.5f, t);
    check(s.level() == 3, "under 5 s calm: nothing restored yet");
    for (; t < 30000; t += 10) s.update(0.5f, t);
    check(s.level() == 0, "after enough calm every step comes back");

    std::printf(fails ? "\n%d failed\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
