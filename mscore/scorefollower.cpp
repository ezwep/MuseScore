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
      }

//---------------------------------------------------------
//   ~ScoreFollower
//---------------------------------------------------------

ScoreFollower::~ScoreFollower()
      {
      delete _log;
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
            logLine(QString("E %1 %2 %3").arg(i).arg(_events[i].tick.ticks()).arg(sl.join(',')));
            }
      }

//---------------------------------------------------------
//   reset
//---------------------------------------------------------

void ScoreFollower::reset()
      {
      _cur     = -1;
      _stuck   = 0;
      _farCand = -1;
      _matched.clear();

      _evidence.clear();
      _attacks.clear();
      }

//---------------------------------------------------------
//   setEvents (testing)
//---------------------------------------------------------

void ScoreFollower::setEvents(const std::vector<Event>& ev)
      {
      _score  = nullptr;
      _events = ev;
      reset();
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
//    Make sure the event list matches the score.
//    Returns false when there is nothing to follow (or the
//    score is in the middle of an edit).
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
//    Build the list of struck chords in playback order
//    (repeats unrolled). Keeps the current position, in the
//    same repeat pass where possible.
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
                                    }
                              }
                        if (!ev.pitches.isEmpty())
                              _events.push_back(ev);
                        }
                  }
            }

      reset();
      if (hadPosition) {
            int idx = nearestRun(oldTick, oldIdx);
            if (idx >= 0)
                  setCurrent(idx, QSet<int>());
            }
      logEvents();
      }

//---------------------------------------------------------
//   setCurrent
//---------------------------------------------------------

void ScoreFollower::setCurrent(int idx, const QSet<int>& consumed)
      {
      if (idx != _cur)
            _lastMove = _now;
      _cur     = idx;
      _matched = consumed;
      _evidence.clear();
      _attacks.clear();
      _stuck   = 0;
      _farCand = -1;
      }

//---------------------------------------------------------
//   matchPitch
//    Returns the score pitch of event idx that the played
//    pitch matches (exactly, or with octave = true an octave
//    displaced pitch), skipping consumed pitches; -1 if none.
//---------------------------------------------------------

int ScoreFollower::matchPitch(int idx, int pitch, const QSet<int>& consumed, bool octave) const
      {
      const QSet<int>& pitches = _events[idx].pitches;
      if (!octave)
            return (pitches.contains(pitch) && !consumed.contains(pitch)) ? pitch : -1;
      int best = -1;
      for (int p : pitches) {
            if (p == pitch || (p - pitch) % 12 != 0 || consumed.contains(p))
                  continue;
            if (best < 0 || qAbs(p - pitch) < qAbs(best - pitch))
                  best = p;
            }
      return best;
      }

//---------------------------------------------------------
//   evidenceCount
//    number of different pitches of event idx played within
//    the last EVIDENCE_MS; optionally whether one of them did
//    not also fit the current chord, and the first/last time
//---------------------------------------------------------

int ScoreFollower::evidenceCount(int idx, bool* unexplained, qint64* first, qint64* last) const
      {
      if (unexplained)
            *unexplained = false;
      auto it = _evidence.constFind(idx);
      if (it == _evidence.constEnd())
            return 0;
      int n = 0;
      qint64 lo = LLONG_MAX, hi = LLONG_MIN;
      for (const Evidence& e : it.value()) {
            if (_now - e.time >= EVIDENCE_MS)
                  continue;
            ++n;
            lo = qMin(lo, e.time);
            hi = qMax(hi, e.time);
            if (!e.explained && unexplained)
                  *unexplained = true;
            }
      if (first)
            *first = lo;
      if (last)
            *last = hi;
      return n;
      }

//---------------------------------------------------------
//   needed
//    How many of its pitches must be played before we move to
//    event idx. The next chord needs two (one for a single
//    note); anything further away, or back, needs three, so
//    that one or two wrong notes never move the position.
//---------------------------------------------------------

int ScoreFollower::needed(int idx) const
      {
      const int size = _events[idx].pitches.size();
      if (idx == _cur + 1)
            return qMin(2, size);
      return qMin(3, size);
      }

//---------------------------------------------------------
//   lastStrike
//    pitches attacked within the last STRIKE_MS: the chord
//    that was just struck
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
//    At the end of the piece, striking the first chord means
//    the next verse: go back to the beginning.
//---------------------------------------------------------

bool ScoreFollower::nextVerse()
      {
      const int n = int(_events.size());
      if (_cur < n - VERSE_TAIL)
            return false;
      // between verses there is a pause: the attack before this strike must be
      // a while ago, else it is just a chord played differently at the end
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
      const QSet<int> strike = lastStrike();
      int fit = 0, curFit = 0;
      QSet<int> consumed;
      for (int p : strike) {
            if (first.contains(p)) {
                  ++fit;
                  consumed.insert(p);
                  }
            curFit += _events[_cur].pitches.contains(p);
            }
      // On the last chord, fully played, a new strike of the opening chord is
      // the next verse even when it is the same chord (hymns end and start on
      // the tonic). Otherwise it must fit the opening better than the current.
      const bool lastDone = _cur == n - 1 && _matched.size() >= _events[_cur].pitches.size();
      if (fit < need || (!lastDone && fit <= curFit))
            return false;
      // in a short piece the end is within reach of the start: only restart
      // from the fully played last chord
      if (!lastDone && n <= VERSE_TAIL + WINDOW + 1)
            return false;
      setCurrent(0, consumed);
      return true;
      }

//---------------------------------------------------------
//   struckBefore
//    Did one earlier strike (attacks within STRIKE_MS of each
//    other, all before 'before') hit at least 'need' pitches of
//    event idx?
//---------------------------------------------------------

bool ScoreFollower::struckBefore(int idx, int need, qint64 before) const
      {
      auto it = _evidence.constFind(idx);
      if (it == _evidence.constEnd())
            return false;
      std::vector<qint64> times;
      for (const Evidence& e : it.value())
            if (_now - e.time < EVIDENCE_MS && e.time < before)
                  times.push_back(e.time);
      std::sort(times.begin(), times.end());
      for (size_t i = 0; i + need <= times.size(); ++i)
            if (times[i + need - 1] - times[i] <= STRIKE_MS)
                  return true;
      return false;
      }

//---------------------------------------------------------
//   farPending
//    a jump proposed by an earlier strike that may still be
//    confirmed
//---------------------------------------------------------

bool ScoreFollower::farPending() const
      {
      return _farCand >= 0 && _now - _farCandTime < 2 * EVIDENCE_MS;
      }

//---------------------------------------------------------
//   chooseCandidate
//    nearest event (next one first, then alternately back and
//    ahead) with enough evidence; -1 if none.
//    A jump other than to the next chord needs two separate
//    strikes: the first one only proposes it (_farCand), the
//    next one must fit the event after it.
//---------------------------------------------------------

int ScoreFollower::chooseCandidate()
      {
      const int n = int(_events.size());
      const bool pending = farPending();
      bool proposed = false;
      // order: the next chord, further ahead, and only then back. A player moves
      // forward, so with identical chords before and after, ahead is more likely.
      std::vector<int> order;
      if (pending)
            order.push_back(_farCand + 1);      // a confirmation is checked before anything else
      for (int d = 1; d <= WINDOW + 1; ++d)
            order.push_back(_cur + d);
      for (int d = 1; d <= BACK + 1; ++d)
            order.push_back(_cur - d);
      for (int j : order) {
            {
                  if (j < 0 || j >= n)
                        continue;
                  // the confirming strike must show progress: the chord after the
                  // proposed one (the same chord again proves nothing: F major is everywhere)
                  const bool confirmTarget = pending && j == _farCand + 1;
                  if (!confirmTarget && (j < _cur - BACK || j > _cur + WINDOW))
                        continue;
                  const int need = needed(j);
                  bool unexplained = false;
                  qint64 first = 0;
                  if (evidenceCount(j, &unexplained, &first) < need)
                        continue;
                  // at least one attack must not be explained by the current chord
                  // (a late or re-voiced note of it), else repeated chords run ahead
                  if (!unexplained)
                        continue;
                  // and the candidate must explain most of the attacks that did not
                  // fit the current chord (half for the next chord, 60% otherwise)
                  // (for a far single note, the note before it explains attacks too)
                  const bool seq = need == 1 && j != _cur + 1 && j > 0;
                  // only the last RATIO_ATTACKS such attacks count: older ones may be
                  // right notes that were not accepted yet, they must not hold us back
                  int total = 0;
                  int hits = 0;
                  for (auto ai = _attacks.crbegin(); ai != _attacks.crend() && total < RATIO_ATTACKS; ++ai) {
                        const Attack& a = *ai;
                        if (_now - a.time >= EVIDENCE_MS)
                              break;
                        ++total;
                        if (_events[j].pitches.contains(a.pitch) || (seq && _events[j - 1].pitches.contains(a.pitch)))
                              ++hits;
                        }
                  // ... unless the chord just struck consists only of its notes (enough
                  // of them): a clean strike after some wrong notes speaks for itself
                  bool cleanStrike = false;
                  {
                        const QSet<int> strike = lastStrike();
                        int in = 0;
                        for (int p : strike)
                              in += _events[j].pitches.contains(p);
                        cleanStrike = !strike.isEmpty() && in == strike.size() && in >= need;
                        }
                  if (!cleanStrike && (j == _cur + 1 ? hits * 2 < total : hits * 10 < total * 6))
                        continue;
                  // further away or back: the chord that was just struck (the
                  // attacks of the last STRIKE_MS) must contain enough of its
                  // pitches and fit it better than the current chord; re-striking
                  // the current chord is not a jump to a similar chord elsewhere
                  if (j != _cur + 1 && need >= 2) {
                        const QSet<int> strike = lastStrike();
                        int candFit = 0, curFit = 0;
                        for (int p : strike) {
                              candFit += _events[j].pitches.contains(p);
                              curFit  += _events[_cur].pitches.contains(p);
                              }
                        if (candFit < need)
                              continue;
                        const bool newStrike = _now - _farCandTime > STRIKE_MS;
                        // second strike confirming the proposed jump (it may equal the
                        // current chord in fit: the sequence is the evidence now)
                        if (confirmTarget && newStrike && candFit >= curFit) {
                              _why = QString("confirmed %1").arg(j);
                              return j;
                              }
                        if (candFit <= curFit)
                              continue;
                        // first strike pointing here: only propose. Not again for the
                        // pending one (that would restart its clock), and not where no
                        // confirmation is possible: the chord just before the current
                        // one (its successor is where we are) or the very last chord.
                        if (pending && j == _farCand)
                              continue;
                        if (j == _cur - 1 || j == n - 1)
                              continue;
                        if (!proposed && (!pending || newStrike)) {
                              _farCand     = j;
                              _farCandTime = _now;
                              _why = QString("proposed %1").arg(j);
                              }
                        proposed = true;
                        continue;
                        }
                  // a far single note is too weak on its own: it must follow an
                  // earlier, separate attack (not part of the same strike) on the
                  // event before it (of a chord: at least two of its notes, not one
                  // stray note). Never backwards: going back needs a confirmed chord.
                  if (need == 1 && j < _cur)
                        continue;
                  if (need == 1 && j != _cur + 1) {
                        if (j == 0 || !struckBefore(j - 1, qMin(2, int(_events[j - 1].pitches.size())), first - STRIKE_MS))
                              continue;
                        }
                  _why = QString(j == _cur + 1 ? "next %1" : "sequence %1").arg(j);
                  return j;
                  }
            }
      return -1;
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

      // Not positioned yet (no note clicked): start at the first chord. Letting
      // the first incoming note pick any chord in the look-ahead window made an
      // early pedal note jump straight to the third chord.
      if (!hasPosition())
            setCurrent(0, QSet<int>());

      // 1. notes struck together with the chord we just moved to belong to that
      //    chord: they may only complete it, never move on
      int sp = matchPitch(_cur, pitch, _matched, false);
      if (_now - _lastMove < CHORD_MS) {
            if (sp < 0)
                  sp = matchPitch(_cur, pitch, _matched, true);
            if (sp >= 0)
                  _matched.insert(sp);
            _why = sp >= 0 ? "chord-window current" : "chord-window ignored";
            return false;
            }

      // 2. a still unplayed note of the current chord, exact or an octave
      //    displaced (pedal 16'), completes it
      if (sp < 0)
            sp = matchPitch(_cur, pitch, _matched, true);
      const bool explained = sp >= 0;
      if (explained) {
            if (_matched.isEmpty())
                  _lastMove = _now;     // first attack of a chord we were positioned at: arm the chord window
            _matched.insert(sp);
            _stuck  = 0;                // progress: a note of the current chord
            }
      else {
            _attacks.push_back(Attack { pitch, _now });
            if (_attacks.size() > 32)
                  _attacks.erase(_attacks.begin());
            }

      // 3. every attack is evidence for the events around the current one
      //    (exact pitches only: octave matches ahead are unreliable with 4' couplers)
      const int n = int(_events.size());
      bool anywhere = false;
      const int hi = qMin(n - 1, qMax(_cur + WINDOW, farPending() ? _farCand + 1 : 0));
      for (int j = qMax(0, _cur - BACK); j <= hi; ++j) {
            if (j != _cur && _events[j].pitches.contains(pitch)) {
                  _evidence[j].insert(pitch, Evidence { _now, explained });
                  anywhere = true;
                  }
            }

      // 4. move when an event has enough evidence; at the end of the piece a
      //    strike of the opening chord means the next verse
      const int j = chooseCandidate();
      if (j < 0 && !explained && nextVerse()) {
            _why = "verse";
            return true;
            }
      if (j >= 0) {
            QSet<int> consumed;
            const auto& ev = _evidence[j];
            for (auto it = ev.constBegin(); it != ev.constEnd(); ++it)
                  if (_now - it.value().time < EVIDENCE_MS)
                        consumed.insert(it.key());
            setCurrent(j, consumed);
            return true;
            }
      if (explained) {
            if (_why.isEmpty())
                  _why = "current";
            return false;
            }

      // 5. no progress (also when the pitch occurs nearby but nothing moved).
      //    There is deliberately no search through the whole score: hymns repeat
      //    their lines and chords, so such a search finds wrong places and jumps
      //    there. After several such attacks lost() turns the cursor orange and
      //    the player clicks a note (or uses Reset / Next System).
      ++_stuck;
      if (_why.isEmpty())
            _why = anywhere ? "evidence" : "miss";
      return false;
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
      setCurrent(idx, QSet<int>());
      _lastMove = -1000000;
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
      setCurrent(0, QSet<int>());
      _lastMove = -1000000;
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
                  setCurrent(i, QSet<int>());
                              _lastMove = -1000000;
                  return true;

                  }
            }
      return false;
      }

} // namespace Ms
