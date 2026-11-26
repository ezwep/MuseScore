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
//   reset
//---------------------------------------------------------

void ScoreFollower::reset()
      {
      _cur     = -1;
      _misses  = 0;
      _matched.clear();
      _evidence.clear();
      _attacks.clear();
      _history.clear();
      }

//---------------------------------------------------------
//   setEvents (testing)
//---------------------------------------------------------

void ScoreFollower::setEvents(const std::vector<Event>& ev)
      {
      _score  = nullptr;
      _events = ev;
      reset();
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
      _misses  = 0;
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
//   fitsHistory
//    Can the played history end at event last? Attacks struck
//    together (within STRIKE_MS) must belong to the same event,
//    a later strike to a later event (at most two further).
//    The history must contain at least one such transition: a
//    single chord occurs everywhere and proves nothing.
//---------------------------------------------------------

bool ScoreFollower::fitsHistory(int last) const
      {
      const int k = int(_history.size());
      if (!_events[last].pitches.contains(_history[k - 1].pitch))
            return false;
      bool transition = false;
      QSet<int> reach { last };
      for (int t = k - 2; t >= 0; --t) {
            const bool sameStrike = _history[t + 1].time - _history[t].time <= STRIKE_MS;
            transition = transition || !sameStrike;
            QSet<int> prev;
            for (int r : reach) {
                  if (sameStrike) {
                        if (_events[r].pitches.contains(_history[t].pitch))
                              prev.insert(r);
                        }
                  else {
                        for (int j = r - 1; j >= qMax(0, r - 2); --j)
                              if (_events[j].pitches.contains(_history[t].pitch))
                                    prev.insert(j);
                        }
                  }
            if (prev.isEmpty())
                  return false;
            reach = prev;
            }
      return transition;
      }

//---------------------------------------------------------
//   recover
//    Search the whole score for the last HISTORY played
//    pitches. Returns the event index of the last pitch if
//    they fit in only one place, else -1.
//---------------------------------------------------------

int ScoreFollower::recover() const
      {
      if (int(_history.size()) < HISTORY)
            return -1;
      int first = -1;
      int found = -1;
      const int n = int(_events.size());
      for (int i = 0; i < n; ++i) {
            if (!fitsHistory(i))
                  continue;
            if (first < 0)
                  first = i;
            else if (i - first > RECOVER_SPAN)
                  return -1;        // fits in more than one place: ambiguous
            found = i;              // neighbouring candidates: take the latest
            }
      return found;
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
//   chooseCandidate
//    nearest event (next one first, then alternately back and
//    ahead) with enough evidence; -1 if none
//---------------------------------------------------------

int ScoreFollower::chooseCandidate() const
      {
      const int n = int(_events.size());
      for (int d = 1; d <= qMax(WINDOW, BACK); ++d) {
            for (int j : { _cur + d, _cur - d }) {
                  if (j < 0 || j >= n || j < _cur - BACK || j > _cur + WINDOW)
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
                  if (j == _cur + 1 ? hits * 2 < total : hits * 10 < total * 6)
                        continue;
                  // further away or back: the chord that was just struck (the
                  // attacks of the last STRIKE_MS) must contain enough of its
                  // pitches and fit it better than the current chord; re-striking
                  // the current chord is not a jump to a similar chord elsewhere
                  if (j != _cur + 1 && need >= 2) {
                        QSet<int> strike;
                        for (const Attack& a : _recent)
                              if (_now - a.time <= STRIKE_MS)
                                    strike.insert(a.pitch);
                        int candFit = 0, curFit = 0;
                        for (int p : strike) {
                              candFit += _events[j].pitches.contains(p);
                              curFit  += _events[_cur].pitches.contains(p);
                              }
                        if (candFit < need || candFit <= curFit)
                              continue;
                        }
                  // a far single note is too weak on its own: it must follow an
                  // earlier, separate attack on the event before it
                  if (need == 1 && j != _cur + 1) {
                        qint64 prevFirst = 0;
                        if (j == 0 || evidenceCount(j - 1, nullptr, &prevFirst) == 0 || prevFirst >= first)
                              continue;
                        }
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
      // A key that is already down sends no new attack: ignore repeated
      // note-ons (MIDI echo/thru, couplers on other channels).
      auto held = _held.constFind(pitch);
      if (held != _held.constEnd() && ms - held.value() < HELD_EXPIRE_MS)
            return false;
      _held.insert(pitch, ms);
      _now = ms;
      _recent.push_back(Attack { pitch, ms });
      if (_recent.size() > 32)
            _recent.erase(_recent.begin());

      if (!ensureEvents())
            return false;

      _history.push_back(Attack { pitch, ms });
      if (int(_history.size()) > HISTORY)
            _history.erase(_history.begin());

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
            _misses = 0;                // progress: a note of the current chord
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
      for (int j = qMax(0, _cur - BACK); j <= qMin(n - 1, _cur + WINDOW); ++j) {
            if (j != _cur && _events[j].pitches.contains(pitch)) {
                  _evidence[j].insert(pitch, Evidence { _now, explained });
                  anywhere = true;
                  }
            }

      // 4. move when an event has enough evidence
      const int j = chooseCandidate();
      if (j >= 0) {
            QSet<int> consumed;
            const auto& ev = _evidence[j];
            for (auto it = ev.constBegin(); it != ev.constEnd(); ++it)
                  if (_now - it.value().time < EVIDENCE_MS)
                        consumed.insert(it.key());
            setCurrent(j, consumed);
            return true;
            }
      if (explained)
            return false;

      // 5. no progress (also when the pitch occurs nearby but nothing moved):
      //    after several such attacks, search the whole score
      Q_UNUSED(anywhere);
      if (++_misses >= MAX_MISSES) {
            _misses = 0;
            int r = recover();
            if (r >= 0 && r != _cur) {
                  setCurrent(r, QSet<int>({ pitch }));
                  return true;
                  }
            }
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
      setCurrent(idx, QSet<int>());
      _history.clear();
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
      setCurrent(0, QSet<int>());
      _history.clear();
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
                  setCurrent(i, QSet<int>());
                  _history.clear();
                  _lastMove = -1000000;
                  return true;

                  }
            }
      return false;
      }

} // namespace Ms
