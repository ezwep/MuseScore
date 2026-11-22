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
      Score* _score        { nullptr };
      int _undoState       { -1 };      // undo stack state the event list was built for
      bool _repeats        { true };    // repeat expansion the event list was built for
      std::vector<Event> _events;

      int _cur             { -1 };      // index of the current event, -1 = not started
      QSet<int> _matched;               // score pitches of _cur already consumed
      int _pending         { -1 };      // candidate for a larger jump, needs confirmation
      QSet<int> _pendingMatched;        // score pitches of _pending consumed so far
      int _misses          { 0 };       // consecutive unmatched notes
      std::vector<int> _history;        // last played pitches (for recovery)

      bool needsRebuild() const;
      bool ensureEvents();
      void rebuild();
      void setCurrent(int idx, const QSet<int>& consumed);
      int matchPitch(int idx, int pitch, const QSet<int>& consumed, bool octave) const;
      int findForward(int from, int to, int pitch, bool octave, int* scorePitch) const;
      bool fitsHistory(int last) const;
      int recover() const;
      int nearestRun(const Fraction& tick, int hint) const;

   public:
      static constexpr int WINDOW         = 8;   // look-ahead in events
      static constexpr int DIRECT_JUMP    = 2;   // jumps up to this size are accepted at once
      static constexpr int MAX_MISSES     = 6;   // misses before a global recovery search
      static constexpr int HISTORY        = 4;   // pitches used for recovery
      static constexpr int RECOVER_SPAN   = 2;   // max spread of recovery candidates

      void setScore(Score* s);
      Score* score() const { return _score; }
      void reset();

      // Positions the follower at (the first event at or after) tick.
      // Returns true when a position was found.
      bool setPosition(const Fraction& tick);

      // Feed a note-on. Returns true when the position changed.
      bool noteOn(int pitch);

      // Moves forward to the first event at or after tick; a repeat jump on
      // the way counts as destination. Never moves backward.

      bool advanceTo(const Fraction& tick);

      bool hasPosition() const { return _cur >= 0 && _cur < int(_events.size()); }
      Fraction tick() const    { return hasPosition() ? _events[_cur].tick : Fraction(-1, 1); }

      // for testing
      void setEvents(const std::vector<Event>& ev);
      int currentIndex() const { return _cur; }
      };

} // namespace Ms
#endif
