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
#include <QString>
#include "libmscore/fraction.h"

class QFile;

namespace Ms {

class Score;

//---------------------------------------------------------
//   ScoreFollower
//    Follows a musician playing live (MIDI note-ons) from the
//    score and keeps track of the position, so the view can
//    scroll along.
//
//    The score is a list of events: the pitches struck at one
//    moment (repeats unrolled). Matching is an on-line dynamic
//    programming alignment (after Dannenberg's score follower):
//    for every event near the position a score is kept of how
//    well everything played so far fits "the player is here".
//    Each note updates all of them at once:
//      - a note of the event (not yet played there): + MATCH
//      - one octave from one (pedal 16'), current only: + OCTAVE
//      - anything else (wrong, repeated, extra):      + EXTRA (< 0)
//      - entering the next event costs ENTER, every event
//        skipped on the way costs (per note, up to a whole chord).
//    Wrong notes cost every position the same and move nothing;
//    a skipped or optional note costs one SKIP; held notes cost
//    nothing; a repeated chord is entered by its new attack. If
//    the shown position was wrong, the right one keeps gaining
//    and takes over again (hysteresis against flicker).
//---------------------------------------------------------

class ScoreFollower {
   public:
      struct Event {
            Fraction tick;          // score tick (not unrolled)
            QSet<int> pitches;      // pitches struck at this position
            bool optional { false };// only notes in parentheses: may be skipped for free
            };

      // alignment weights (tuned with tools/scorefollower-test: a real recording of
      // Psalm 25 and the stress tests; "inline" so the test tool can vary them)
      static inline double MATCH       =  1.0;
      static inline double OCTAVE      =  0.6;
      static inline double EXTRA       = -0.5;
      static inline double ENTER       = -0.1;
      static inline double SKIP_PER_NOTE = 1.0;  // passing an event unplayed costs this per note ...
      static inline double SKIP_SINGLE =  0.5;   // ... a single (passing) note: this ...
      static inline double SKIP_MAX    =  3.5;   // ... at most this (a whole chord)
      static inline double FWD_MARGIN  =  0.4;  // a position ahead must lead by this ...
      static inline double FWD_PER_NOTE = 0.4;   // ... plus this per note of its chord after the first (max 4) ...
      // (... plus the skip costs of what lies between, see forwardMargin())
      static inline double BACK_MARGIN =  2.0;   // a position behind must lead by this to be shown
      static constexpr int MAX_SKIP       = 3;      // events that may be skipped in one step
      static constexpr int AHEAD          = 8;      // events kept alive ahead of the shown position
      static constexpr int BEHIND         = 6;      // ... and behind it

      static constexpr int LOST_MISSES    = 6;      // notes in a row that fit nowhere: lost
      static constexpr int STRIKE_MS      = 150;    // notes within this time form one strike
      static constexpr int VERSE_TAIL     = 2;      // last events from which the first chord means "next verse"
      static constexpr int VERSE_PAUSE_MS = 1200;   // silence before the opening chord that marks a new verse
      static constexpr int HELD_EXPIRE_MS = 20000;  // forget a held key after this time (lost note-off)
      static constexpr qint64 LOG_MAX_BYTES = 5 * 1024 * 1024;

   private:
      struct Attack {
            int pitch;
            qint64 time;
            };

      Score* _score        { nullptr };
      int _undoState       { -1 };      // undo stack state the event list was built for
      bool _repeats        { true };    // repeat expansion the event list was built for
      std::vector<Event> _events;

      // alignment state, per event (only [_lo, _hi] is alive)
      std::vector<double> _v;           // score of "the player is at this event"
      std::vector<QSet<int>> _used;     // pitches of the event already played on that path
      int _lo              { 0 };
      int _hi              { -1 };
      int _cur             { -1 };      // shown position, -1 = not started

      int _stuck           { 0 };       // notes in a row without a match on the best path
      QHash<int, qint64> _held;         // keys down -> time pressed
      std::vector<int> _mistakes;       // wrong notes per event (practice overview)
      std::vector<Attack> _recent;      // recent attacks (strikes, verse pause)
      qint64 _now          { 0 };
      QElapsedTimer _clock;

      // diagnostic log
      QFile* _log          { nullptr };
      int _logLines        { 0 };
      QString _why;

      bool needsRebuild() const;
      bool ensureEvents();
      void rebuild();
      void startAt(int idx);
      double reward(int idx, int pitch, const QSet<int>& used, int* scorePitch, bool octave) const;
      double forwardMargin(int j) const;
      double skipCost(int idx) const;
      QSet<int> lastStrike() const;
      bool nextVerse();
      int nearestRun(const Fraction& tick, int hint) const;
      bool noteOnImpl(int pitch, qint64 ms);
      void logLine(const QString& line);
      void logEvents();

   public:
      ScoreFollower() { _clock.start(); }
      ~ScoreFollower();

      void setScore(Score* s);
      Score* score() const { return _score; }
      void reset();

      // Positions the follower at (the first event at or after) tick.
      bool setPosition(const Fraction& tick);
      // Back to the very first chord of the score.
      bool restart();
      // Moves forward to the first event at or after tick; a repeat jump on
      // the way counts as destination. Never moves backward.
      bool advanceTo(const Fraction& tick);

      // Feed a note-on / note-off. noteOn returns true when the shown position changed.
      bool noteOn(int pitch)               { return noteOn(pitch, _clock.elapsed()); }
      bool noteOn(int pitch, qint64 ms);
      void noteOff(int pitch)              { noteOff(pitch, _clock.elapsed()); }
      void noteOff(int pitch, qint64 ms);

      bool hasPosition() const { return _cur >= 0 && _cur < int(_events.size()); }
      // the chord at the shown position, and its pitches played so far
      QSet<int> currentPitches() const { return hasPosition() ? _events[_cur].pitches : QSet<int>(); }
      QSet<int> currentPlayed() const;
      // does a played pitch belong where the player is (this chord, the next
      // one already, or an octave of this chord)?
      bool fits(int pitch) const;
      // wrong notes per event since the last restart / clearMistakes()
      const std::vector<int>& mistakes() const { return _mistakes; }
      const std::vector<Event>& events() const { return _events; }
      void clearMistakes()     { _mistakes.assign(_events.size(), 0); }
      // several notes in a row fitted nowhere near the position
      bool lost() const        { return hasPosition() && _stuck >= LOST_MISSES; }
      Fraction tick() const    { return hasPosition() ? _events[_cur].tick : Fraction(-1, 1); }

      // Diagnostic log (empty path: off): the chord list on every rebuild and
      // every note with the decision; the test tool can replay it.
      void setLogFile(const QString& path);

      // for testing
      void setEvents(const std::vector<Event>& ev);
      int currentIndex() const { return _cur; }
      const QString& lastReason() const { return _why; }
      QString debugState() const;
      };

} // namespace Ms
#endif
