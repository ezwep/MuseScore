#include "scorefollower.h"
#include <cstdio>
using namespace Ms;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { ++fails; printf("FAIL: %s (line %d, idx %d)\n", msg, __LINE__, f.currentIndex()); } else printf("ok:   %s\n", msg); } while (0)
static ScoreFollower::Event ev(int t, std::initializer_list<int> p) { ScoreFollower::Event e; e.tick = Fraction::fromTicks(t); for (int x : p) e.pitches.insert(x); return e; }

// simulated organist: releases the previous chord, then strikes the next one
// (notes 5 ms apart); optional echo repeats every note-on 3 ms later
static qint64 now = 1000;
static std::vector<int> down;
static void chord(ScoreFollower& f, std::initializer_list<int> ps, bool echo = false)
      {
      now += 500;
      for (int p : down) f.noteOff(p);
      down.clear();
      qint64 t = now;
      for (int p : ps) {
            f.noteOn(p, t);
            if (echo) f.noteOn(p, t + 3);
            down.push_back(p);
            t += 5;
            }
      }
static void chordSpaced(ScoreFollower& f, std::initializer_list<int> ps, int spacing)
      {
      now += 500;
      for (int p : down) f.noteOff(p);
      down.clear();
      qint64 t = now;
      for (int p : ps) { f.noteOn(p, t); down.push_back(p); t += spacing; }
      now = t;
      }
static void reset(ScoreFollower& f, const std::vector<ScoreFollower::Event>& e)
      {
      for (int p : down) f.noteOff(p);
      down.clear();
      now += 1000;
      f.setEvents(e);
      }

int main() {
      ScoreFollower f;
      // 4-voice hymn
      std::vector<ScoreFollower::Event> hymn = {
            ev(0,{67,64,60,48}), ev(480,{67,62,59,55}), ev(960,{69,65,60,53}), ev(1440,{67,64,60,48}),
            ev(1920,{65,62,59,55}), ev(2400,{64,60,55,48}) };
      reset(f, hymn);
      chord(f, {67,64,60,48}); CHECK(f.currentIndex()==0, "first chord");
      chord(f, {62,67,55,59}); CHECK(f.currentIndex()==1, "second chord, any order");
      chord(f, {69,65,60,53}); CHECK(f.currentIndex()==2, "third chord");
      chord(f, {67,64,60,36}); CHECK(f.currentIndex()==3, "pedal octave lower");
      chord(f, {61});          CHECK(f.currentIndex()==3, "wrong note: stay");
      chord(f, {65,62,59,55}); CHECK(f.currentIndex()==4, "continue after wrong note");

      // a fumble that lands on notes of the next chord moves ahead; playing the
      // right chord again must bring the position back
      reset(f, hymn);
      chord(f, {67,64,60,48}); chord(f, {67,62,59,55}); CHECK(f.currentIndex()==1, "fumble: at chord 2");
      chord(f, {64,48});       CHECK(f.currentIndex()==1, "fumble: two notes two chords ahead do not move");
      chord(f, {64,60,48});    CHECK(f.currentIndex()==0, "fumble: three notes of chord 1/4 (identical) move to the nearest");
      chord(f, {69,65,60,53}); CHECK(f.currentIndex()==2, "fumble: playing chord 3 correctly brings it back");
      chord(f, {67,64,60,48}); CHECK(f.currentIndex()==3, "fumble: and on to chord 4");
      chord(f, {65,62,59,55}); CHECK(f.currentIndex()==4, "fumble: chord 5");

      // single wrong notes never move, even when they occur ahead
      reset(f, hymn);
      chord(f, {67,64,60,48});
      chord(f, {65});  CHECK(f.currentIndex()==0, "single wrong note 65 (in chord 3 and 5): stay");
      chord(f, {69});  CHECK(f.currentIndex()==0, "another single wrong note 69: stay");
      chord(f, {67,62,59,55}); CHECK(f.currentIndex()==1, "then the right chord 2");

      // codex review 4 counterexamples
      reset(f, {ev(0,{60}), ev(480,{62,65,69}), ev(960,{64}), ev(1440,{64})});
      chord(f, {60}); chord(f, {64}); CHECK(f.currentIndex()==0, "one wrong note cannot confirm two far single events");
      reset(f, {ev(0,{60,64,67}), ev(480,{64,67}), ev(960,{72})});
      chord(f, {60,64}); chord(f, {64,67}); CHECK(f.currentIndex()==1, "incomplete chord: next chord with a shared tone still reached");
      reset(f, {ev(0,{60,64,48}), ev(480,{36}), ev(960,{72})});
      chord(f, {60,64}); chord(f, {36}); CHECK(f.currentIndex()==0, "pedal octave completes current chord, not the next {36}");
      reset(f, {ev(0,{60,64,67}), ev(480,{60,64,67}), ev(960,{60,64,67}), ev(1440,{62})});
      chord(f, {60,64,67}); chord(f, {60,64,67}); chord(f, {60,64,67}); chord(f, {62});
      CHECK(f.currentIndex()==3, "repeated identical chords followed one by one");
      reset(f, {ev(0,{60,64,67}), ev(480,{60,64,67}), ev(960,{62})});
      chordSpaced(f, {60,64,67}, 150); CHECK(f.currentIndex()==0, "slowly spread chord on repeated chords does not run ahead");

      // codex review 6: re-striking the current chord, then one wrong note, must not jump
      reset(f, {ev(0,{60,64,67}), ev(480,{70,72,74}), ev(960,{60,64,67,62}), ev(1440,{80})});
      chord(f, {60,64,67}); chord(f, {60,64,67}); chord(f, {62});
      CHECK(f.currentIndex()==0, "re-struck chord + one wrong note does not jump to a similar chord");

      // Psalm 25 opening (shared tones between chords) with MIDI echo
      std::vector<ScoreFollower::Event> ps25 = {
            ev(0,{65,60,57,53}), ev(960,{67,64,57,45}), ev(1440,{65,61,53,49}), ev(1920,{60,64,57,48}) };
      reset(f, ps25);
      chord(f, {65,60,57,53}, true); CHECK(f.currentIndex()==0, "Psalm 25: first chord with echo stays on chord 1");
      chord(f, {67,64,57,45}, true); CHECK(f.currentIndex()==1, "Psalm 25: second chord with echo");
      chord(f, {65,61,53,49}, true); CHECK(f.currentIndex()==2, "Psalm 25: third chord with echo");

      // no position yet, pedal (octave lower) arrives first and that pitch is exact in chord 3
      reset(f, {ev(0,{69,65,60,53}), ev(960,{67,64,57,45}), ev(1440,{65,61,53,41}), ev(1920,{60})});
      chord(f, {41,69,65,60}); CHECK(f.currentIndex()==0, "start: early pedal F2 does not jump to chord 3");
      chord(f, {67,64,57,45}); CHECK(f.currentIndex()==1, "start: then chord 2");
      chord(f, {41,65,61,53}); CHECK(f.currentIndex()==2, "start: then chord 3");

      // shared tone struck together with the next chord must not skip ahead
      std::vector<ScoreFollower::Event> share = { ev(0,{60,64}), ev(480,{62,65}), ev(960,{64,67}), ev(1440,{65,69}) };
      reset(f, share);
      chord(f, {60,64}); chord(f, {65,62}); CHECK(f.currentIndex()==1, "chord 2");
      chord(f, {64,67}); CHECK(f.currentIndex()==2, "64 (also in nothing ahead) -> chord 3, not further");

      // held notes (only some voices re-struck)
      std::vector<ScoreFollower::Event> held = { ev(0,{60,64,67}), ev(480,{65}), ev(960,{64}), ev(1440,{62,67}) };
      reset(f, held);
      chord(f,{60,64,67}); chord(f,{65}); CHECK(f.currentIndex()==1, "only moving voice");
      chord(f,{64}); CHECK(f.currentIndex()==2, "repeated pitch 64 moves forward");

      // codex example {48},{60}
      reset(f, {ev(0,{48}), ev(480,{60})});
      chord(f,{48}); chord(f,{60}); CHECK(f.currentIndex()==1, "next event exact");

      // large jump requires confirmation
      std::vector<ScoreFollower::Event> lin;
      for (int i = 0; i < 12; ++i) lin.push_back(ev(i*480, {60 + i}));
      reset(f, lin);
      chord(f,{60}); CHECK(f.currentIndex()==0, "start");
      chord(f,{63}); CHECK(f.currentIndex()==0, "single far note alone does not move");
      chord(f,{51}); CHECK(f.currentIndex()==0, "unrelated note does not move");
      chord(f,{62}); chord(f,{63}); CHECK(f.currentIndex()==3, "skipped note: sequence 62,63 moves to the 63");
      chord(f,{64}); CHECK(f.currentIndex()==4, "continue");
      chord(f,{76}); CHECK(f.currentIndex()==4, "4' coupler octave ahead does not move");

      // repeats
      std::vector<ScoreFollower::Event> rep = { ev(0,{60}), ev(480,{62}), ev(960,{64}), ev(0,{60}), ev(480,{62}), ev(960,{64}), ev(1440,{65}) };
      reset(f, rep);
      f.setPosition(Fraction::fromTicks(480)); CHECK(f.currentIndex()==1, "setPosition first pass");
      chord(f,{62}); chord(f,{64}); chord(f,{60}); CHECK(f.currentIndex()==3, "follows into repeat");
      f.setPosition(Fraction::fromTicks(480)); CHECK(f.currentIndex()==4, "setPosition picks nearest pass");
      reset(f, rep); f.setPosition(Fraction::fromTicks(960));
      CHECK(f.advanceTo(Fraction::fromTicks(1440)) && f.currentIndex()==3, "advanceTo stops at repeat jump");
      CHECK(f.advanceTo(Fraction::fromTicks(1440)) && f.currentIndex()==6, "advanceTo then reaches target");
      CHECK(!f.advanceTo(Fraction::fromTicks(99999)), "advanceTo never moves backward");
      CHECK(f.restart() && f.currentIndex()==0, "restart goes to first chord");
      reset(f, {ev(0,{60,64}), ev(480,{72,76}), ev(960,{60})});
      f.restart(); chord(f, {60,64,72});
      CHECK(f.currentIndex()==0, "after restart: 4' coupler in first chord does not jump");
      chord(f, {72,76}); CHECK(f.currentIndex()==1, "then next chord moves on");

      // recovery
      std::vector<ScoreFollower::Event> rec;
      for (int i = 0; i < 40; ++i) rec.push_back(ev(i*480, {40 + i}));
      reset(f, rec); chord(f,{40});
      for (int p : {70,71,72,73,74,75,76,77}) chord(f, {p});
      CHECK(f.currentIndex()==37, "recovered after misses");

      // stress: generated 4-voice piece (stepwise voices), played with random
      // wrong notes and fumbled chords
      {
            unsigned seed = 12345;
            auto rnd = [&](int n) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % unsigned(n)); };
            std::vector<ScoreFollower::Event> piece;
            int v[4] = { 72, 67, 60, 48 };
            for (int i = 0; i < 60; ++i) {
                  ScoreFollower::Event e; e.tick = Fraction::fromTicks(i * 480);
                  for (int k = 0; k < 4; ++k) { v[k] += rnd(5) - 2; e.pitches.insert(v[k]); }
                  piece.push_back(e);
                  }
            int maxLead = 0, worstLag = 0;
            for (int round = 0; round < 5; ++round) {
                  reset(f, piece);
                  for (int i = 0; i < 60; ++i) {
                        if (rnd(100) < 15) chord(f, { 48 + rnd(30) });                      // wrong note
                        if (rnd(100) < 5)  { int a = 48 + rnd(30); chord(f, { a, a + 4 }); } // fumble
                        std::vector<int> ps(piece[i].pitches.begin(), piece[i].pitches.end());
                        now += 500; for (int p : down) f.noteOff(p); down.clear();
                        qint64 t = now; for (int p : ps) { f.noteOn(p, t); down.push_back(p); t += 5; }
                        maxLead  = qMax(maxLead, f.currentIndex() - i);
                        worstLag = qMax(worstLag, i - f.currentIndex());
                        }
                  CHECK(f.currentIndex() == 59, "stress: ends on the last chord");
                  }
            printf("      stress: max lead %d, worst lag %d", maxLead, worstLag);
            puts("");
            CHECK(maxLead <= 1, "stress: never more than one chord ahead");
            CHECK(worstLag <= 2, "stress: never more than two chords behind");
      }

      // stress 2: held voices: only voices that move are re-struck (ties), the
      // others stay down; random wrong notes in between
      {
            unsigned seed = 777;
            auto rnd = [&](int n) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % unsigned(n)); };
            std::vector<ScoreFollower::Event> piece;
            std::vector<std::vector<int>> moves;    // per event: (voice, newPitch) pairs flattened
            int v[4] = { 72, 67, 60, 48 };
            for (int i = 0; i < 60; ++i) {
                  ScoreFollower::Event e; e.tick = Fraction::fromTicks(i * 480);
                  std::vector<int> mv;
                  for (int k = 0; k < 4; ++k) {
                        if (i == 0 || rnd(100) < 55) {
                              int np = v[k] + (i == 0 ? 0 : (rnd(2) ? 1 : -1) * (1 + rnd(2)));
                              v[k] = np; e.pitches.insert(np); mv.push_back(k); mv.push_back(np);
                              }
                        }
                  if (e.pitches.isEmpty()) { v[0] += 2; e.pitches.insert(v[0]); mv.push_back(0); mv.push_back(v[0]); }
                  piece.push_back(e); moves.push_back(mv);
                  }
            int maxLead = 0, worstLag = 0;
            for (int round = 0; round < 5; ++round) {
                  for (int p : down) f.noteOff(p);
                  down.clear();
                  now += 1000;
                  f.setEvents(piece);
                  int heldKey[4] = { -1, -1, -1, -1 };
                  for (int i = 0; i < 60; ++i) {
                        now += 500;
                        if (rnd(100) < 15) { int w = 48 + rnd(30); f.noteOn(w, now); f.noteOff(w); now += 120; }
                        const std::vector<int>& mv = moves[i];
                        for (size_t q = 0; q < mv.size(); q += 2) { int k = mv[q]; if (heldKey[k] >= 0) f.noteOff(heldKey[k]); }
                        qint64 t = now;
                        for (size_t q = 0; q < mv.size(); q += 2) { int k = mv[q]; heldKey[k] = mv[q + 1]; f.noteOn(mv[q + 1], t); t += 5; }
                        maxLead  = qMax(maxLead, f.currentIndex() - i);
                        worstLag = qMax(worstLag, i - f.currentIndex());
                        }
                  for (int k = 0; k < 4; ++k) if (heldKey[k] >= 0) f.noteOff(heldKey[k]);
                  CHECK(f.currentIndex() == 59, "stress 2 (held voices): ends on the last chord");
                  }
            printf("      stress 2: max lead %d, worst lag %d", maxLead, worstLag);
            puts("");
            CHECK(maxLead <= 1, "stress 2: never more than one chord ahead");
            CHECK(worstLag <= 2, "stress 2: never more than two chords behind");
      }

      // stress 3: many mistakes: bursts of 3-6 wrong notes before 30% of the chords
      {
            unsigned seed = 4242;
            auto rnd = [&](int n) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % unsigned(n)); };
            std::vector<ScoreFollower::Event> piece;
            int v[4] = { 72, 67, 60, 48 };
            for (int i = 0; i < 60; ++i) {
                  ScoreFollower::Event e; e.tick = Fraction::fromTicks(i * 480);
                  for (int k = 0; k < 4; ++k) { v[k] += rnd(5) - 2; e.pitches.insert(v[k]); }
                  piece.push_back(e);
                  }
            int maxLead = 0, worstLag = 0, endOk = 0;
            for (int round = 0; round < 10; ++round) {
                  reset(f, piece);
                  for (int i = 0; i < 60; ++i) {
                        std::vector<int> log; int before = f.currentIndex();
                        if (rnd(100) < 30) {
                              int burst = 3 + rnd(4);
                              for (int b = 0; b < burst; ++b) { now += 120; int w = 45 + rnd(35); f.noteOn(w, now); f.noteOff(w); log.push_back(w); log.push_back(f.currentIndex()); }
                              }
                        if (rnd(100) < 10) { int a = 48 + rnd(30); chord(f, { a, a + 3, a + 7 }); log.push_back(-a); log.push_back(f.currentIndex()); }
                        std::vector<int> ps(piece[i].pitches.begin(), piece[i].pitches.end());
                        now += 500; for (int p : down) f.noteOff(p); down.clear();
                        qint64 t = now; for (int p : ps) { f.noteOn(p, t); down.push_back(p); t += 5; }
                        if (i - f.currentIndex() > 2 && worstLag <= 2) {
                              printf("DEBUG-LAG round %d chord %d: was %d now %d; played:", round, i, before, f.currentIndex());
                              for (size_t q = 0; q < log.size(); q += 2) printf(" %d->%d", log[q], log[q+1]);
                              printf(" | true chord:"); for (int p : ps) printf(" %d", p);
                              printf(" | events:"); for (int e = qMax(0, f.currentIndex() - 1); e <= qMin(59, i); ++e) { printf(" [%d:", e); for (int p : piece[e].pitches) printf(" %d", p); printf("]"); }
                              puts("");
                              }
                        if (f.currentIndex() - i > 1 && maxLead <= 1) {
                              printf("DEBUG round %d chord %d: was %d now %d; played:", round, i, before, f.currentIndex());
                              for (size_t q = 0; q < log.size(); q += 2) printf(" %d->%d", log[q], log[q+1]);
                              printf(" | true chord:"); for (int p : ps) printf(" %d", p);
                              printf(" | events:"); for (int e = qMax(0, i - 1); e <= qMin(59, f.currentIndex()); ++e) { printf(" [%d:", e); for (int p : piece[e].pitches) printf(" %d", p); printf("]"); }
                              puts("");
                              }
                        maxLead  = qMax(maxLead, f.currentIndex() - i);
                        worstLag = qMax(worstLag, i - f.currentIndex());
                        }
                  if (f.currentIndex() == 59) ++endOk;
                  }
            printf("      stress 3 (many mistakes): max lead %d, worst lag %d, ended right %d/10", maxLead, worstLag, endOk);
            puts("");
            CHECK(maxLead <= 1, "stress 3: never more than one chord ahead");
            CHECK(endOk == 10, "stress 3: always ends on the last chord");
      }

      printf(fails ? "\n%d FAILURES\n" : "\nALL OK\n", fails);
      return fails;
}
