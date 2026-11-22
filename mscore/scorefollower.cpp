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
      _pending = -1;
      _misses  = 0;
      _matched.clear();
      _pendingMatched.clear();
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
      _cur     = idx;
      _matched = consumed;
      _pending = -1;
      _pendingMatched.clear();
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
//   findForward
//    first event in [from, to] matching pitch
//---------------------------------------------------------

int ScoreFollower::findForward(int from, int to, int pitch, bool octave, int* scorePitch) const
      {
      from = qMax(from, 0);
      to   = qMin(to, int(_events.size()) - 1);
      static const QSet<int> none;
      for (int i = from; i <= to; ++i) {
            int sp = matchPitch(i, pitch, none, octave);
            if (sp >= 0) {
                  *scorePitch = sp;
                  return i;
                  }
            }
      return -1;
      }

//---------------------------------------------------------
//   fitsHistory
//    Can the played history end at event last? Each earlier
//    pitch must be in the same chord as the next one, or up
//    to two chords before it. All feasible paths are kept.
//---------------------------------------------------------

bool ScoreFollower::fitsHistory(int last) const
      {
      const int k = int(_history.size());
      if (!_events[last].pitches.contains(_history[k - 1]))
            return false;
      QSet<int> reach { last };
      for (int t = k - 2; t >= 0; --t) {
            QSet<int> prev;
            for (int r : reach) {
                  for (int j = r; j >= qMax(0, r - 2); --j) {
                        if (_events[j].pitches.contains(_history[t]))
                              prev.insert(j);
                        }
                  }
            if (prev.isEmpty())
                  return false;
            reach = prev;
            }
      return true;
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
//   noteOn
//---------------------------------------------------------

bool ScoreFollower::noteOn(int pitch)
      {
      if (!ensureEvents())
            return false;

      _history.push_back(pitch);
      if (int(_history.size()) > HISTORY)
            _history.erase(_history.begin());

      int sp = -1;

      // 1. exact, still unplayed note of the current chord
      if (hasPosition() && (sp = matchPitch(_cur, pitch, _matched, false)) >= 0) {
            _matched.insert(sp);
            _misses  = 0;
            _pending = -1;
            _pendingMatched.clear();
            return false;
            }

      // 2. confirm a pending larger jump: another note of that chord,
      //    or a note of one of the chords right after it
      if (_pending >= 0) {
            if ((sp = matchPitch(_pending, pitch, _pendingMatched, false)) >= 0) {
                  QSet<int> consumed = _pendingMatched;
                  consumed.insert(sp);
                  setCurrent(_pending, consumed);
                  return true;
                  }
            int j = findForward(_pending + 1, _pending + DIRECT_JUMP, pitch, false, &sp);
            if (j >= 0) {
                  setCurrent(j, QSet<int>({ sp }));
                  return true;
                  }
            _pending = -1;
            _pendingMatched.clear();
            }

      // 3. candidates: exact match ahead; octave match in the current or a nearby chord
      int spExact = -1;
      const int jExact = findForward(_cur + 1, _cur + WINDOW, pitch, false, &spExact);
      int spOct = -1;
      int jOct  = -1;
      if (hasPosition() && (spOct = matchPitch(_cur, pitch, _matched, true)) >= 0)
            jOct = _cur;
      else
            jOct = findForward(_cur + 1, _cur + DIRECT_JUMP, pitch, true, &spOct);

      // a nearby exact match beats an octave match
      if (jExact >= 0 && (jExact - _cur <= DIRECT_JUMP || !hasPosition())) {
            setCurrent(jExact, QSet<int>({ spExact }));
            return true;
            }
      if (jOct >= 0) {
            if (jOct == _cur) {
                  _matched.insert(spOct);
                  _misses = 0;
                  return false;
                  }
            setCurrent(jOct, QSet<int>({ spOct }));
            return true;
            }
      if (jExact >= 0) {
            _pending = jExact;          // wait for a second note to confirm
            _pendingMatched = QSet<int>({ spExact });
            return false;
            }

      // 4. miss
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
                  return true;
                  }
            }
      return false;
      }

} // namespace Ms
