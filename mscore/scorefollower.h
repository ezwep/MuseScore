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
      // A jump other than to the next chord must be confirmed by a second,
      // separate strike: one deviating chord is not enough to jump.
      int _farCand         { -1 };      // event the previous strike pointed to (confirmed by a strike on the next one)
      qint64 _farCandTime  { 0 };       // when that strike began

      int _stuck           { 0 };       // attacks without progress (for lost())
      QHash<int, qint64> _held;         // pitches currently down -> time pressed
      qint64 _now          { 0 };       // time of the note being processed (ms)
      qint64 _lastMove     { -1000000 };// time of the last forward move (ms)
      QElapsedTimer _clock;

      // diagnostic log (what came in, what was decided and why)
      QFile* _log          { nullptr };
      int _logLines        { 0 };
      QString _why;

      bool needsRebuild() const;
      bool ensureEvents();
      void rebuild();
      void setCurrent(int idx, const QSet<int>& consumed);
      int matchPitch(int idx, int pitch, const QSet<int>& consumed, bool octave) const;
      int evidenceCount(int idx, bool* unexplained = nullptr, qint64* first = nullptr, qint64* last = nullptr) const;
      int needed(int idx) const;
      bool farPending() const;
      bool struckBefore(int idx, int need, qint64 before) const;
      int chooseCandidate();
      QSet<int> lastStrike() const;
      bool nextVerse();
      int nearestRun(const Fraction& tick, int hint) const;
      bool noteOnImpl(int pitch, qint64 ms);
      void logLine(const QString& line);
      void logEvents();

   public:
      static constexpr int WINDOW         = 3;   // look-ahead in events (bigger skips: click, Reset, Next System)
      static constexpr int BACK           = 4;   // look-back in events (recover from a false advance)
      static constexpr int EVIDENCE_MS    = 1500;// played pitches count as evidence this long
      static constexpr int STRIKE_MS      = 150; // a chord further away must be struck together within this time
      static constexpr int RATIO_ATTACKS  = 4;   // last attacks a candidate must mostly explain
      static constexpr int LOST_MISSES    = 6;   // attacks without progress before we call ourselves lost
      static constexpr int VERSE_TAIL     = 2;   // last events from which the first chord means "next verse"
      static constexpr int VERSE_PAUSE_MS = 1200;// silence before the opening chord that marks a new verse
      static constexpr int CHORD_MS       = 60;  // notes within this time after a move belong to the same chord
      static constexpr qint64 LOG_MAX_BYTES = 5 * 1024 * 1024;
      static constexpr int HELD_EXPIRE_MS = 20000; // forget a held key after this time (lost note-off)

      ScoreFollower() { _clock.start(); }
      ~ScoreFollower();

      void setScore(Score* s);
      Score* score() const { return _score; }
      void reset();

      // Positions the follower at (the first event at or after) tick.
      // Returns true when a position was found.
      bool setPosition(const Fraction& tick);

      // Back to the very first chord of the score.
      bool restart();

      // Feed a note-on. Returns true when the position changed.
      bool noteOn(int pitch)               { return noteOn(pitch, _clock.elapsed()); }
      bool noteOn(int pitch, qint64 ms);
      void noteOff(int pitch)              { noteOff(pitch, _clock.elapsed()); }
      void noteOff(int pitch, qint64 ms);

      // Moves forward to the first event at or after tick; a repeat jump on
      // the way counts as destination. Never moves backward.
      bool advanceTo(const Fraction& tick);

      bool hasPosition() const { return _cur >= 0 && _cur < int(_events.size()); }
      // several attacks in a row did not fit where we think the player is
      bool lost() const        { return hasPosition() && _stuck >= LOST_MISSES; }
      Fraction tick() const    { return hasPosition() ? _events[_cur].tick : Fraction(-1, 1); }

      // Write a diagnostic log (empty path: off). Every rebuild writes the
      // chord list, every note what was decided; the test tool can replay it.
      void setLogFile(const QString& path);

      // for testing
      void setEvents(const std::vector<Event>& ev);
      int currentIndex() const { return _cur; }
      const QString& lastReason() const { return _why; }
      };

} // namespace Ms
#endif
