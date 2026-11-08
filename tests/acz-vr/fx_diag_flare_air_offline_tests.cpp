// Offline tests for fx_diag_flare_air.h: flare candidate rules, bbox, rate limits, bounded formatters, line queue. Synthetic data only.
#include "fx_diag_flare_air.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace acz::fxdiag;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static FlareFacts facts(bool depth_en, uint32_t func, bool blend_on, uint8_t dst) {
    FlareFacts f; f.ps8 = 0xd34ee153u; f.vs8 = 0x534f29cau; f.pso_known = true; f.blend_on = blend_on; f.depth_enable = depth_en; f.depth_func = func;
    const uint8_t b[6] = {5, dst, 1, 1, 0, 1}; std::memcpy(f.blend, b, 6); f.count = 4; f.instances = 1; return f;
}
static Bbox box(float x0, float y0, float x1, float y1) { Bbox b; b.add(x0, y0); b.add(x1, y1); return b; }

int main() {
    // depth-always / additive predicates
    CHECK(depth_always(facts(true, kFuncAlways, true, 6)));
    CHECK(depth_always(facts(false, 4, true, 6)));          // depth test disabled counts as always
    CHECK(!depth_always(facts(true, 5, true, 6)));          // GREATER (smoke/sprites) is not
    CHECK(additive(facts(true, kFuncAlways, true, kBlendOne)));
    CHECK(!additive(facts(true, kFuncAlways, true, 6)));    // alpha-over
    CHECK(!additive(facts(true, kFuncAlways, false, kBlendOne)));   // blend off
    FlareFacts unk = facts(true, kFuncAlways, true, kBlendOne); unk.pso_known = false;
    CHECK(!additive(unk) && !flare_prefilter(unk));
    CHECK(!flare_prefilter(facts(true, 5, true, kBlendOne)));

    // bbox + centre (either coordinate convention)
    Bbox b; CHECK(!near_centre(b));
    b.add(std::numeric_limits<float>::quiet_NaN(), 1.0f); CHECK(b.n == 0);
    CHECK(near_centre(box(2000, 2000, 2100, 2100)));        // raw GS space
    CHECK(near_centre(box(300, 200, 340, 240)));            // offset removed
    CHECK(near_centre(box(0, 0, 640, 448)));                // full-screen quad covers the centre
    CHECK(!near_centre(box(2300, 2300, 2400, 2400)));
    CHECK(!near_centre(box(560, 20, 620, 60)));             // top-right HUD corner
    const FlareFacts add = facts(true, kFuncAlways, true, kBlendOne), alpha = facts(true, kFuncAlways, true, 6);
    CHECK(flare_candidate(add, box(2300, 2300, 2400, 2400)));    // additive anywhere
    CHECK(flare_candidate(alpha, box(300, 200, 340, 240)));      // centre, alpha-over
    CHECK(!flare_candidate(alpha, box(560, 20, 620, 60)));
    CHECK(!flare_candidate(facts(true, 5, true, kBlendOne), box(300, 200, 340, 240)));
    CHECK(flare_sig_key(add) != flare_sig_key(alpha));

    // limiter: read budget, window line cap, per-signature cap, additive allowance, run cap, suppressed accounting
    Limiter l; l.arm_delay_ms = 0; unsigned reads = 0;
    for (unsigned i = 0; i < 1000; ++i) if (l.allow_read(5000)) ++reads;
    CHECK(reads == Limiter::kReadsPerWindow);
    CHECK(l.allow_read(6001));                              // new window resets the budget
    Limiter m; m.arm_delay_ms = 0; unsigned logged = 0;
    for (unsigned i = 0; i < 50; ++i) if (m.allow_line(1000, 7, false)) ++logged;
    CHECK(logged == Limiter::kPerSigLines);
    logged = 0; for (unsigned i = 0; i < 50; ++i) if (m.allow_line(1000, 8, true)) ++logged;
    CHECK(logged == Limiter::kPerSigAdditive);              // 12 additive; window total 18 < 24
    logged = 0; for (unsigned s = 100; s < 140; ++s) if (m.allow_line(1000, s, false)) ++logged;
    CHECK(logged == Limiter::kLinesPerWindow - Limiter::kPerSigLines - Limiter::kPerSigAdditive);
    CHECK(m.suppressed > 0 && m.take_suppressed() > 0 && m.take_suppressed() == 0);
    CHECK(m.allow_line(2500, 7, false));                    // next window opens again
    Limiter r; r.arm_delay_ms = 0; uint64_t total = 0;
    for (unsigned w = 0; w < 100; ++w) for (unsigned k = 0; k < 40; ++k) if (r.allow_line(1000 + w * 1000, 1000u + k % 20u, false)) ++total;
    CHECK(total == Limiter::kRunLines && r.run_lines == Limiter::kRunLines);

    // start gate (batch K: the 400-line cap was consumed on the runway): nothing is logged, counted against the run cap or read before arming
    {
        Limiter g; CHECK(Limiter::kArmDelayMs == 90000 && g.arm_delay_ms == Limiter::kArmDelayMs && !g.armed);
        uint64_t grounded = 0;
        for (uint64_t t = 1000; t < 1000 + 60000; t += 100) {             // 60 s on the runway, 10 draws/s, all candidates
            if (g.allow_read(t)) ++grounded;
            if (g.allow_line(t, t % 7, true)) ++grounded;
        }
        CHECK(grounded == 0 && !g.armed && g.run_lines == 0 && g.held > 0 && g.suppressed == 0 && g.considered == 600);
        // fallback delay: armed kArmDelayMs after the first draw, counters start at zero, the full run cap is available in flight
        uint64_t t = 1000 + 89900; CHECK(!g.allow_line(t, 1, false) && !g.armed);
        t = 1000 + 90000; CHECK(g.allow_line(t, 1, false) && g.armed && g.run_lines == 1 && g.armed_ms == t);
        uint64_t flight = 1; for (unsigned w = 1; w < 100; ++w) for (unsigned k = 0; k < 40; ++k) if (g.allow_line(t + w * 1000, 1000u + k % 20u, false)) ++flight;
        CHECK(flight + 0 == Limiter::kRunLines - 0 && g.run_lines == Limiter::kRunLines);   // exactly the cap after arming, none lost to the ground
        // trigger file: created after the baseline poll arms at once; the baseline itself (file already present from an earlier run) never does
        Limiter f; CHECK(f.poll_due(5000) && !f.poll_due(5500) && f.poll_due(6000));
        f.observe_stamp(0); CHECK(!f.trigger);                          // baseline: absent
        f.observe_stamp(0); CHECK(!f.trigger);
        CHECK(!f.allow_line(7000, 1, false));
        f.observe_stamp(0x1234567u); CHECK(f.trigger);                  // created in flight
        CHECK(f.allow_line(8000, 1, false) && f.armed && f.run_lines == 1);
        CHECK(!f.poll_due(20000));                                      // no more polling once armed
        Limiter e; e.observe_stamp(0x99u); CHECK(!e.trigger);           // stale file at the first poll is the baseline
        e.observe_stamp(0x99u); CHECK(!e.trigger);                      // unchanged: no arm
        e.observe_stamp(0xabu); CHECK(e.trigger);                       // touched: arm
        Limiter z; z.observe_stamp(5); z.observe_stamp(0); CHECK(!z.trigger);   // deleted after the baseline: no arm
        // reads are gated too, and arming opens a fresh window
        Limiter rr; rr.trigger = true; CHECK(rr.allow_read(100) && rr.armed && rr.reads == 1);
    }

    // FlareTracker (batch M): per-signature dedupe, candidate ranking, intermittence, summary
    {
        const FlareFacts inst = facts(true, kFuncAlways, true, 6);          // HUD instrument class: depth-always alpha-over
        const FlareFacts addf = facts(true, kFuncAlways, true, kBlendOne);  // sun-like: additive
        FlareFacts inst2 = inst; inst2.ps8 = 0x11112222u;
        CHECK(bbox_bucket(Bbox()) == 31 && bbox_bucket(box(2000, 2000, 2100, 2100)) == bbox_bucket(box(2000 - 1728, 2000 - 1824, 2100 - 1728, 2100 - 1824)));
        CHECK(bbox_large(box(0, 0, 200, 100)) && bbox_large(box(0, 0, 330, 10)) && !bbox_large(box(0, 0, 60, 60)) && !bbox_large(Bbox()));
        // 5000 identical instrument draws over 60 s: one line (first sighting) + at most K-1 repeats, never the whole cap
        FlareTracker* t = new FlareTracker();
        const Bbox gauge = box(560, 20, 620, 60);
        unsigned lines = 0; uint64_t now = 100000;
        for (unsigned i = 0; i < 5000; ++i) { now = 100000 + i * 12; if (t->observe(now, inst, gauge, "admitted", "hud", "instrument").log) ++lines; }
        CHECK(lines >= 1 && lines <= FlareTracker::kPerSigTotal && t->nsig == 1 && t->sig[0].count == 5000 && t->sig[0].n_hud == 5000 && !t->sig[0].candidate);
        CHECK(lines == 2);                                                 // 60 s at a 30 s gap: first sighting + one repeat
        // first sighting of a different signature is always logged even when many instrument lines came before
        FlareTracker::Obs o = t->observe(now + 1, inst2, gauge, "admitted", "hud", "instrument");
        CHECK(o.log && o.first && !o.candidate);
        // a rare additive draw near the centre (the sun flare) is a candidate, logged at first sight, scored high, tagged A and C
        o = t->observe(now + 2, addf, box(300, 200, 340, 240), "admitted", "hud", "marker-box");
        CHECK(o.log && o.first && o.candidate && o.score >= 6 && std::strchr(o.reasons, 'A') && std::strchr(o.reasons, 'C') && o.sig->n_hud == 1);
        // candidate repeats: gap kCandGapMs and at most kPerSigCandidate lines
        unsigned clines = 1; uint64_t ct = now + 2;
        for (unsigned i = 0; i < 400; ++i) { ct += 1000; if (t->observe(ct, addf, box(300, 200, 340, 240), "admitted", "hud", "marker-box").log) ++clines; }
        CHECK(clines == FlareTracker::kPerSigCandidate);
        // admitted-image routing is counted per image and the last verdict is kept
        t->observe(ct + 1, addf, box(300, 200, 340, 240), "region-skip", "none", "marker-box");
        t->observe(ct + 2, addf, box(300, 200, 340, 240), "admitted", "markers", "marker-box");
        const FlareTracker::Sig* sa = nullptr; for (unsigned i = 0; i < FlareTracker::kSigCap; ++i) if (t->sig[i].used && t->sig[i].additive) sa = &t->sig[i];
        CHECK(sa && sa->n_none == 1 && sa->n_markers == 1 && sa->n_hud > 100 && std::strcmp(sa->verdict, "admitted") == 0);
        // summary: due after the interval, ranks the candidate first, lists counts/routing, bounded
        CHECK(t->summary_due(ct + 3) && !FlareTracker().summary_due(5));
        unsigned top[FlareTracker::kSummaryTop]; const unsigned nt = t->top(top);
        CHECK(nt == 3 && t->sig[top[0]].additive && t->sig[top[0]].candidate);
        char sb[kLineMax + 8]; std::memset(sb, 'X', sizeof sb);
        const int ns = format_flare_summary(sb, kLineMax, *t, ct + 3, 0);
        CHECK(ns > 0 && ns < int(kLineMax) && std::strncmp(sb, "fx-flare-summary;", 17) == 0 && std::strstr(sb, ";sigs=3;cand=") && sb[kLineMax] == 'X');
        const int ng = format_flare_sig(sb, kLineMax, t->sig[top[0]], 1, ct + 3, t->arm_ms);
        CHECK(ng > 0 && ng < int(kLineMax) && std::strncmp(sb, "fx-flare-sig;t=", 15) == 0 && std::strstr(sb, ";r=1;") && std::strstr(sb, "tag=flare-candidate;") && std::strstr(sb, "add=1;") &&
              std::strstr(sb, ";adm=hud:") && std::strstr(sb, ",mk:1,no:1;") && std::strstr(sb, "ps=d34ee153;"));
        t->summary_taken(ct + 3); CHECK(!t->summary_due(ct + 4) && !t->summary_due(ct + 3 + FlareTracker::kSummaryMs));   // no new draws: no summary
        t->observe(ct + 3 + FlareTracker::kSummaryMs, addf, box(300, 200, 340, 240), "admitted", "hud", "x"); CHECK(t->summary_due(ct + 3 + FlareTracker::kSummaryMs));
        delete t;
        // intermittence: a large alpha-over sprite (not centre, not additive) seen in 2 s bursts with 4 s gaps becomes a candidate once it reappears
        FlareTracker* u = new FlareTracker(); const Bbox side = box(10, 300, 60, 340);   // small, off-centre: not a candidate alone
        o = u->observe(1000, inst, side, "admitted", "hud", "marker-box"); CHECK(o.log && !o.candidate);
        for (uint64_t ms = 1100; ms < 3000; ms += 100) o = u->observe(ms, inst, side, "admitted", "hud", "marker-box");
        CHECK(!o.candidate && u->sig[0].runs == 1);
        o = u->observe(7000, inst, side, "admitted", "hud", "marker-box");   // reappears after a gap >= 2 epochs
        CHECK(o.candidate && o.log && std::strchr(o.reasons, 'I') && u->sig[0].runs == 2 && o.score >= 3);
        o = u->observe(7100, inst, side, "admitted", "hud", "marker-box"); CHECK(o.candidate && !o.log);   // same instant burst: no extra line
        // gone flag: unseen for >= 3 s in the sig line
        char gb[kLineMax]; format_flare_sig(gb, sizeof gb, u->sig[0], 1, 7100 + 4000, u->arm_ms); CHECK(std::strstr(gb, ";gone=1;"));
        format_flare_sig(gb, sizeof gb, u->sig[0], 1, 7200, u->arm_ms); CHECK(std::strstr(gb, ";gone=0;"));
        // t= is ms since arm on both summary and sig lines
        CHECK(std::strstr(gb, "fx-flare-sig;t=6200;r=1;") == gb);   // arm_ms 1000, now 7200
        delete u;
        // batch N tightening: HUD chrome (instrument / marker-label / hud-text) is never a candidate even when additive + centre + large + intermittent
        {
            FlareTracker* c = new FlareTracker(); const Bbox ctr = box(300, 200, 340, 240), big = box(0, 0, 400, 300);
            const char* chrome[3] = { "instrument", "marker-label", "hud-text" };
            for (const char* k : chrome) {
                const Bbox* bbs[2] = { &ctr, &big };
                for (const Bbox* bb : bbs) {
                    FlareTracker::Obs q = c->observe(1000, addf, *bb, "admitted", "hud", k); CHECK(!q.candidate && !q.sig->candidate);
                    q = c->observe(9000, addf, *bb, "admitted", "hud", k); CHECK(!q.candidate);   // reappearance (I) does not rescue it
                }
            }
            CHECK(c->candidates() == 0);
            // same draw classed marker-box (or unknown) and near the centre IS a candidate; additive alone, small and off-centre, is not
            FlareTracker::Obs q = c->observe(9100, addf, ctr, "admitted", "hud", "marker-box"); CHECK(q.candidate && std::strchr(q.reasons, 'C'));
            q = c->observe(9100, addf, box(10, 300, 60, 340), "admitted", "hud", "marker-box"); CHECK(!q.candidate && std::strchr(q.reasons, 'A'));
            q = c->observe(9100, inst, big, "admitted", "hud", "marker-box"); CHECK(q.candidate && std::strchr(q.reasons, 'L'));
            CHECK(hud_chrome_class("instrument") && hud_chrome_class("marker-label") && hud_chrome_class("hud-text") && !hud_chrome_class("marker-box") && !hud_chrome_class(nullptr));
            char tb[kLineMax];
            const int n1 = format_flare_summary(tb, kLineMax, *c, c->arm_ms + 12345, 0); CHECK(n1 > 0 && std::strstr(tb, "fx-flare-summary;t=12345;draws="));
            const int n2 = format_flare_sig(tb, kLineMax, c->sig[0], 1, c->arm_ms + 4321, c->arm_ms); CHECK(n2 > 0 && n2 < int(kLineMax) && std::strstr(tb, "fx-flare-sig;t=4321;r=1;"));
            delete c;
        }
        // W8-3 F1: the caller-equivalent class of a central depth-ALWAYS draw that is not a box/label/known text is "unclassified": it IS a candidate;
        // positively identified chrome stays excluded
        {
            CHECK(std::strcmp(flare_diag_class(false, false, false), "unclassified") == 0 && !hud_chrome_class(flare_diag_class(false, false, false)));
            CHECK(std::strcmp(flare_diag_class(true, false, false), "marker-box") == 0 && std::strcmp(flare_diag_class(false, true, false), "marker-label") == 0 &&
                  std::strcmp(flare_diag_class(false, false, true), "hud-text") == 0 && hud_chrome_class(flare_diag_class(false, true, false)) && hud_chrome_class(flare_diag_class(false, false, true)));
            FlareTracker* c = new FlareTracker(); const Bbox ctr = box(300, 200, 340, 240);
            FlareTracker::Obs q = c->observe(1000, addf, ctr, "admitted", "hud", flare_diag_class(false, false, false));
            CHECK(q.candidate && q.log && std::strchr(q.reasons, 'C') && c->candidates() == 1);
            q = c->observe(1100, inst, ctr, "admitted", "hud", flare_diag_class(false, false, true)); CHECK(!q.candidate);
            delete c;
        }
        // W8-3 F2: an intermittent non-candidate signature (small, off-centre, depth-always) seen at 100000/102000/104000 ms logs once, not on every return
        {
            FlareTracker* c = new FlareTracker(); const Bbox side = box(10, 300, 60, 340); unsigned ln = 0;
            { const uint64_t seq[3] = {100000ull, 102000ull, 104000ull}; for (uint64_t ms : seq) if (c->observe(ms, inst, side, "admitted", "hud", "instrument").log) ++ln; }
            CHECK(ln == 1);
            CHECK(c->observe(130000, inst, side, "admitted", "hud", "instrument").log);   // the documented 30 s spacing releases the next line
            // candidate: epochs differ by >= 2 (new runs) but the 5 s candidate spacing still applies
            FlareTracker* d = new FlareTracker(); unsigned dl = 0;
            { const uint64_t seq[4] = {1000ull, 3000ull, 5000ull, 7000ull}; for (uint64_t ms : seq) if (d->observe(ms, inst, side, "admitted", "hud", "marker-box").log) ++dl; }
            CHECK(dl == 2);   // 1000 (first) and 7000 (>= 5000 after... 1000+5000 = 6000 <= 7000); 3000/5000 suppressed
            delete c; delete d;
        }
        // signature table: bounded; fill with distinct non-candidate sigs, evict LRU non-candidates, candidates survive, lines stay under the run cap
        FlareTracker* w = new FlareTracker(); uint64_t wl = 0, wt = 1000;
        o = w->observe(wt, addf, box(300, 200, 340, 240), "admitted", "hud", "m"); CHECK(o.candidate);
        for (unsigned i = 0; i < 400; ++i) { FlareFacts g = inst; g.ps8 = 0x1000u + i; wt += 5; if (w->observe(wt, g, side, "admitted", "hud", "instrument").log) ++wl; }
        CHECK(w->nsig == FlareTracker::kSigCap && w->evictions == 400 - (FlareTracker::kSigCap - 1) && w->overflow == 0 && wl <= 3 * FlareTracker::kLinesPerWindow);
        bool kept = false; for (const auto& s : w->sig) if (s.used && s.additive) kept = true; CHECK(kept);
        // per-window line cap (24/s) and run caps (non-candidate 250, total 400) hold over a long noisy run
        FlareTracker* x = new FlareTracker(); uint64_t xl = 0;
        for (unsigned i = 0; i < 20000; ++i) { FlareFacts g = inst; g.ps8 = 0x2000u + i % 5000; const uint64_t tm = 1000 + i * 5ull; if (x->observe(tm, g, side, "admitted", "hud", "instrument").log) ++xl; }
        CHECK(xl <= FlareTracker::kRunLinesNonCandidate && x->run_lines == xl);
        // full table of candidates: a further new signature is counted as overflow, not stored
        FlareTracker* y = new FlareTracker();
        for (unsigned i = 0; i < FlareTracker::kSigCap + 3; ++i) { FlareFacts g = addf; g.ps8 = 0x3000u + i; y->observe(1000 + i, g, box(300, 200, 340, 240), "admitted", "hud", "m"); }
        CHECK(y->nsig == FlareTracker::kSigCap && y->overflow == 3);
        // note_unread, queue push_text
        y->note_unread(); CHECK(y->unread == 1);
        LineQueue* lq = new LineQueue(); lq->push_text("fx-flare-summary;t=1"); CHECK(lq->count == 1 && std::strcmp(lq->front(), "fx-flare-summary;t=1") == 0); delete lq;
        // draw line carries tag / signature id / score / reasons first
        FlareLine tl; tl.frame = 1; tl.ord = 2; tl.f = addf; tl.box = box(300, 200, 340, 240); tl.verdict = "admitted"; tl.image = "hud"; tl.hud_class = "marker-label";
        tl.tag = "flare-candidate"; tl.score = 8; tl.reasons = "AC"; tl.sg = 0xabcdef01u; tl.n = 17; char tb[kLineMax]; format_flare(tb, sizeof tb, tl);
        CHECK(std::strstr(tb, ";tag=flare-candidate;sg=abcdef01;sc=8;rs=AC;n=17;ps=d34ee153;"));
        delete w; delete x; delete y;
    }

    // formatters: bounded with extreme values (the 426-byte queue line, the 480-byte log bound), no overflow
    FlareLine fl; fl.frame = ~0ull; fl.ord = ~0ull; fl.f = facts(true, 0xffffffffu, true, 255);
    fl.f.ps8 = fl.f.vs8 = 0xffffffffu; std::memset(fl.f.blend, 255, 6); fl.f.count = fl.f.instances = 0xffffffffu; fl.f.indexed = true;
    fl.box.add(-3.4e38f, -3.4e38f); fl.box.add(3.4e38f, 3.4e38f); fl.nvtx = 48; fl.sup = ~0ull;
    fl.verdict = "a-very-long-refusal-reason-that-must-be-truncated-by-the-formatter"; fl.image = "markers-and-more"; fl.hud_class = "marker-label-xxxxxxxxxx";
    fl.read_why = "vertex-outside-view-partial-and-more"; fl.depth_tested = true;
    char buf[kLineMax + 64]; std::memset(buf, 'X', sizeof buf);
    const int n1 = format_flare(buf, kLineMax, fl);
    CHECK(n1 > 0 && n1 < int(kLineMax) && std::strlen(buf) == size_t(n1) && std::strncmp(buf, "fx-flare-draw;", 14) == 0);
    CHECK(buf[kLineMax] == 'X');                             // never wrote past the cap
    char tiny[16]; CHECK(format_flare(tiny, sizeof tiny, fl) == int(sizeof tiny - 1) && std::strlen(tiny) == sizeof tiny - 1);
    CHECK(format_flare(nullptr, 10, fl) == 0 && format_flare(buf, 1, fl) == 0);
    FlareLine ok; ok.frame = 12; ok.ord = 345; ok.f = add; ok.box = box(300, 200, 340, 240); ok.nvtx = 4; ok.verdict = "admitted"; ok.image = "hud"; ok.hud_class = "instrument";
    format_flare(buf, kLineMax, ok);
    CHECK(std::strstr(buf, "ps=d34ee153;vs=534f29ca;") && std::strstr(buf, "add=1;") && std::strstr(buf, "ctr=1;") && std::strstr(buf, "img=hud;") && std::strstr(buf, "hud=admitted;"));
    CHECK(std::strstr(buf, "bbox=300,200,340,240;") && std::strstr(buf, "why=ok;"));

    AirLine a; a.frame = ~0ull; a.ord = ~0ull; a.cpt = false; a.ps8 = a.vs8 = 0xffffffffu; a.count = a.instances = 0xffffffffu; a.indexed = true;
    a.rc_available = true; a.rc_mask = 0xf; for (auto& w : a.rc) w = 0xffffffffu; a.psoKnown = true; a.blend = true; a.depth_enable = true; a.depth_write = true;
    for (auto& w : a.bl) w = 0xffffffffu; a.depth_func = 0xffffffffu; a.write_mask = 0xffffffffu; a.rt = a.rtv = ~0ull; a.rt_w = ~0ull; a.rt_h = a.rt_fmt = 0xffffffffu;
    a.vp_w = a.vp_h = 3.4e38f; std::memset(a.roots, 'r', sizeof a.roots - 1); a.sup = ~0ull;
    std::memset(buf, 'X', sizeof buf);
    const int n2 = format_air(buf, kLineMax, a);
    CHECK(n2 > 0 && n2 < int(kLineMax) && std::strlen(buf) == size_t(n2) && std::strncmp(buf, "fx-air-draw;", 12) == 0 && buf[kLineMax] == 'X');
    AirLine refl; refl.rc_available = true; refl.rc_mask = 1; refl.rc[0] = 7; refl.frame = 3; refl.ord = 9; std::snprintf(refl.roots, sizeof refl.roots, "3:T0=1a2b");
    CHECK(air_reflect(refl));
    format_air(buf, kLineMax, refl);
    CHECK(std::strstr(buf, "flags=7(0x7);refl=1;") && std::strstr(buf, "k=acm;") && std::strstr(buf, "roots=3:T0=1a2b;"));
    AirLine plain = refl; plain.rc[0] = 258; CHECK(!air_reflect(plain));
    AirLine unkn; CHECK(!air_reflect(unkn)); format_air(buf, kLineMax, unkn); CHECK(std::strstr(buf, "flags=unknown;refl=0;") && std::strstr(buf, "roots=none;"));
    AirLine nomask = refl; nomask.rc_mask = 0; CHECK(!air_reflect(nomask));

    // queue: FIFO, full = dropped
    LineQueue* q = new LineQueue();
    for (unsigned i = 0; i < LineQueue::kCap + 5; ++i) { ok.frame = i; if (i % 2) q->push_air(refl); else q->push_flare(ok); }
    CHECK(q->count == LineQueue::kCap && q->dropped == 5);
    CHECK(std::strncmp(q->front(), "fx-flare-draw;f=0;", 18) == 0);
    q->pop(); CHECK(std::strncmp(q->front(), "fx-air-draw;", 12) == 0 && q->count == LineQueue::kCap - 1);
    while (q->front()) q->pop();
    CHECK(q->count == 0 && q->front() == nullptr);
    delete q;

    CHECK(kArmFlare != 0 && kArmAir != 0 && kArmFlare != kArmAir);
    std::printf("fx_diag_flare_air_offline: %u checks passed\n", g_checks);
    return 0;
}
