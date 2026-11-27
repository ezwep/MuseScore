#include "scorefollower.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <algorithm>
#include <QFile>
#include <QTextStream>
#include <QStringList>
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

// load a chord list in replay-log format (EVENTS n / E idx tick p1,p2,...)
static std::vector<ScoreFollower::Event> loadEvents(const QString& path)
      {
      std::vector<ScoreFollower::Event> ev;
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return ev;
      QTextStream in(&file);
      while (!in.atEnd()) {
            const QStringList w = in.readLine().split(' ', Qt::SkipEmptyParts);
            if (w.size() >= 4 && w[0] == "E") {
                  ScoreFollower::Event e;
                  e.tick = Fraction::fromTicks(w[2].toInt());
                  for (const QString& p : w[3].split(',')) e.pitches.insert(p.toInt());
                  e.optional = w.size() > 4 && w[4] == "opt";
                  ev.push_back(e);
                  }
            }
      return ev;
      }

static unsigned orderSeed = 1;
static void strike(ScoreFollower& f, const QSet<int>& ps)
      {
      now += 500;
      for (int p : down) f.noteOff(p);
      down.clear();
      // like a real player: the notes of a chord arrive in varying order
      std::vector<int> v(ps.begin(), ps.end());
      std::sort(v.begin(), v.end());
      for (size_t i = v.size(); i > 1; --i) {
            orderSeed = orderSeed * 1103515245u + 12345u;
            std::swap(v[i - 1], v[(orderSeed >> 16) % i]);
            }
      qint64 t = now;
      for (int p : v) { f.noteOn(p, t); down.push_back(p); t += 3 + int((orderSeed >> 8) % 20); }
      }

// Replay a MuseScoreMeelezen.log: feed the logged notes with their logged times
// and show, per note, what the app decided then and what the current code decides.
// The replay runs on continuously (no resynchronising): after the first difference
// the two may drift apart, which is shown as such. Each START is a fresh session.
static int replay(const char* path)
      {
      QFile file(QString::fromLocal8Bit(path));
      if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { printf("cannot open %s\n", path); return 2; }
      std::unique_ptr<ScoreFollower> f(new ScoreFollower);
      std::vector<ScoreFollower::Event> events;
      int expect = -1, diffs = 0, ons = 0, lineNo = 0, firstDiff = -1;
      QTextStream in(&file);
      auto bad = [&](const QString& l) { printf("  (line %d skipped: %s)\n", lineNo, qPrintable(l)); };
      while (!in.atEnd()) {
            const QString line = in.readLine();
            ++lineNo;
            const QStringList w = line.split(' ', Qt::SkipEmptyParts);
            if (w.isEmpty())
                  continue;
            bool ok1 = true, ok2 = true;
            if (w[0] == "START") {
                  f.reset(new ScoreFollower);           // new session: no state carried over
                  events.clear();
                  expect = -1;
                  printf("%s\n", qPrintable(line));
                  }
            else if (w[0] == "EVENTS" && w.size() >= 2) {
                  events.clear();
                  expect = w[1].toInt(&ok1);
                  if (!ok1) { bad(line); expect = -1; continue; }
                  if (expect == 0) f->setEvents(events);
                  }
            else if (w[0] == "E" && w.size() >= 3) {
                  ScoreFollower::Event e;
                  e.tick = Fraction::fromTicks(w[2].toInt(&ok1));
                  if (w.size() > 3)
                        for (const QString& p : w[3].split(',')) { bool ok; int v = p.toInt(&ok); if (ok) e.pitches.insert(v); }
                  e.optional = w.size() > 4 && w[4] == "opt";
                  if (!ok1) { bad(line); continue; }
                  events.push_back(e);
                  if (int(events.size()) == expect) { f->setEvents(events); printf("  (%d chords loaded)\n", expect); }
                  }
            else if (w[0] == "OFF" && w.size() >= 3) {
                  const qint64 t = w[1].toLongLong(&ok1);
                  const int p = w[2].toInt(&ok2);
                  if (ok1 && ok2) f->noteOff(p, t); else bad(line);
                  }
            else if (w[0] == "POS" && w.size() >= 5 && w[3].startsWith("tick=")) {
                  const int tick = w[3].mid(5).toInt(&ok1);
                  if (!ok1) { bad(line); continue; }
                  if (w[4] == "restart") f->restart();
                  else if (w[4] == "advance") f->advanceTo(Fraction::fromTicks(tick));
                  else f->setPosition(Fraction::fromTicks(tick));
                  printf("%-60s | replay -> %d\n", qPrintable(line), f->currentIndex());
                  }
            else if (w[0] == "ON" && w.size() >= 4 && w[3].contains("->")) {
                  const qint64 t = w[1].toLongLong(&ok1);
                  const int p = w[2].toInt(&ok2);
                  const QStringList ba = w[3].split("->");
                  bool ok3 = false;
                  const int after = ba.value(1).toInt(&ok3);
                  if (!ok1 || !ok2 || !ok3) { bad(line); continue; }
                  ++ons;
                  f->noteOn(p, t);
                  const bool diff = f->currentIndex() != after;
                  if (diff) { ++diffs; if (firstDiff < 0) firstDiff = lineNo; }
                  printf("%-60s | replay -> %d %s%s\n", qPrintable(line), f->currentIndex(),
                         qPrintable(f->lastReason()), diff ? "   <== DIFFERENT" : "");
                  }
            else
                  bad(line);
            }
      printf("\n%d notes replayed, %d decisions differ from the log", ons, diffs);
      if (firstDiff > 0)
            printf(" (first at line %d)", firstDiff);
      puts("");
      return 0;
      }

// ---------------------------------------------------------------------------
// Evaluation against the ideal alignment: with all notes known afterwards, a
// full (off-line) dynamic programming alignment says where the player really was
// at every note. The live follower is compared with that, note by note.
// ---------------------------------------------------------------------------

struct PlayedNote { int pitch; qint64 time; };

static double offReward(const ScoreFollower::Event& e, int pitch, const QSet<int>& used, int* sp)
      {
      *sp = -1;
      if (e.pitches.contains(pitch) && !used.contains(pitch)) { *sp = pitch; return ScoreFollower::MATCH; }
      for (int p : { pitch + 12, pitch - 12 })
            if (e.pitches.contains(p) && !used.contains(p)) { *sp = p; return ScoreFollower::OCTAVE; }
      return ScoreFollower::EXTRA;
      }

static double offSkip(const ScoreFollower::Event& e)
      {
      if (e.optional) return 0.0;
      if (e.pitches.size() == 1) return ScoreFollower::SKIP_SINGLE;
      return qMin(ScoreFollower::SKIP_MAX, ScoreFollower::SKIP_PER_NOTE * e.pitches.size());
      }

// position (event index) per note of the best complete alignment starting at 'start'
static std::vector<int> idealPath(const std::vector<ScoreFollower::Event>& ev, const std::vector<PlayedNote>& notes, int start)
      {
      const int n = int(ev.size()), T = int(notes.size());
      const double NEG = -1e9;
      std::vector<std::vector<double>> V(T + 1, std::vector<double>(n, NEG));
      std::vector<std::vector<int>> from(T + 1, std::vector<int>(n, -1));
      std::vector<std::vector<QSet<int>>> used(T + 1, std::vector<QSet<int>>(n));
      V[0][start] = 0;
      for (int t = 0; t < T; ++t) {
            for (int i = start; i < n; ++i) {
                  double best = NEG; int bf = -1; QSet<int> bu;
                  if (V[t][i] > NEG / 2) {
                        int sp; double r = offReward(ev[i], notes[t].pitch, used[t][i], &sp);
                        best = V[t][i] + r; bf = i; bu = used[t][i]; if (sp >= 0) bu.insert(sp);
                        }
                  double skipped = 0;
                  for (int k = i - 1; k >= qMax(start, i - 1 - ScoreFollower::MAX_SKIP); --k) {
                        if (k < i - 1) skipped += offSkip(ev[k + 1]);
                        if (V[t][k] <= NEG / 2) continue;
                        int sp; double r = offReward(ev[i], notes[t].pitch, QSet<int>(), &sp);
                        if (sp < 0) continue;     // enter an event only with one of its notes
                        double val = V[t][k] + ScoreFollower::ENTER - skipped + r;
                        if (val > best) { best = val; bf = k; bu.clear(); if (sp >= 0) bu.insert(sp); }
                        }
                  V[t + 1][i] = best; from[t + 1][i] = bf; used[t + 1][i] = bu;
                  }
            }
      int bi = start;
      for (int i = start; i < n; ++i) if (V[T][i] > V[T][bi]) bi = i;
      std::vector<int> path(T);
      for (int t = T; t >= 1; --t) { path[t - 1] = bi; bi = from[t][bi]; }
      return path;
      }

static void evaluateBlock(const std::vector<ScoreFollower::Event>& events,
                          const std::vector<std::pair<int, std::vector<std::pair<bool, PlayedNote>>>>& segs, int blockNo)
      {
      int total = 0, same = 0, ahead = 0, behind = 0, maxLead = 0, maxLag = 0, backJumps = 0, lostNotes = 0, notesAll = 0;
      for (auto& seg : segs) {
            if (seg.first < 0 || seg.first >= int(events.size())) continue;
            std::vector<PlayedNote> notes;
            QSet<int> down;
            ScoreFollower f;
            f.setEvents(events);
            f.setPosition(events[seg.first].tick);
            std::vector<int> online;
            int prev = seg.first;
            for (auto& on : seg.second) {
                  if (!on.first) { down.remove(on.second.pitch); f.noteOff(on.second.pitch, on.second.time); continue; }
                  if (down.contains(on.second.pitch)) continue;
                  down.insert(on.second.pitch);
                  f.noteOn(on.second.pitch, on.second.time);
                  notes.push_back(on.second);
                  online.push_back(f.currentIndex());
                  if (f.currentIndex() < prev) ++backJumps;
                  prev = f.currentIndex();
                  ++notesAll; if (f.lost()) ++lostNotes;
                  }
            if (notes.empty()) continue;
            const std::vector<int> ideal = idealPath(events, notes, seg.first);
            for (size_t t = 0; t < notes.size(); ++t) {
                  if (t + 1 < notes.size() && notes[t + 1].time - notes[t].time <= ScoreFollower::STRIKE_MS)
                        continue;
                  const int d = online[t] - ideal[t];
                  ++total;
                  if (d == 0) ++same; else if (d > 0) ++ahead; else ++behind;
                  maxLead = qMax(maxLead, d); maxLag = qMax(maxLag, -d);
                  if (qEnvironmentVariableIsSet("SF_STATE") && qAbs(d) >= 2)
                        printf("  block %d t=%lld note %d: follower %d, ideal %d (%+d)\n", blockNo, notes[t].time, notes[t].pitch, online[t], ideal[t], d);
                  }
            }
      if (total)
            printf("piece %d (%d chords): %d strikes, %.0f%% on the right chord, %d ahead, %d behind; at most %d ahead, %d behind; %d steps back; lost on %d of %d notes\n",
                   blockNo, int(events.size()), total, 100.0 * same / total, ahead, behind, maxLead, maxLag, backJumps, lostNotes, notesAll);
      }

static int evaluate(const char* path)
      {
      QFile file(QString::fromLocal8Bit(path));
      if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { printf("cannot open %s\n", path); return 2; }
      std::vector<ScoreFollower::Event> events;
      std::vector<std::pair<int, std::vector<std::pair<bool, PlayedNote>>>> segs;
      int block = 0;
      QTextStream in(&file);
      while (!in.atEnd()) {
            const QStringList w = in.readLine().split(' ', Qt::SkipEmptyParts);
            if (w.isEmpty()) continue;
            if (w[0] == "EVENTS" && w.size() >= 2) {
                  if (!events.empty()) evaluateBlock(events, segs, block);
                  ++block; events.clear(); segs.clear();
                  }
            else if (w[0] == "E" && w.size() >= 4) {
                  ScoreFollower::Event e; e.tick = Fraction::fromTicks(w[2].toInt());
                  for (const QString& p : w[3].split(',')) e.pitches.insert(p.toInt());
                  e.optional = w.size() > 4 && w[4] == "opt";
                  events.push_back(e);
                  }
            else if (w[0] == "POS" && w.size() >= 3 && w[2].contains("->")) segs.push_back({ w[2].split("->")[1].toInt(), {} });
            else if ((w[0] == "ON" || w[0] == "OFF") && w.size() >= 3) {
                  if (segs.empty()) segs.push_back({ 0, {} });
                  segs.back().second.push_back({ w[0] == "ON", PlayedNote { w[2].toInt(), w[1].toLongLong() } });
                  }
            }
      if (!events.empty()) evaluateBlock(events, segs, block);
      return 0;
      }

int main(int argc, char** argv) {
      qSetGlobalQHashSeed(0);       // same note order inside chords on every run
      // tuning: SF_P_<NAME>=value overrides an alignment weight
      {
            struct { const char* n; double* v; } ps[] = {
                  { "MATCH", &ScoreFollower::MATCH }, { "OCTAVE", &ScoreFollower::OCTAVE }, { "EXTRA", &ScoreFollower::EXTRA },
                  { "ENTER", &ScoreFollower::ENTER }, { "SKIP_PER_NOTE", &ScoreFollower::SKIP_PER_NOTE },
                  { "SKIP_MAX", &ScoreFollower::SKIP_MAX }, { "SKIP_SINGLE", &ScoreFollower::SKIP_SINGLE }, { "FWD_MARGIN", &ScoreFollower::FWD_MARGIN },
                  { "FWD_PER_NOTE", &ScoreFollower::FWD_PER_NOTE }, { "BACK_MARGIN", &ScoreFollower::BACK_MARGIN } };
            for (auto& p : ps) {
                  const QByteArray v = qgetenv(QByteArray("SF_P_") + p.n);
                  if (!v.isEmpty()) *p.v = v.toDouble();
                  }
      }
      if (argc > 2 && !strcmp(argv[1], "--evaluate"))
            return evaluate(argv[2]);
      if (argc > 1)
            return replay(argv[1]);

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
      chord(f, {64,60,48});    CHECK(f.currentIndex()==1, "fumble: one deviating chord (= chord 1/4) does not jump");
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

      // verses: the piece ends and starts on the same chord; after the last chord
      // a new strike of the opening chord goes back to the beginning
      {
            std::vector<ScoreFollower::Event> v;
            const std::initializer_list<int> tonic = {67,64,60,48};
            v.push_back(ev(0, tonic));
            for (int i = 1; i < 11; ++i) v.push_back(ev(i * 480, {60 + i, 55 + i, 48 + i}));
            v.push_back(ev(11 * 480, tonic));
            reset(f, v);
            chord(f, tonic);
            for (int i = 1; i < 11; ++i) chord(f, {60 + i, 55 + i, 48 + i});
            CHECK(f.currentIndex()==10, "verse: before the last chord");
            chord(f, tonic); CHECK(f.currentIndex()==11, "verse: last chord");
            now += 3000;     // pause between the verses
            chord(f, tonic); CHECK(f.currentIndex()==0, "verse: opening chord again = next verse");
            chord(f, {61,56,49}); CHECK(f.currentIndex()==1, "verse: and on");
            for (int i = 2; i < 6; ++i) chord(f, {60 + i, 55 + i, 48 + i});
            chord(f, tonic); CHECK(f.currentIndex()==5, "verse: opening chord in the middle is not a restart");
      }

      // review 7: penultimate dominant, then the closing tonic (= opening chord),
      // spread over more than 60 ms: must go to the last chord, not restart
      {
            std::vector<ScoreFollower::Event> v;
            const std::initializer_list<int> tonic = {67,64,60,48};
            v.push_back(ev(0, tonic));
            for (int i = 1; i < 10; ++i) v.push_back(ev(i * 480, {60 + i, 55 + i, 48 + i}));
            v.push_back(ev(10 * 480, {67,62,59,55}));     // dominant
            v.push_back(ev(11 * 480, tonic));
            reset(f, v);
            chord(f, tonic);
            for (int i = 1; i < 10; ++i) chord(f, {60 + i, 55 + i, 48 + i});
            chord(f, {67,62,59,55}); CHECK(f.currentIndex()==10, "end: dominant");
            chordSpaced(f, tonic, 90); CHECK(f.currentIndex()==11, "end: rolled closing tonic goes to the last chord, no restart");
      }
      // short piece: verse restart from the fully played last chord
      reset(f, {ev(0,{60,64,67}), ev(480,{62,65,69}), ev(960,{60,64,67})});
      chord(f, {60,64,67}); chord(f, {62,65,69}); chord(f, {60,64,67});
      CHECK(f.currentIndex()==2, "short piece: last chord");
      now += 3000; chord(f, {60,64,67}); CHECK(f.currentIndex()==0, "short piece: next verse");

      // lost stays lost during continuous wrong notes (no flicker after a failed recovery)
      reset(f, hymn);
      chord(f, {67,64,60,48});
      for (int p : {30, 31, 33, 35, 37, 39, 30, 31, 33, 35, 37, 39}) chord(f, {p});
      CHECK(f.lost(), "lost: still lost after twelve wrong notes (failed recoveries)");

      // lost: several notes that fit nowhere near -> lost(), right chord -> not lost
      reset(f, hymn);
      chord(f, {67,64,60,48});
      CHECK(!f.lost(), "lost: not lost while following");
      for (int p : {30, 31, 33, 35}) chord(f, {p});
      CHECK(!f.lost(), "lost: four wrong notes are not yet 'lost'");
      for (int p : {37, 39}) chord(f, {p});
      CHECK(f.lost(), "lost: after six notes that fit nowhere");
      chord(f, {67,62,59,55}); CHECK(f.currentIndex()==1 && !f.lost(), "lost: right chord -> found again");

      // review 8: a skipped chord followed by two identical chords: the second strike
      // confirms (it must not re-propose and restart the confirmation)
      reset(f, {ev(0,{60,64,67}), ev(480,{62,65,69}), ev(960,{55,59,62}), ev(1440,{55,59,62}), ev(1920,{57,60,64}), ev(2400,{72})});
      chord(f, {60,64,67});
      chord(f, {55,59,62}); chord(f, {55,59,62}); CHECK(f.currentIndex()>=2, "skipped chord, then B B: follows (at most one behind)");
      chord(f, {57,60,64}); CHECK(f.currentIndex()==4, "and on");

      // mistakes: what fits, and the practice count per chord
      reset(f, hymn);
      chord(f, {67,64,60,48});
      CHECK(f.fits(64) && f.fits(36) && f.fits(62), "fits: chord note, octave (pedal), next chord");
      CHECK(!f.fits(61), "fits: a wrong note does not");
      chord(f, {61}); chord(f, {63});
      CHECK(f.mistakes().size() == hymn.size() && f.mistakes()[0] == 2, "two wrong notes counted at chord 1");
      f.restart(); CHECK(f.mistakes()[0] == 0, "Reset starts a new count");

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


      // recovery
      std::vector<ScoreFollower::Event> rec;
      for (int i = 0; i < 40; ++i) rec.push_back(ev(i*480, {40 + i}));
      reset(f, rec); chord(f,{40});
      for (int p : {48,49,50,51,52,53,54,55}) chord(f, {p});
      CHECK(f.currentIndex()==0 && f.lost(), "far away: no automatic jump, the cursor says lost (click a note)");

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
                        for (size_t q = 0; q < mv.size(); q += 2) {
                              int k = mv[q], np = mv[q + 1];
                              // one key cannot be down twice: another voice holding it lets go first
                              for (int o = 0; o < 4; ++o) if (o != k && heldKey[o] == np) { f.noteOff(np); heldKey[o] = -1; }
                              heldKey[k] = np; f.noteOn(np, t); t += 5;
                              }
                        if (i - f.currentIndex() > 2) {
                              printf("DEBUG2 round %d chord %d at %d (%s); new:", round, i, f.currentIndex(), qPrintable(f.lastReason()));
                              for (size_t q = 0; q < mv.size(); q += 2) printf(" %d", mv[q + 1]);
                              printf(" | events:"); for (int e = qMax(0, f.currentIndex()); e <= i; ++e) { printf(" [%d:", e); for (int p : piece[e].pitches) printf(" %d", p); printf("]"); }
                              puts("");
                              }
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
            CHECK(maxLead <= 2, "stress 3: at most two chords ahead (and it corrects itself)");
            CHECK(endOk == 10, "stress 3: always ends on the last chord");
      }

      // the real Psalm 25 (tools/scorefollower-test/psalm025.events): the F major
      // chord occurs six times, chords 15 and 16 are identical, bars 7-8 repeat bars 1-2
      {
            const std::vector<ScoreFollower::Event> ps = loadEvents("C:/ms3/follow/tools/scorefollower-test/psalm025.events");
            CHECK(ps.size() == 152, "Psalm 25: chord list loaded");
            if (ps.size() == 152) {
                  QFile::remove("C:/ms3/sftest/psalm25-scenario.log");
                  f.setLogFile("C:/ms3/sftest/psalm25-scenario.log");     // also checks the log/replay path
                  reset(f, ps);
                  for (int i = 0; i <= 21; ++i) strike(f, ps[i].pitches);
                  CHECK(f.currentIndex() == 21, "Psalm 25: followed to chord 21 (bar 8)");
                  // bar 8 played differently: two F major chords in other voicings
                  strike(f, QSet<int>({ 41, 57, 60, 65 }));
                  strike(f, QSet<int>({ 53, 57, 60, 65, 69 }));
                  CHECK(f.currentIndex() >= 21, "Psalm 25: other chords in bar 8 do not jump back to bar 7");
                  for (int i = 22; i <= 40; ++i) strike(f, ps[i].pitches);
                  CHECK(f.currentIndex() == 40, "Psalm 25: and on from there");
                  f.setLogFile(QString());

                  // whole psalm with mistakes, ten times
                  unsigned seed = 99;
                  auto rnd = [&](int n) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % unsigned(n)); };
                  int maxLead = 0, worstLag = 0, endOk = 0, maxBack = 0;
                  for (int round = 0; round < 30; ++round) {
                        reset(f, ps);
                        int prev = 0;
                        for (int i = 0; i < 152; ++i) {
                              auto watch = [&](const char* what, int bef) {
                                    const int d = f.currentIndex() - bef;
                                    if (d > 1 || d < -1) {
                                          printf("JUMP round %d at chord %d during %s: %d -> %d (%s)", round, i, what, bef, f.currentIndex(), qPrintable(f.lastReason()));
                                          puts("");
                                          }
                                    };
                              if (rnd(100) < 20) { int b = 1 + rnd(4); for (int k = 0; k < b; ++k) { now += 120; int w = 45 + rnd(30); int bef = f.currentIndex(); f.noteOn(w, now); f.noteOff(w); watch("wrong note", bef); } }
                              if (rnd(100) < 10) {      // a chord played differently: an F major voicing
                                    static const QSet<int> fmaj[3] = { {41,57,60,65}, {53,57,60,65,69}, {48,53,57,65} };
                                    int bef = f.currentIndex();
                                    now += 500; for (int p : down) f.noteOff(p); down.clear();
                                    qint64 t = now;
                                    for (int p : fmaj[rnd(3)]) {
                                          int b2 = f.currentIndex();
                                          f.noteOn(p, t); down.push_back(p); t += 5;
                                          if (f.currentIndex() != b2 && (f.currentIndex() - b2 > 1 || b2 - f.currentIndex() > 1)) {
                                                printf("  NOTE %d: %d -> %d (%s)", p, b2, f.currentIndex(), qPrintable(f.lastReason()));
                                                puts("");
                                                }
                                          }
                                    watch("other F chord", bef);
                                    }
                              const int bef = f.currentIndex();
                              strike(f, ps[i].pitches);
                              if (i - f.currentIndex() == 4) {
                                    printf("LAG round %d chord %d: follower %d (%s) | played:", round, i, f.currentIndex(), qPrintable(f.lastReason()));
                                    for (int p : ps[i].pitches) printf(" %d", p);
                                    printf(" | events:"); for (int e = f.currentIndex(); e <= i; ++e) { printf(" [%d:", e); for (int p : ps[e].pitches) printf(" %d", p); printf("]"); }
                                    puts("");
                                    }
                              if ((f.currentIndex() - i > 1 || bef - f.currentIndex() > 1) && maxLead <= 1 && maxBack <= 1) {
                                    printf("DEBUG-PS round %d chord %d: %d -> %d (%s)", round, i, bef, f.currentIndex(), qPrintable(f.lastReason()));
                                    puts("");
                                    }
                              maxLead  = qMax(maxLead, f.currentIndex() - i);
                              worstLag = qMax(worstLag, i - f.currentIndex());
                              maxBack  = qMax(maxBack, prev - f.currentIndex());
                              prev = f.currentIndex();
                              }
                        if (f.currentIndex() == 151) ++endOk;
                        }
                  printf("      Psalm 25 with mistakes: max lead %d, worst lag %d, biggest jump back %d, ended right %d/30", maxLead, worstLag, maxBack, endOk);
                  puts("");
                  CHECK(maxLead <= 3, "Psalm 25 with mistakes (extra chords inserted): at most three ahead, briefly");
                  CHECK(maxBack <= 1, "Psalm 25 with mistakes: never jumps back more than one chord");
                  CHECK(endOk == 30, "Psalm 25 with mistakes: always ends on the last chord");
                  }
      }

      printf(fails ? "\n%d FAILURES\n" : "\nALL OK\n", fails);
      return fails;
}
