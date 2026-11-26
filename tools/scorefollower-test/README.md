# ScoreFollower-test (Meelezen)

Losse test van de matchlogica in `mscore/scorefollower.cpp`, zonder MuseScore te starten.
Simuleert een organist: akkoorden, vastgehouden stemmen, pedaal een octaaf lager,
MIDI-echo, foute noten, misgrepen, herhalingen, en drie stresstests
(gewoon spel, vastgehouden stemmen, veel fouten).

Bouwen en draaien (PowerShell):

    cmd /c C:\ms3\follow\tools\scorefollower-test\build.cmd
    $env:PATH = "C:\Qt\5.15.2\msvc2019_64\bin;$env:PATH"; C:\ms3\sftest\sftest.exe

`flags.txt` zijn de compileropties uit `bld\build.ninja` (na een volledige build).
De linkerwaarschuwingen over onopgeloste partituurfuncties zijn verwacht
(`/FORCE:UNRESOLVED`): de test roept die niet aan.

De hele app bouwen: `tools\build-follow.cmd` (kopie van `C:\ms3\build-follow.cmd`).

## Terug naar een eerdere versie

Elke ronde heeft een tag: `meelezen-1` ... `meelezen-4` (zie `git tag`).

    git log --oneline --decorate          # overzicht
    git switch -c proef meelezen-3        # oude versie bekijken/bouwen op een eigen branch
    git switch score-follow               # terug naar de nieuwste
