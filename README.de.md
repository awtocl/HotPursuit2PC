<p align="center">
  <img src="docs/assets/banner.svg" alt="HotPursuit2PC" width="100%">
</p>

<p align="center">
  <a href="https://github.com/awtocl/HotPursuit2PC/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/awtocl/HotPursuit2PC/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <img alt="Plattform: Windows x64" src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-1d3a5f">
  <img alt="Sprache: C++20" src="https://img.shields.io/badge/C%2B%2B-20-1d3a5f">
  <a href="LICENSE"><img alt="Lizenz: MIT" src="https://img.shields.io/badge/license-MIT-ff921c"></a>
  <img alt="Status: Entwicklungsversion" src="https://img.shields.io/badge/status-development%20build-ff921c">
  <a href="#ein-wort-zu-ki"><img alt="Mit KI-Unterstützung" src="https://img.shields.io/badge/AI--assisted-Claude%20Code-6a5acd"></a>
</p>

<p align="center">
  <b>Die PlayStation-2-Version von <i>Need for Speed: Hot Pursuit 2</i>, nativ unter Windows — gebaut aus deiner eigenen Disc.</b>
</p>

<p align="center">
  <a href="https://github.com/awtocl/HotPursuit2PC/releases/latest"><b>Download</b></a> ·
  <a href="docs/GETTING-STARTED.de.md"><b>Erste Schritte</b></a> ·
  <a href="#schnellstart">Schnellstart</a> ·
  <a href="#was-drin-ist">Was drin ist</a> ·
  <a href="#steuerung">Steuerung</a> ·
  <a href="#bekannte-probleme">Bekannte Probleme</a> ·
  <a href="#rechtliches-und-herkunft">Rechtliches</a> ·
  <a href="#ein-wort-zu-ki">Ein Wort zu KI</a>
</p>

<p align="center">
  🌐 <a href="README.md">English</a> · <a href="README.es.md">Español</a> · <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <a href="README.fr.md">Français</a> · <b>Deutsch</b> · <a href="README.it.md">Italiano</a>
</p>

> [!IMPORTANT]
> **Es sind keine Spieldateien enthalten, und es werden keine heruntergeladen.** Du brauchst **deine eigene Disc** von
> *Need for Speed: Hot Pursuit 2* für die PlayStation 2 (Nordamerika, `SLUS-20362`).

> [!NOTE]
> Mit viel Hilfe von KI gebaut (Claude Code). Neugierig oder skeptisch? Hier steht, [was ich darüber denke](#ein-wort-zu-ki).

---

## Warum?

> Die PS2-Version ist die beste Version dieses Spiels – also bringt dieses Projekt genau die auf den PC.

*Hot Pursuit 2* erschien auf mehreren Konsolen, und an die PS2-Version erinnern sich die Leute: das Fahrgefühl, die
Autos, die Strecken. Das hier ist kein Remake und auch nicht die (ziemlich andere) PC-Version von 2003. Es ist genau
dieses PS2-Spiel, als richtiges Windows-Programm: nativer x64-Code, die Auflösung deines Monitors, dein Controller —
und kein Emulator dazwischen.

## Was drin ist

| | |
|---|---|
| **Kein Emulator** | Der Code des Spiels selbst wird in nativen x64-Code übersetzt (mit LLVM), direkt auf deinem PC, aus deiner Kopie. Die Hardware der Konsole wird durch PC-Gegenstücke ersetzt: deine Dateien statt des Laufwerks, deine Grafikkarte, ein Sound-Mixer, dein Controller. |
| **Scharfe Grafik** | Direct3D 11 mit 1- bis 8-facher Originalauflösung (Supersampling, wenn sie über der deines Bildschirms liegt), scharfe Skalierung, Kantenglättung (FXAA), optional Schärfe und Scanlines, randloses Vollbild. |
| **16:9 und 60 fps** | Breitbild ab Werk (du siehst mehr von der Straße; HUD und Menüs behalten ihre Form) und 60 Bilder pro Sekunde, dank der Community-Patches für dieses Spiel (siehe [Credits](#lizenz-und-credits)). Beides lässt sich abschalten. |
| **Der ganze Sound** | Die EA-Trax-Musik, Motoren und Effekte, der Polizeifunk und der Ton der Videos. |
| **Ein Launcher** | Er findet deine Disc, kopiert und prüft sie, bereitet das Spiel vor und merkt sich danach alles. Dazu: Einstellungen mit einer Empfehlung für deinen Bildschirm, Sicherungen der Memory Card und eine Screenshot-Galerie (F12). Mit Maus, Tastatur oder Controller. |
| **Deine Steuerung, wie du willst** | Eine klassische (PS2) und eine moderne (Trigger) Controller-Belegung, und jede Taste und jeder Knopf lässt sich neu belegen. Zurück zum Standard mit einem Klick. |
| **Sechs Sprachen** | Launcher und Spielmenü sprechen Englisch, Spanisch, Portugiesisch, Französisch, Deutsch und Italienisch (beim ersten Start wie dein Windows). |
| **Einstellungen im Spiel** | **F1** (oder Select+Start) öffnet ein Einstellungsmenü im Stil des Spiels, mit *Spiel beenden*. |
| **Deine Dateien bleiben deine** | Deine Disc wird nur gelesen. Alles, was daraus entsteht, bleibt auf deinem PC. |

<p align="center"><img src="docs/assets/launcher-home.png" width="760" alt="Der HotPursuit2PC-Launcher: die Seite Spielen mit dem Status von Disc und Spiel, einem großen SPIELEN-Knopf und den Tasten im Spiel"></p>

## Schnellstart

**Herunterladen und spielen**, ganz ohne Build-Werkzeuge:

1. Hol dir **`HotPursuit2PC-v0.2.0-win64.zip`** aus dem **[neuesten Release](https://github.com/awtocl/HotPursuit2PC/releases/latest)** und entpacke es, wohin du willst.
2. Starte **`HotPursuit2Launcher.exe`**. Die Programme sind nicht signiert, deshalb fragt Windows vielleicht zuerst:
   *Weitere Informationen* → *Trotzdem ausführen*.
3. Leg **deine eigene Disc** ins Laufwerk (oder binde deine `.iso` ein). Der Launcher kopiert und prüft sie und
   übersetzt das Spiel **aus deiner Kopie, auf deinem PC** (etwa eine Minute). Dann auf **Spielen** drücken.

Was sich in jeder Version geändert hat: [CHANGELOG.md](CHANGELOG.md) (auf Englisch).

> [!TIP]
> **Zum ersten Mal so etwas?** Kein Problem: Die Anleitung **[Erste Schritte](docs/GETTING-STARTED.de.md)** führt dich
> Schritt für Schritt durch alles: was du brauchst, die Werkzeuge installieren, bauen, die besten Einstellungen und was
> zu tun ist, wenn etwas schiefgeht.

**Oder selbst bauen:** unter Windows 10/11 x64 mit installierten *Build Tools for Visual Studio 2022* (C++-Workload), Git
und PowerShell 7, und **deiner eigenen Disc** im Laufwerk (oder deiner eingebundenen `.iso`):

```powershell
git clone https://github.com/awtocl/HotPursuit2PC.git
cd HotPursuit2PC
pwsh tools/install-native.ps1       # baut alles und öffnet dann den Launcher
```

Beim ersten Mal wird LLVM gebaut (Zeit für einen Kaffee: 15–40 Minuten), dann das Projekt, und Launcher, Spiel und
Recompiler landen in `game-native\`. Danach übernimmt der Launcher: Er kopiert und prüft deine Disc und übersetzt das
Spiel **aus deiner Kopie, auf deinem PC** (etwa eine Minute). Danach startest du einfach
**`HotPursuit2Launcher.exe`**. Beim ersten Mal etwa eine Stunde; nach einem Update ein paar Minuten.

Der Download enthält nur die Programme dieses Projekts. Das übersetzte Spiel entsteht immer aus deiner eigenen Kopie,
auf deinem eigenen PC – es wird nie verteilt.

## Steuerung

Wähl eine Belegung oder bau dir deine eigene:

| Aktion | Tastatur | Controller: Klassisch (PS2) | Controller: Modern |
|---|---|---|---|
| Gas geben | Z | A | RT (oder A) |
| Bremsen / Rückwärts | A | X | LT |
| Lenken | ← → | Linker Stick / Steuerkreuz | Linker Stick / Steuerkreuz |
| Handbremse | W | RB | B |
| Hupe / Sirene | X | B | Klick linker Stick |
| Ansicht wechseln | S | Y | Y |
| Zurückschauen | Q | LB | Klick rechter Stick |
| Speed-Boost | E | Klick rechter Stick | X |
| Hochschalten / Zone Zoom | 2 | RT | RB |
| Runterschalten / Zone Freeze | 1 | LT | LB |
| Pause | Enter, Leertaste | Start | Start |
| HUD wechseln | Rücktaste, rechte Umschalttaste | Back | Back |

*Klassisch* ist der PS2-Controller Knopf für Knopf; *Modern* legt Gas und Bremse auf die Trigger, wie bei heutigen
Rennspielen. Um etwas zu ändern, öffne im Launcher die Seite **Steuerung**, wähl ein Feld und drück die gewünschte Taste
oder den Knopf (je zwei pro Aktion). **Standard-Tastatur wiederherstellen** und **Standard-Controller wiederherstellen**
holen die Werksbelegungen jederzeit zurück, und deine eigene Belegung bleibt für später gespeichert. Die
Controller-Belegung lässt sich auch im Spiel mit **F1** wechseln.

Auch praktisch: **F1** Einstellungsmenü, **F12** Screenshot, **Alt+Enter** oder **F11** Vollbild, **Alt+F4** oder *Spiel
beenden* im Einstellungsmenü zum Beenden.

## Bekannte Probleme

Es ist eine der ersten Versionen und bisher nur auf einem PC getestet. Darauf kannst du stoßen:

- **Speichern ist ungetestet.** Das Spiel hat eine Memory Card (Dateien in `game-native\saves`), aber Speichern und Laden
  einer Karriere wurde noch nicht von Anfang bis Ende geprüft.
- **Sound:** noch kein Hall und keine echten Lautstärkehüllkurven, daher hören manche Geräusche etwas abrupt auf.
- **Grafik:** noch kein Tiefennebel, keine Mipmaps, kein Dithering und keine Linien; der Entfernungsnebel im Rennen fehlt
  vorerst.
- **Steuerung:** noch keine Vibration und kein Lenkrad. Das Spiel liest Gas und Bremse als An/Aus-Knöpfe, daher
  arbeiten auch die Trigger wie Knöpfe.
- **Sprache:** die Texte des Spiels selbst bleiben Englisch (so kommt diese nordamerikanische Disc); Launcher und
  Einstellungsmenü folgen deiner Sprache.
- Ein Wechsel zwischen 16:9 und 4:3 greift erst nach einem Neustart (das Spiel legt sein HUD einmal an).
- Noch nicht ausprobiert mit AMD- oder Intel-Grafik, auf einem frisch installierten Windows oder in sehr langen Sitzungen.

Etwas anderes gefunden? [Eröffne ein Issue](https://github.com/awtocl/HotPursuit2PC/issues) – das hilft sehr.

## Rechtliches und Herkunft

> [!NOTE]
> Diese Übersetzung dient deiner Bequemlichkeit. Maßgeblich ist die englische Fassung:
> [Legal position and provenance](README.md#legal-position-and-provenance).

Bitte lies diesen Abschnitt, bevor du irgendetwas nutzt oder weitergibst.

**In diesem Repository befindet sich kein Eigentum von Electronic Arts.** Jede Quelldatei wurde für dieses Projekt
geschrieben und steht unter der MIT-Lizenz (siehe [`LICENSE`](LICENSE)). Das Repository enthält keinen Code, keine
Assets, Texte, Audio-, Video- oder sonstigen Inhalte von Electronic Arts Inc., Sony Interactive Entertainment oder anderen
Rechteinhabern des Spiels und keinen Teil der ausführbaren Datei des Spiels in irgendeiner Form (original,
disassembliert, dekompiliert oder übersetzt).

Damit das überprüfbar ist, hier alles im Repository, was aus der Disc *abgeleitet* wurde – ausschließlich Fakten über die
Disc, nichts von ihrem Inhalt:

- [`converter/data/slus20362_rom_manifest.tsv`](converter/data/slus20362_rom_manifest.tsv): Namen, Größen und
  SHA-256-Prüfsummen der 27 Dateien der Disc, um eine gute Kopie von einer beschädigten oder anderen zu unterscheiden.
- [`assets-pipeline/data/hp2_slus20362_names.txt`](assets-pipeline/data/hp2_slus20362_names.txt): die Namen von 362
  Dateien im Archiv des Spiels, wiederhergestellt, damit entpackte Dateien lesbare Namen bekommen.
- [`tests/fixtures/slus20362_zfileset_inventory.tsv`](tests/fixtures/slus20362_zfileset_inventory.tsv): Positionen,
  Größen und SHA-256-Prüfsummen jeder Datei in diesem Archiv, für die Tests.
- [`docs/dump-analysis.md`](docs/dump-analysis.md): eine Beschreibung des Containerformats und das vollständige
  Dateiverzeichnis.
- [`docs/recomp-analysis.md`](docs/recomp-analysis.md) und `recomp/tests/test_recomp_dump.cpp`: Messungen des
  Spielprogramms (Adressen und Größen der Abschnitte, Anzahlen von Befehlen, Funktionen und Sprungtabellen, einige
  Code-Adressen). Keine Befehle und keine Bytes des Programms selbst.
- [`recomp/include/hp2/game_patches.h`](recomp/include/hp2/game_patches.h): Adressen und Werte der 16:9- und
  60-fps-Patches (siehe [Credits](#lizenz-und-credits)).

**Du musst deine eigene Kopie mitbringen.** Um diese Werkzeuge zu nutzen, brauchst du eine Kopie eines Exemplars des
Spiels, das *du* rechtmäßig besitzt, von dir selbst von deiner eigenen Disc erstellt. Dieses Projekt stellt keine
Spieldateien, Disc-Images oder Konsolen-Firmware bereit, verlinkt nicht darauf und hilft nicht, sie zu finden – und wird
es auch nicht tun. Ein PlayStation-2-BIOS wird weder benötigt noch verwendet. Die Gesetze zum Erstellen und Verwenden
von Sicherungskopien eigener Software unterscheiden sich von Land zu Land; du bist dafür verantwortlich, die für dich
geltenden einzuhalten.

**Wie getestet wurde.** Die gesamte Entwicklung und alle Tests erfolgten mit einer einzigen Kopie: der, die die Person,
der dieses Repository gehört, von ihrer eigenen Original-Disc erstellt hat. Zu keinem Zeitpunkt wurde eine andere Kopie des Spiels, ein
heruntergeladenes Image oder Material der Windows-Version verwendet. Die Werkzeuge öffnen die Kopie nur lesend und
verändern sie nie.

**Die Übersetzung passiert auf deinem Rechner.** Alles, was die Werkzeuge aus dem Spiel ableiten – der recompilierte
Code, entpackte Assets – entsteht auf deinem eigenen Computer aus deiner eigenen Kopie und bleibt dort. Gib es nicht
weiter.

**Keine Verbindung.** Dies ist ein unabhängiges, nicht kommerzielles Projekt zur Bewahrung und Interoperabilität. Es ist
nicht mit Electronic Arts, Sony oder den Autoren der Open-Source-Software, auf der es aufbaut, verbunden und wird von
ihnen weder gesponsert noch unterstützt. *Need for Speed* und *Hot Pursuit 2* sind Marken von Electronic Arts Inc.;
*PlayStation* ist eine Marke von Sony Interactive Entertainment Inc. Sie werden hier nur verwendet, um zu sagen, womit die
Werkzeuge kompatibel sind.

Wenn du Rechteinhaber bist und meinst, dass etwas hier nicht hingehört, eröffne bitte ein Issue.

## Ein Wort zu KI

> Ich weiß, dass viele Leute KI hassen, und früher oder später wird alles, was auch nur ein bisschen von einem KI-Modell
> berührt wurde, als „AI slop" abgestempelt. Mein Standpunkt: KI geht nicht wieder weg. Sie ist ein neues Werkzeug, mit
> dem wir Dinge schaffen, die wir vorher nicht für möglich gehalten hätten, und ich lasse mir von keiner Ideologie
> verbieten, sie in meine Arbeit einzubauen. Ob man sie liebt oder hasst: KI kann verdammt guten Code schreiben. Dann
> kann man sie auch nutzen, um sich das Leben ein bisschen leichter zu machen, unterwegs herauszufinden, wofür sie taugt
> – und, in meinem Fall, in Projekte einzutauchen, die die Spiele meiner Kindheit auf den PC bringen.

Ich habe HotPursuit2PC gestartet, um praktisch zu lernen, wie man zusammen mit KI Software baut – an etwas, das schwer
genug ist, um ein echter Test zu sein. So war die Arbeit aufgeteilt:

- **Ein KI-Programmieragent hat Code, Tests, Skripte und Dokumentation geschrieben** – Claude von Anthropic, in Claude
  Code – unter meiner Leitung. Commits tragen eine Zeile `Co-Authored-By: Claude`.
- **Ich habe Ziele und Regeln festgelegt** (sie stehen in [`docs/brief.md`](docs/brief.md)), meine eigene Disc kopiert,
  jeden Schritt genehmigt, der über das Projekt hinausging (etwa das Herunterladen fremden Codes), jede Version getestet
  und gespielt und die Entscheidungen getroffen – zum Beispiel, nach dem Spielen von der Emulator-Version auf statische
  Recompilierung umzusteigen.
- **Was wir über das Spiel wissen, stammt aus meiner eigenen Kopie**, durch Untersuchen und Ausprobieren, mit den
  Belegen in [`docs/dump-analysis.md`](docs/dump-analysis.md). Der Quellcode des Spiels war nie verfügbar und wurde nie
  verwendet.

Und, fair dir gegenüber: KI-geschriebener Code kann sich voller Überzeugung irren. Darum setzt das Projekt auf Vorgehen
statt Vertrauen: Jede Aussage über die Disc ist durch eine Prüfung oder einen Test belegt, Unbewiesenes ist als
**UNVERIFIED** markiert, und jede Entscheidung ist mit ihren Alternativen aufgeschrieben. Niemand hat jede Zeile von Hand
geprüft – schau es dir an, bevor du dich darauf verlässt.

## Für Entwickler

Wie es intern funktioniert, wie man aus dem Quellcode baut, der Aufbau des Repositorys und die ganze technische
Dokumentation stehen im [englischen README](README.md#how-it-works) und in [`CONTRIBUTING.md`](CONTRIBUTING.md). Die
Designentscheidungen liegen in [`docs/adr/`](docs/adr/).

## Fahrplan

- [ ] Verlässliches Speichern (als PC-Dateien)
- [ ] Hall und echte Hüllkurven im Sound-Mixer
- [ ] Tiefennebel und Mipmaps im GPU-Renderer
- [ ] Vibration und Lenkräder
- [ ] Schärfere Texte und Texturen
- [x] Ein fertiger Download, ganz ohne Build-Werkzeuge
- [ ] Flüssigere Bewegung über 60 fps

Es ist ein langer Weg, darum keine Termine: Es ist fertig, wenn es fertig ist.

## Lizenz und Credits

Code und Dokumentation dieses Repositorys stehen unter der [MIT-Lizenz](LICENSE).

- **Spiel-Patches:** Die Werte für 16:9 und 60 fps stammen aus der Community-Patchdatei für diese Disc in
  [PCSX2/pcsx2_patches](https://github.com/PCSX2/pcsx2_patches) (`SLUS-20362_1D2818AF.pnach`): *Widescreen 16:9* von
  **nemesis2000**, *HUD fixed for widescreen* von **Aero_** und *60 FPS* von **asasega**. Verwendet wurden nur Adressen
  und Werte, neu umgesetzt als schaltbare Patch-Punkte. Danke!
- **LLVM** (Apache 2.0 mit LLVM-Ausnahmen): der Codegenerator des Recompilers.
- **Play!** von Jean-Philip Desjardins (BSD 2-Clause): der Emulator-Kern des Referenz-Launchers und seine Bibliotheken
  (Details in [`docs/prior-art.md`](docs/prior-art.md)). Das native Spiel enthält keinen fremden Code; es nutzt Direct3D 11,
  XAudio2 und XInput von Windows.
