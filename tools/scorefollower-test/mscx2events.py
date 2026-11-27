"""Extract the struck chords of a MuseScore 3 .mscx (no repeats) in the
replay-log format of the ScoreFollower test:  EVENTS n / E idx tick p1,p2,...

usage: python -I mscx2events.py score.mscx > events.log
"""
import sys
import xml.etree.ElementTree as ET
from fractions import Fraction

DUR = {'whole': 4, 'half': 2, 'quarter': 1, 'eighth': Fraction(1, 2), '16th': Fraction(1, 4),
       '32nd': Fraction(1, 8), 'breve': 8, 'measure': None}
TPQ = 480


def dur_ticks(el, measure_len):
    t = el.findtext('durationType')
    if t == 'measure':
        return measure_len
    d = Fraction(DUR[t])
    dots = int(el.findtext('dots') or 0)
    add = d
    for _ in range(dots):
        add /= 2
        d += add
    return int(d * TPQ)


def main(path):
    root = ET.parse(path).getroot()
    score = root.find('Score')
    events = {}
    for staff in score.findall('Staff'):
        tick = 0
        sig = Fraction(4, 4)
        for m in staff.findall('Measure'):
            ts = m.find('.//TimeSig')
            if ts is not None:
                sig = Fraction(int(ts.findtext('sigN')), int(ts.findtext('sigD')))
            measure_len = int(sig * 4 * TPQ)
            longest = 0
            for voice in m.findall('voice'):
                t = tick
                for el in voice:
                    if el.tag not in ('Chord', 'Rest'):
                        continue
                    d = dur_ticks(el, measure_len)
                    if el.tag == 'Chord':
                        for n in el.findall('Note'):
                            tie_end = any(sp.get('type') == 'Tie' and sp.find('prev') is not None
                                          for sp in n.findall('Spanner'))
                            if tie_end:
                                continue
                            events.setdefault(t, set()).add(int(n.findtext('pitch')))
                    t += d
                longest = max(longest, t - tick)
            tick += longest or measure_len
    ev = [(t, sorted(p)) for t, p in sorted(events.items()) if p]
    print('EVENTS %d' % len(ev))
    for i, (t, p) in enumerate(ev):
        print('E %d %d %s' % (i, t, ','.join(map(str, p))))


if __name__ == '__main__':
    main(sys.argv[1])
