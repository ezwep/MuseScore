//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2026 MuseScore Evolution contributors
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "scorefollower.h"

#include <algorithm>
#include <climits>

#include <QFile>
#include <QTextStream>
#include <QDateTime>

#include "libmscore/score.h"
#include "libmscore/measure.h"
#include "libmscore/segment.h"
#include "libmscore/chord.h"
#include "libmscore/note.h"
#include "libmscore/staff.h"
#include "libmscore/part.h"
#include "libmscore/repeatlist.h"
#include "libmscore/undo.h"

namespace Ms {

static constexpr double NONE = -1e9;      // a position that is not alive
static inline bool alive(double v) { return v > NONE / 2; }

//---------------------------------------------------------
//   ~ScoreFollower
//---------------------------------------------------------

ScoreFollower::~ScoreFollower()
      {
      delete _log;
      }

//---------------------------------------------------------
//   setScore
//---------------------------------------------------------

void ScoreFollower::setScore(Score* s)
      {
      if (s == _score)
            return;
      _score = s;
      _events.clear();
      _undoState = -1;
      reset();
      _mistakes.clear();
      }

//---------------------------------------------------------
//   reset
//---------------------------------------------------------

void ScoreFollower::reset()
      {
      _v.clear();
      _used.clear();
      _lo = 0;
      _hi = -1;
      _cur = -1;
      _stuck = 0;
      _recent.clear();
      }

//---------------------------------------------------------
//   setEvents (testing)
//---------------------------------------------------------

void ScoreFollower::setEvents(const std::vector<Event>& ev)
      {
      _score  = nullptr;
      _events = ev;
      reset();
      clearMistakes();
      logEvents();
      }

//---------------------------------------------------------
//   needsRebuild
//---------------------------------------------------------

bool ScoreFollower::needsRebuild() const
      {
      if (!_score)
            return false;
      return _undoState != _score->undoStack()->state()
             || _repeats != _score->masterScore()->expandRepeats();
      }

//---------------------------------------------------------
//   ensureEvents
//    Make sure the event list matches the score. Returns false
//    when there is nothing to follow (or the score is in the
//    middle of an edit).
//---------------------------------------------------------

bool ScoreFollower::ensureEvents()
      {
      if (_score && _score->undoStack()->active())
            return false;
      if (needsRebuild())
            rebuild();
      return !_events.empty();
      }

//---------------------------------------------------------
//   rebuild
//    Build the list of struck chords in playback order (repeats
//    unrolled). Notes in parentheses (ghost notes) are optional:
//    an event of only such notes may be skipped for free.
//    Keeps the current position, in the same repeat pass where
//    possible.
//---------------------------------------------------------

void ScoreFollower::rebuild()
      {
      const bool hadPosition = hasPosition();
      const int oldIdx       = _cur;
      const Fraction oldTick = tick();

      _events.clear();
      _undoState = _score->undoStack()->state();
      _repeats   = _score->masterScore()->expandRepeats();

      const RepeatList& rl = _score->masterScore()->repeatList();
      const int ntracks = _score->ntracks();
      for (const RepeatSegment* rs : rl) {
            const Fraction start = Fraction::fromTicks(rs->tick);
            const Fraction end   = Fraction::fromTicks(rs->tick + rs->len());
            for (Measure* m = _score->tick2measure(start); m && m->tick() < end; m = m->nextMeasure()) {
                  for (Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                        Event ev;
                        ev.tick = s->tick();
                        bool allGhost = true;
                        for (int track = 0; track < ntracks; ++track) {
                              Element* e = s->element(track);
                              if (!e || !e->isChord())
                                    continue;
                              const Staff* st = _score->staff(track / VOICES);
                              if (!st || !st->show() || st->isDrumStaff(ev.tick))
                                    continue;
                              for (const Note* n : toChord(e)->notes()) {
                                    if (n->tieBack() || !n->play())
                                          continue;
                                    ev.pitches.insert(n->ppitch());
                                    allGhost = allGhost && n->ghost();
                                    }
                              }
                        ev.optional = allGhost;
                        if (!ev.pitches.isEmpty())
                              _events.push_back(ev);
                        }
                  }
            }

      reset();
      clearMistakes();
      if (hadPosition) {
            int idx = nearestRun(oldTick, oldIdx);
            if (idx >= 0)
                  startAt(idx);
            }
      logEvents();
      }

//---------------------------------------------------------
//   startAt
//    (re)start the alignment with the player at event idx
//---------------------------------------------------------

void ScoreFollower::startAt(int idx)
      {
      const int n = int(_events.size());
      _v.assign(n, NONE);
      _used.assign(n, QSet<int>());
      _stuck = 0;
      _cur = idx;
      if (idx < 0 || idx >= n) {
            _cur = -1;
            _lo = 0;
            _hi = -1;
            return;
            }
      _v[idx] = 0.0;
      _lo = qMax(0, idx - BEHIND);
      _hi = qMin(n - 1, idx + AHEAD);
      }

//---------------------------------------------------------
//   skipCost
//    cost of passing event idx without playing it: a single
//    (passing) note is often left out, a full chord rarely;
//    optional (ghost) notes cost nothing
//---------------------------------------------------------

double ScoreFollower::skipCost(int idx, int from) const
      {
      const Event& e = _events[idx];
      if (e.optional)
            return 0.0;
      // A repeated chord held through: event idx right after 'from' with only
      // notes of 'from', all still held down, is being played by holding them.
      // (Only then: a free pass elsewhere would make jumps cheap.)
      if (from >= 0 && idx == from + 1) {
            bool heldThrough = true;
            for (int p : e.pitches) {
                  auto h = _held.constFind(p);
                  if (!_events[from].pitches.contains(p) || h == _held.constEnd() || _now - h.value() >= HELD_EXPIRE_MS) {
                        heldThrough = false;
                        break;
                        }
                  }
            if (heldThrough)
                  return 0.0;
            }
      if (e.pitches.size() == 1)
            return SKIP_SINGLE;     // a passing note left out is common
      return qMin(SKIP_MAX, SKIP_PER_NOTE * e.pitches.size());
      }

//---------------------------------------------------------
//   forwardMargin
//    lead over the shown position that event j ahead needs to
//    be shown: more for a bigger chord, plus what lies between
//---------------------------------------------------------

double ScoreFollower::forwardMargin(int j) const
      {
      // keys still held down that belong to event j count as present: an
      // organist holds a repeated note instead of re-striking it (Psalm 25,
      // bar 4: the F held, only the A struck again)
      int present = 0;
      for (int p : _events[j].pitches) {
            auto h = _held.constFind(p);
            if (h != _held.constEnd() && _now - h.value() < HELD_EXPIRE_MS && !_used[j].contains(p))
                  ++present;
            }
      const int size = qMax(1, qMin(4, int(_events[j].pitches.size()) - present));
      // skipping costs once more on top of the scores: a deliberate brake,
      // measured to keep wrong notes and other voicings from jumping ahead
      double m = FWD_MARGIN + FWD_PER_NOTE * (size - 1);
      for (int k = _cur + 1; k < j; ++k)
            m += skipCost(k, k == _cur + 1 ? _cur : -1);
      return m;
      }

//---------------------------------------------------------
//   reward
//    what a played pitch is worth for "the player is at event
//    idx", given the pitches of it already played (used). On a
//    match, *scorePitch is the score pitch it stands for.
//---------------------------------------------------------

double ScoreFollower::reward(int idx, int pitch, const QSet<int>& used, int* scorePitch, bool octave) const
      {
      const QSet<int>& ps = _events[idx].pitches;
      *scorePitch = -1;
      if (ps.contains(pitch) && !used.contains(pitch)) {
            *scorePitch = pitch;
            return MATCH;
            }
      // exactly one octave from a pitch not yet played (pedal 16' or 4')
      if (octave) {
            for (int p : { pitch + 12, pitch - 12 }) {
                  if (ps.contains(p) && !used.contains(p)) {
                        *scorePitch = p;
                        return OCTAVE;
                        }
                  }
            }
      return EXTRA;
      }

//---------------------------------------------------------
//   lastStrike
//    pitches attacked within the last STRIKE_MS
//---------------------------------------------------------

QSet<int> ScoreFollower::lastStrike() const
      {
      QSet<int> strike;
      for (const Attack& a : _recent)
            if (_now - a.time <= STRIKE_MS)
                  strike.insert(a.pitch);
      return strike;
      }

//---------------------------------------------------------
//   nextVerse
//    At the end of the piece, after a pause, striking the
//    opening chord means the next verse.
//---------------------------------------------------------

bool ScoreFollower::nextVerse()
      {
      const int n = int(_events.size());
      if (_cur < n - VERSE_TAIL)
            return false;
      qint64 previous = LLONG_MIN;
      for (const Attack& a : _recent)
            if (_now - a.time > STRIKE_MS)
                  previous = qMax(previous, a.time);
      if (previous != LLONG_MIN && _now - previous < VERSE_PAUSE_MS)
            return false;
      const QSet<int>& first = _events[0].pitches;
      const int need = qMin(3, first.size());
      if (need < 2)
            return false;           // a single note is too weak to restart on
      int fit = 0, curFit = 0;
      QSet<int> played;
      for (int p : lastStrike()) {
            if (first.contains(p)) {
                  ++fit;
                  played.insert(p);
                  }
            curFit += _events[_cur].pitches.contains(p);
            }
      // on the fully played last chord the opening chord may equal it
      // (hymns end and start on the tonic); otherwise it must fit better
      const bool lastDone = _cur == n - 1 && _used[_cur].size() >= _events[_cur].pitches.size();
      if (fit < need || (!lastDone && fit <= curFit))
            return false;
      if (!lastDone && n <= VERSE_TAIL + AHEAD)
            return false;           // short piece: only from the fully played last chord
      startAt(0);
      _used[0] = played;
      _v[0] = MATCH * played.size();
      return true;
      }

//---------------------------------------------------------
//   noteOn
//---------------------------------------------------------

bool ScoreFollower::noteOn(int pitch, qint64 ms)
      {
      const int before = _cur;
      _why.clear();
      const bool moved = noteOnImpl(pitch, ms);
      if (_log)
            logLine(QString("ON %1 %2 %3->%4 %5%6").arg(ms).arg(pitch).arg(before).arg(_cur)
                    .arg(_why.isEmpty() ? QString("-") : _why).arg(lost() ? " LOST" : ""));
      return moved;
      }

//---------------------------------------------------------
//   noteOff
//---------------------------------------------------------

void ScoreFollower::noteOff(int pitch, qint64 ms)
      {
      _held.remove(pitch);
      if (_log)
            logLine(QString("OFF %1 %2").arg(ms).arg(pitch));
      }

//---------------------------------------------------------
//   noteOnImpl
//    one step of the alignment
//---------------------------------------------------------

bool ScoreFollower::noteOnImpl(int pitch, qint64 ms)
      {
      // A key that is already down sends no new attack: ignore repeated
      // note-ons (MIDI echo/thru, couplers on other channels).
      auto held = _held.constFind(pitch);
      if (held != _held.constEnd() && ms - held.value() < HELD_EXPIRE_MS) {
            _why = "held";
            return false;
            }
      _held.insert(pitch, ms);
      _now = ms;
      _recent.push_back(Attack { pitch, ms });
      if (_recent.size() > 32)
            _recent.erase(_recent.begin());

      if (!ensureEvents()) {
            _why = "no-events";
            return false;
            }
      // far outside the range of the piece (pistons and toe studs that send a
      // note, e.g. 95/96 on this console): not a played note at all
      {
            int lo = 127, hi = 0;
            for (const Event& e : _events)
                  for (int p : e.pitches) { lo = qMin(lo, p); hi = qMax(hi, p); }
            if (pitch > hi + 12 || pitch < lo - 24) {
                  _why = "outside the piece";
                  return false;
                  }
      }
      if (!hasPosition())
            startAt(0);
      if (int(_v.size()) != int(_events.size()))
            startAt(qMin(_cur, int(_events.size()) - 1));

      if (nextVerse()) {
            _why = "verse";
            return true;
            }

      // --- update every live position ---------------------------------
      const int n  = int(_events.size());
      const int lo = qMax(0, _cur - BEHIND);
      const int hi = qMin(n - 1, _cur + AHEAD);
      std::vector<double> nv(n, NONE);
      std::vector<QSet<int>> nused(n);
      std::vector<bool> nmatch(n, false);
      for (int i = lo; i <= hi; ++i) {
            double best = NONE;
            // stay: the player is (still) at event i
            if (alive(_v[i])) {
                  int sp;
                  const double r = reward(i, pitch, _used[i], &sp, true);
                  best = _v[i] + r;
                  nused[i] = _used[i];
                  if (sp >= 0)
                        nused[i].insert(sp);
                  nmatch[i] = sp >= 0;
                  }
            // enter: the player moved on to event i from k (skipping k+1 .. i-1).
            // Only with a note of event i: a wrong note is no reason to move on
            // (else every wrong note would be a cheap step forward).
            int spEnter;
            const double rEnter = reward(i, pitch, QSet<int>(), &spEnter, false);
            double further = 0.0;       // skip costs of k+2 .. i-1
            for (int k = i - 1; spEnter >= 0 && k >= qMax(lo, i - 1 - MAX_SKIP); --k) {
                  // the event right after k may be passed by holding its notes
                  const double skipped = k < i - 1 ? further + skipCost(k + 1, k) : 0.0;
                  if (k < i - 1)
                        further += skipCost(k + 1, -1);
                  if (!alive(_v[k]))
                        continue;
                  const int sp = spEnter;
                  const double r = rEnter;
                  const double val = _v[k] + ENTER - skipped + r;
                  if (val > best) {
                        best = val;
                        nused[i].clear();
                        if (sp >= 0)
                              nused[i].insert(sp);
                        nmatch[i] = sp >= 0;
                        }
                  }
            nv[i] = best;
            }

      // keep the numbers small
      double top = NONE;
      int b = -1;
      for (int i = lo; i <= hi; ++i) {
            if (!alive(nv[i]))
                  continue;
            // best position; on a tie the one nearest to the shown position
            if (nv[i] > top + 1e-9 || (qAbs(nv[i] - top) <= 1e-9 && qAbs(i - _cur) < qAbs(b - _cur))) {
                  top = nv[i];
                  b = i;
                  }
            }
      if (b < 0) {                  // cannot happen: the shown position stays alive
            startAt(_cur);
            _why = "restart (no live position)";
            return false;
            }
      for (int i = lo; i <= hi; ++i)
            if (alive(nv[i]))
                  nv[i] -= top;
      _v.swap(nv);
      _used.swap(nused);
      _lo = lo;
      _hi = hi;

      // --- the shown position follows the best one, with hysteresis ----
      _stuck = nmatch[b] ? 0 : _stuck + 1;
      const int old = _cur;
      if (b > _cur) {
            // a chord needs two of its notes before it is shown (a single
            // note one), and the further ahead, the clearer the lead must be
            if (!alive(_v[_cur]) || _v[b] - _v[_cur] >= forwardMargin(b))
                  _cur = b;
            else {
                  // perhaps a nearer one ahead is clear enough
                  for (int j = b - 1; j > _cur; --j) {
                        if (alive(_v[j]) && _v[j] - _v[_cur] >= forwardMargin(j)) {
                              _cur = j;
                              break;
                              }
                        }
                  }
            }
      else if (b < _cur) {
            if (!alive(_v[_cur]) || _v[b] - _v[_cur] >= BACK_MARGIN)
                  _cur = b;
            }
      if (!fits(pitch) && _cur >= 0) {
            if (int(_mistakes.size()) != n)
                  _mistakes.assign(n, 0);
            ++_mistakes[_cur];
            }
      _why = QString("%1 best=%2%3").arg(_cur > old ? "ahead" : _cur < old ? "back" : "stay")
             .arg(b).arg(nmatch[b] ? "" : " nomatch");
      return _cur != old;
      }

//---------------------------------------------------------
//   nearestRun
//    Among the runs of events at or after tick (there can be
//    several with repeats) return the start of the one
//    nearest to index hint (or the first one if hint < 0).
//---------------------------------------------------------

int ScoreFollower::nearestRun(const Fraction& t, int hint) const
      {
      const int n = int(_events.size());
      int best = -1;
      int bestDist = INT_MAX;
      for (int i = 0; i < n; ++i) {
            if (_events[i].tick < t)
                  continue;
            // only the first event of a run that reaches t
            if (i > 0 && _events[i - 1].tick >= t && _events[i - 1].tick < _events[i].tick)
                  continue;
            int dist = hint >= 0 ? qAbs(i - hint) : i;
            if (dist < bestDist) {
                  best = i;
                  bestDist = dist;
                  }
            }
      return best;
      }

//---------------------------------------------------------
//   setPosition
//---------------------------------------------------------

bool ScoreFollower::setPosition(const Fraction& t)
      {
      if (!ensureEvents())
            return false;
      int idx = nearestRun(t, hasPosition() ? _cur : -1);
      if (idx < 0)
            return false;
      if (_log)
            logLine(QString("POS %1 %2->%3 tick=%4 set").arg(_clock.elapsed()).arg(_cur).arg(idx).arg(t.ticks()));
      startAt(idx);
      return true;
      }

//---------------------------------------------------------
//   restart
//---------------------------------------------------------

bool ScoreFollower::restart()
      {
      if (!ensureEvents())
            return false;
      if (_log)
            logLine(QString("POS %1 %2->0 tick=0 restart").arg(_clock.elapsed()).arg(_cur));
      startAt(0);
      clearMistakes();
      return true;
      }

//---------------------------------------------------------
//   advanceTo
//    Move forward to the first event at or after tick. A
//    repeat jump before reaching it counts as the destination
//    (it is what will be played next). Never moves backward.
//---------------------------------------------------------

bool ScoreFollower::advanceTo(const Fraction& t)
      {
      if (!ensureEvents())
            return false;
      if (!hasPosition())
            return setPosition(t);
      const int n = int(_events.size());
      for (int i = _cur + 1; i < n; ++i) {
            if (_events[i].tick < _events[i - 1].tick || _events[i].tick >= t) {
                  if (_log)
                        logLine(QString("POS %1 %2->%3 tick=%4 advance").arg(_clock.elapsed()).arg(_cur).arg(i).arg(t.ticks()));
                  startAt(i);
                  return true;
                  }
            }
      return false;
      }

//---------------------------------------------------------
//   setLogFile
//---------------------------------------------------------

void ScoreFollower::setLogFile(const QString& path)
      {
      delete _log;
      _log = nullptr;
      if (path.isEmpty())
            return;
      _log = new QFile(path);
      if (!_log->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            qWarning("ScoreFollower: cannot write the log %s", qPrintable(path));
            delete _log;
            _log = nullptr;
            return;
            }
      _logLines = 0;
      logLine(QString("START %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
      logEvents();
      }

//---------------------------------------------------------
//   logLine
//---------------------------------------------------------

void ScoreFollower::logLine(const QString& line)
      {
      if (!_log)
            return;
      // keep it small: every so often, when it grew beyond LOG_MAX_BYTES, the
      // current file becomes .1 (replacing an older one) and a new one starts
      if (++_logLines % 1000 == 0 && _log->size() > LOG_MAX_BYTES) {
            const QString path = _log->fileName();
            _log->close();
            QFile::remove(path + ".1");
            QFile::rename(path, path + ".1");
            if (!_log->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                  delete _log;
                  _log = nullptr;
                  return;
                  }
            logEvents();
            }
      _log->write(line.toUtf8());
      _log->write("\n");
      _log->flush();            // a few notes per second: cheap, and nothing is lost on a crash
      }

//---------------------------------------------------------
//   logEvents
//    the chord list, so that a log can be replayed exactly
//---------------------------------------------------------

void ScoreFollower::logEvents()
      {
      if (!_log)
            return;
      logLine(QString("EVENTS %1").arg(_events.size()));
      for (size_t i = 0; i < _events.size(); ++i) {
            QList<int> ps = _events[i].pitches.values();
            std::sort(ps.begin(), ps.end());
            QStringList sl;
            for (int p : ps)
                  sl << QString::number(p);
            logLine(QString("E %1 %2 %3%4").arg(i).arg(_events[i].tick.ticks()).arg(sl.join(','))
                    .arg(_events[i].optional ? " opt" : ""));
            }
      }

//---------------------------------------------------------
//   currentPlayed
//---------------------------------------------------------

QSet<int> ScoreFollower::currentPlayed() const
      {
      if (!hasPosition() || _cur >= int(_used.size()))
            return QSet<int>();
      return _used[_cur];
      }

//---------------------------------------------------------
//   fits
//---------------------------------------------------------

bool ScoreFollower::fits(int pitch) const
      {
      if (!hasPosition())
            return true;
      const QSet<int>& cur = _events[_cur].pitches;
      if (cur.contains(pitch) || cur.contains(pitch + 12) || cur.contains(pitch - 12))
            return true;
      return _cur + 1 < int(_events.size()) && _events[_cur + 1].pitches.contains(pitch);
      }

//---------------------------------------------------------
//   debugState
//    for the test tool: shown position and the live scores
//---------------------------------------------------------

QString ScoreFollower::debugState() const
      {
      QStringList l;
      for (int i = _lo; i <= _hi && i < int(_v.size()); ++i) {
            if (!alive(_v[i]))
                  continue;
            QStringList u;
            for (int p : _used[i])
                  u << QString::number(p);
            l << QString("%1:%2{%3}").arg(i).arg(_v[i], 0, 'f', 1).arg(u.join(','));
            }
      return QString("cur=%1 stuck=%2 | %3").arg(_cur).arg(_stuck).arg(l.join(' '));
      }

} // namespace Ms
