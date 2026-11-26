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

#ifndef __SCOREFOLLOWER_H__
#define __SCOREFOLLOWER_H__

#include <vector>
#include <QSet>
#include <QHash>
#include <QElapsedTimer>
#include "libmscore/fraction.h"


namespace Ms {

class Score;

//---------------------------------------------------------
//   ScoreFollower
//    Follows live MIDI input (a musician playing from the
//    score) and keeps track of the position in the score.
//    Only note-on events are used; there is no timing model,
//    the follower just matches pitches against the expected
//    chords, with a small look-ahead window.
//---------------------------------------------------------

class ScoreFollower {
   public:
      struct Event {
            Fraction tick;          // score tick (not unrolled)
            QSet<int> pitches;      // pitches struck at this position
            };

   private:
      struct Attack {
            int pitch;
            qint64 time;
            };

      Score* _score        { nullptr };
      int _undoState       { -1 };      // undo stack state the event list was built for
      bool _repeats        { true };    // repeat expansion the event list was built for
      std::vector<Event> _events;

      int _cur             { -1 };      // index of the current event, -1 = not started
      QSet<int> _matched;               // score pitches of _cur already consumed
      // evidence: for candidate events near _cur, the pitches recently played that
      // belong to them (pitch -> time). Cleared on every move.
      struct Evidence {
            qint64 time;
            bool explained;           // the attack also fitted the current chord
            };
      QHash<int, QHash<int, Evidence>> _evidence;
      // attacks since the last move that did not fit the current chord: a
      // candidate must explain most of them, so a burst of wrong notes that
      // happens to hit a chord ahead does not move the position
      std::vector<Attack> _attacks;
      std::vector<Attack> _recent;      // all recent attacks (for "which chord was just struck")

      int _misses          { 0 };       // attacks without progress (recovery cadence)
      int _stuck           { 0 };       // attacks without progress (for lost(), not reset by recovery attempts)
      std::vector<Attack> _history;     // last played attacks (for recovery)
      QHash<int, qint64> _held;         // pitches currently down -> time pressed
      qint64 _now          { 0 };       // time of the note being processed (ms)
      qint64 _lastMove     { -1000000 };// time of the last forward move (ms)
      QElapsedTimer _clock;

      bool needsRebuild() const;
      bool ensureEvents();
      void rebuild();
      void setCurrent(int idx, const QSet<int>& consumed);
      int matchPitch(int idx, int pitch, const QSet<int>& consumed, bool octave) const;
      int evidenceCount(int idx, bool* unexplained = nullptr, qint64* first = nullptr, qint64* last = nullptr) const;

      int needed(int idx) const;
      int chooseCandidate() const;
      QSet<int> lastStrike() const;
      bool nextVerse();
      bool fitsHistory(int last) const;
      int recover() const;
      int nearestRun(const Fraction& tick, int hint) const;

   public:
      static constexpr int WINDOW         = 3;   // look-ahead in events (bigger skips: recovery, click, next system)
      static constexpr int BACK           = 4;   // look-back in events (recover from a false advance)
      static constexpr int EVIDENCE_MS    = 1500;// played pitches count as evidence this long
      static constexpr int STRIKE_MS      = 150; // a chord further away must be struck together within this time
      static constexpr int RATIO_ATTACKS  = 4;   // last attacks a candidate must mostly explain
      static constexpr int LOST_MISSES    = 6;   // attacks without progress before we call ourselves lost
      static constexpr int VERSE_TAIL     = 2;   // last events from which the first chord means "next verse"

      static constexpr int CHORD_MS       = 60;  // notes within this time after a move belong to the same chord

      static constexpr int HELD_EXPIRE_MS = 20000; // forget a held key after this time (lost note-off)
      static constexpr int MAX_MISSES     = 8;   // misses before a global recovery search
      static constexpr int HISTORY        = 6;   // pitches used for recovery
      static constexpr int RECOVER_SPAN   = 2;   // max spread of recovery candidates

      void setScore(Score* s);
      Score* score() const { return _score; }
      void reset();

      // Positions the follower at (the first event at or after) tick.
      // Returns true when a position was found.
      bool setPosition(const Fraction& tick);

      // Back to the very first chord of the score.
      bool restart();


      ScoreFollower() { _clock.start(); }

      // Feed a note-on. Returns true when the position changed.
      bool noteOn(int pitch)               { return noteOn(pitch, _clock.elapsed()); }
      bool noteOn(int pitch, qint64 ms);
      void noteOff(int pitch)              { _held.remove(pitch); }


      // Moves forward to the first event at or after tick; a repeat jump on
      // the way counts as destination. Never moves backward.

      bool advanceTo(const Fraction& tick);

      bool hasPosition() const { return _cur >= 0 && _cur < int(_events.size()); }
      // several attacks in a row did not fit where we think the player is
      bool lost() const        { return hasPosition() && _stuck >= LOST_MISSES; }
      Fraction tick() const    { return hasPosition() ? _events[_cur].tick : Fraction(-1, 1); }

      // for testing
      void setEvents(const std::vector<Event>& ev);
      int currentIndex() const { return _cur; }
      };

} // namespace Ms
#endif
