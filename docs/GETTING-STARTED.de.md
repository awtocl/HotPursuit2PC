# Erste Schritte — spiel *Hot Pursuit 2* nativ auf deinem PC

🌐 [English](GETTING-STARTED.md) · [Español](GETTING-STARTED.es.md) · [Português (Brasil)](GETTING-STARTED.pt-BR.md) · [Français](GETTING-STARTED.fr.md) · **Deutsch** · [Italiano](GETTING-STARTED.it.md)

Willkommen! Diese Anleitung bringt dich Schritt für Schritt von deiner PS2-Disc zum Rennen auf deinem PC. Du tippst ein
paar Befehle ein, der Rest passiert von selbst. Rechne mit **etwa einer Stunde beim ersten Mal** — vor allem Warten,
während dein PC baut, also hol dir ruhig einen Snack. Danach startest du das Spiel mit einem Doppelklick.

> [!IMPORTANT]
> Du brauchst **deine eigene Original-Disc** von *Need for Speed: Hot Pursuit 2* für die PlayStation 2. Das Projekt
> enthält keine Spieldateien und lädt keine herunter.

> [!WARNING]
> **Speichern ist noch nicht getestet.** Das Spiel schreibt auf eine Memory Card, die als Dateien im Spielordner liegt,
> aber Speichern und Laden einer Karriere wurde noch nicht von Anfang bis Ende geprüft. Verlass dich noch nicht darauf,
> deinen Fortschritt zu behalten. Einzelne Rennen laufen von Anfang bis Ende.

---

## Der schnelle Weg: herunterladen

Seit Version 0.2.0 gibt es einen fertigen Download, du kannst also die Installation der Werkzeuge und das Bauen
überspringen (Schritte 1, 3 und 4):

1. Lade **`HotPursuit2PC-v0.2.0-win64.zip`** aus dem [neuesten Release](https://github.com/awtocl/HotPursuit2PC/releases/latest) herunter.
2. Rechtsklick darauf → **Alle extrahieren…** und such dir einen Ordner aus (zum Beispiel `C:\Spiele\HotPursuit2PC`).
3. Öffne den Ordner und doppelklicke auf **`HotPursuit2Launcher.exe`**. Windows meldet vielleicht, dass es deinen PC
   geschützt hat (die Programme sind nicht signiert): Klick auf **Weitere Informationen** und dann auf **Trotzdem
   ausführen**.
4. Mach dann mit **Schritt 2** (deine Disc) und **Schritt 5** (die Einrichtung im Launcher) weiter unten weiter.

Du brauchst etwa 4 GB freien Platz, das meiste für die Kopie deiner Disc. Lieber selbst bauen oder die allerneuesten
Änderungen? Dann folge allen Schritten unten.

## Was du brauchst

| Du brauchst | Details |
|---|---|
| **Die Disc** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **nordamerikanische Ausgabe**. Der Code **SLUS-20362** steht auf der Disc und auf der Rückseite der Hülle. Andere Regionen (zum Beispiel die europäische `SLES-50731`) und Ausgaben mit einem anderen Programm werden nicht unterstützt — der Launcher prüft das für dich. |
| **Etwas zum Lesen** | Ein DVD-Laufwerk im PC (oder angeschlossen). Hast du schon ein `.iso`-Abbild deiner eigenen Disc gemacht, geht auch das. |
| **Der PC** | Windows 10 oder 11, 64 Bit. Eine Grafikkarte mit Direct3D 11 (jede aus den letzten zehn Jahren). |
| **Freier Speicher** | Etwa **15 GB**: 3 GB für die Kopie deiner Disc, 6 GB für die C++-Build-Werkzeuge (Schritt 1) und 6 GB für das Projekt mit seinem Codegenerator (Schritt 4). |
| **Ein Controller** *(empfohlen)* | Ein Xbox-Controller oder alles, was Windows als solchen erkennt. Die Tastatur geht auch. |
| **Internet** | Für Schritt 1 und für den ersten Lauf des Installers (er lädt etwa 1–2 GB Werkzeuge und Quellcode herunter). |

## Schritt 1 — Werkzeuge installieren (einmalig)

Das Spiel wird auf deinem PC gebaut, dafür braucht es drei kostenlose Werkzeuge von Microsoft und Git. Am einfachsten
geht das mit **winget**, das bei Windows 10 und 11 dabei ist.

1. Öffne das **Start**-Menü, tippe `Terminal`, klick mit der rechten Maustaste auf **Terminal** und wähle **Als
   Administrator ausführen**.
2. Kopiere diese drei Zeilen nacheinander hinein und drück nach jeder Enter. Jede Zeile dauert ein paar Minuten; warte,
   bis sie fertig ist, bevor du die nächste startest.

   ```powershell
   winget install --id Microsoft.PowerShell -e
   winget install --id Git.Git -e
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   Die dritte Zeile installiert die *Build Tools for Visual Studio 2022* mit der Workload *Desktopentwicklung mit C++* —
   den C++-Compiler, den das Projekt braucht (etwa 6 GB). Ein Fenster des Visual-Studio-Installers zeigt den Fortschritt.
3. **Starte den PC neu**, wenn alle drei fertig sind.

<details>
<summary>Lieber von Hand installieren?</summary>

- **PowerShell 7:** aus dem [Microsoft Store](https://apps.microsoft.com/detail/9mz1snwt0n5d) oder von Microsofts
  [PowerShell-Seite](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows).
- **Git für Windows:** von [git-scm.com](https://git-scm.com/download/win); die Standardoptionen passen.
- **Build Tools for Visual Studio 2022:** von [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
  (unter *Tools für Visual Studio*). Im Installer **Desktopentwicklung mit C++** anhaken und auf *Installieren* klicken.
  Hast du Visual Studio 2022 mit dieser Workload schon, musst du nichts installieren.

</details>

## Schritt 2 — Disc bereithalten

Der Launcher kopiert deine Disc in Schritt 5 auf den PC, danach muss sie nicht im Laufwerk bleiben. Mach vor Schritt 5
eins davon:

- **Deine Disc:** leg sie ins DVD-Laufwerk.
- **Ein `.iso`-Abbild deiner eigenen Disc:** doppelklicke die `.iso`-Datei im Explorer. Windows zeigt sie als neues
  DVD-Laufwerk an (nach Schritt 5 kannst du sie auswerfen).
- **Du hast die Dateien der Disc schon in einen Ordner kopiert:** nichts zu tun; du kannst diesen Ordner in Schritt 5
  auswählen. Er muss `SYSTEM.CNF`, `SLUS_203.62` und den Ordner `NFSHP2` enthalten.

## Schritt 3 — HotPursuit2PC herunterladen (einmalig)

1. Öffne das **Start**-Menü, tippe `PowerShell 7` und öffne es (nicht das ältere „Windows PowerShell").
2. Kopiere diese Zeilen hinein und drück nach jeder Enter:

   ```powershell
   cd $HOME\Documents
   git clone https://github.com/awtocl/HotPursuit2PC.git
   cd HotPursuit2PC
   ```

Das Projekt liegt jetzt in `Dokumente\HotPursuit2PC`. (Bitte `git clone` statt *Download ZIP* verwenden: Windows
blockiert Skripte aus einer heruntergeladenen ZIP-Datei, und mit `git` sind Updates außerdem einfach.)

## Schritt 4 — Das Spiel bauen

Tippe im selben PowerShell-7-Fenster:

```powershell
pwsh tools/install-native.ps1
```

Das Skript arbeitet fünf Schritte ab und sagt dir, wo es gerade ist (seine Meldungen sind auf Englisch):

| Schritt | Was passiert | Dauer |
|---|---|---|
| 1/5 Your disc | Noch nichts: Der Launcher fragt in Schritt 5 nach deiner Disc | — |
| 2/5 LLVM | **Nur beim ersten Mal:** lädt und baut LLVM, den Codegenerator des Recompilers | **15–40 Min.** |
| 3/5 Building | Baut HotPursuit2PC: Launcher, Spiel und Recompiler | 3–10 Min. |
| 4/5 Preparing | Noch nichts: Das macht der Launcher, nachdem er deine Disc geprüft hat | — |
| 5/5 Game folder | Legt die drei Programme in `game-native\` | Sekunden |

Dein PC kann in den Schritten 2 und 3 laut und ausgelastet sein; das ist normal. Endet alles mit **„Done …"** in Grün,
öffnet sich der Launcher von selbst.

## Schritt 5 — Das Spiel im Launcher einrichten (einmalig)

Beim ersten Mal führt dich der Launcher durch vier kurze Schritte:

1. **Disc** — dein DVD-Laufwerk (oder die eingebundene `.iso`) erscheint von selbst; wähl **Kopieren von …**, um sie in
   den Spielordner zu kopieren (etwa 3 GB, ein paar Minuten). Oder wähl **einen Ordner mit den Dateien der Disc**. Ist
   die Disc aus einer anderen Region oder Ausgabe, sagt dir der Launcher das hier.
2. **Prüfen** — jede Datei wird mit der Original-Disc verglichen.
3. **Vorbereiten** — das Spielprogramm wird in Code für deinen Prozessor übersetzt (etwa eine Minute).
4. **Fertig** — die Bildqualität ist schon passend für deinen Bildschirm eingestellt. Wähl **Jetzt spielen**.

![Der Startbildschirm des Launchers](assets/launcher-home.png)

## Schritt 6 — Spielen

Ab jetzt öffnest du **`HotPursuit2Launcher.exe`** in `Dokumente\HotPursuit2PC\game-native` und wählst **Spielen**. Für
eine Desktop-Verknüpfung: Launcher → **Disc & Einrichtung** → **Desktop-Verknüpfung**.

Der Launcher geht mit Maus, Tastatur (Pfeiltasten, Enter, Esc, Bild auf/Bild ab) oder Xbox-Controller (A auswählen, B
zurück, LB/RB Seiten, Start spielt). Seine Seiten:

| Seite | Wofür |
|---|---|
| **Spielen** | Das Spiel starten; sehen, ob Disc und Spiel in Ordnung sind; deine Spielzeit |
| **Einstellungen** | Bild und Anzeige, jede Einstellung erklärt, mit der Empfehlung für deinen Bildschirm |
| **Steuerung** | Klassische oder moderne Controller-Belegung, jede Taste und jeden Knopf neu belegen, Standard wiederherstellen |
| **Memory Card** | Was das Spiel gespeichert hat; sichern und eine Sicherung wiederherstellen |
| **Screenshots** | Die Bilder, die du im Spiel mit **F12** gemacht hast |
| **Disc & Einrichtung** | Alle Dateien der Disc prüfen, das Spiel neu vorbereiten, eine andere Disc-Kopie nehmen, eine Desktop-Verknüpfung anlegen |
| **Info** | Was dieses Projekt ist, Credits, Links zu dieser Anleitung und zu den Issues |

Das Spiel startet im Vollbild mit den Intro-Videos; drück **Enter** (oder Start auf dem Controller), um sie zu
überspringen. Der Launcher versteckt sich, während du spielst, und kommt zurück, wenn du beendest.

### Empfohlene Einstellungen

Ändere sie auf der Seite **Einstellungen** im Launcher oder beim Spielen mit **F1** (oder **Select + Start** auf dem
Controller). Die Einrichtung hat die **Bildqualität** (die interne Auflösung) schon für deinen Bildschirm gewählt; sie
bestimmt, wie scharf das Bild ist:

| Dein Bildschirm | Bildqualität |
|---|---|
| 1920 × 1080 (Full HD) | **Qualität (4x)** |
| 2560 × 1440 | **Ultra (6x)** |
| 3840 × 2160 (4K) | **Maximum (8x)** |

Höher ist ruhiger und schärfer (es werden mehr Pixel berechnet, als dein Bildschirm hat, und gemittelt). Ruckelt das
Spiel, nimm den nächstniedrigeren Wert.

| Einstellung | Empfohlen | Warum |
|---|---|---|
| Renderer | **GPU** | Der Software-Renderer ist langsamer und nur zum Vergleich da |
| Bildrate | **60** | Doppelt so viele Bilder wie die 30 der PS2 im Rennen; das Spiel behält seine normale Geschwindigkeit |
| Anzeigemodus | **Vollbild** | Alt+Enter oder F11 schaltet jederzeit um |
| V-Sync | **An** | Ruhigere Bewegung, kein Tearing |
| Skalierungsfilter | **Scharf bilinear** | Klar ohne Treppenkanten |
| Seitenverhältnis | **16:9** | Mehr Sicht im Rennen; die Menüs behalten ihre Form (Wechsel zwischen 16:9 und 4:3 braucht einen Neustart) |
| Schärfe | **Aus** oder **Niedrig** | Höhere Werte können Kanten flimmern lassen |
| Scanlines | **Aus** | Außer du magst den Röhrenfernseher-Look |
| Kantenglättung | **FXAA** | Glättet treppenartige Kanten; bei den höchsten Bildqualitäten ist der Unterschied gering |

Die Einstellungen werden automatisch in `game-native\hp2native.ini` gespeichert.

### Steuerung

Wähl eine Belegung auf der Seite **Steuerung** im Launcher (oder im Spiel mit **F1** → *Controller-Belegung*):

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
| Einstellungsmenü | F1 | Select + Start | Select + Start |
| Screenshot | F12 | – | – |

- **Klassisch** ist der PS2-Controller Knopf für Knopf. **Modern** legt Gas und Bremse auf die Trigger, wie bei heutigen
  Rennspielen.
- **Bau dir deine eigene:** Wähl auf der Seite **Steuerung** ein Feld und drück die gewünschte Taste oder den Knopf.
  Jede Aktion nimmt zwei Tasten und zwei Controller-Knöpfe. Esc bricht ab, Entf löscht eine Taste. Deine erste Änderung
  legt eine *eigene* Belegung an, ausgehend von der, die du gerade benutzt hast.
- **Doch anders überlegt?** **Standard-Tastatur wiederherstellen** und **Standard-Controller wiederherstellen** gehen
  direkt zurück zu den Werksbelegungen. Deine eigene Belegung bleibt gespeichert; wähl *Eigene*, um sie wieder zu nutzen.
- Die Analogsticks arbeiten wie die des DualShock 2. PlayStation-Controller erkennt Windows nicht von selbst als
  Spiel-Controller; sie funktionieren über ein Werkzeug, das sie als Xbox-Controller ausgibt, etwa die
  Controller-Unterstützung von Steam.

### Sprache

Launcher und Einstellungsmenü im Spiel gibt es auf Englisch, Spanisch, Portugiesisch (Brasilien), Französisch, Deutsch
und Italienisch. Beim ersten Start folgen sie deiner Windows-Anzeigesprache; ändern kannst du sie im Launcher unter
**Einstellungen → Sprache** oder im Spiel mit **F1**. Die Texte des Spiels selbst bleiben Englisch: So kommt die
nordamerikanische Disc.

### Beenden

**F1 → Spiel beenden** (zum Bestätigen zweimal drücken) oder **Alt+F4**.

## Aktualisieren

Wenn das Projekt Neuerungen bekommt, aktualisieren und neu bauen — Einstellungen, Spielstände und die Disc-Kopie
bleiben:

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1
```

LLVM wird nicht neu gebaut, das dauert also nur ein paar Minuten. Danach zeigt der Launcher **Vorbereiten & spielen**:
Das Spiel wird für die neue Version neu vorbereitet (etwa eine Minute) und startet.

## Wenn etwas schiefgeht

| Was du siehst | Was du tun kannst |
|---|---|
| *„The C++ build tools were not found"* | Schritt 1 ist nicht fertig: Installiere die Build Tools mit **Desktopentwicklung mit C++**, starte den PC neu und versuch es noch einmal. |
| *„pwsh wird nicht erkannt"* | PowerShell 7 ist nicht installiert, oder der PC wurde nach Schritt 1 nicht neu gestartet. |
| Der Launcher findet deine Disc nicht | Warte nach dem Einlegen ein paar Sekunden; eine `.iso` zuerst doppelklicken. Oder kopiere die Dateien der Disc selbst in einen Ordner und wähl **einen Ordner mit den Dateien der Disc**. |
| *„Dieser Ordner enthält keine PlayStation-2-Disc"* oder *„… fehlt"* | Falscher Ordner oder unvollständige Kopie. Wähl den Ordner, der direkt `SYSTEM.CNF`, `SLUS_203.62` und `NFSHP2` enthält. |
| *„Diese Disc ist SLES-…"* oder *„… ist nicht die unterstützte Version"* | Deine Disc ist aus einer anderen Region oder Ausgabe. Nur die nordamerikanische Ausgabe `SLUS-20362` wird unterstützt. |
| *„… weicht von der Original-Disc ab"* | Die Disc-Kopie ist beschädigt (oft eine zerkratzte Disc). Reinige die Disc und kopiere sie erneut (**Disc & Einrichtung** → **Disc wechseln**). |
| *„Die Vorbereitung des Spiels ist fehlgeschlagen"* | Die Details stehen in `game-native\prepare.log`; häng die Datei an ein [Issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *„Das Spiel wurde beendet: …"* auf der Seite Spielen | Wähl **Log öffnen** und **Problem melden**; häng `native.log` an. |
| *„Downloading LLVM failed"* | Prüf deine Internetverbindung und starte den Installer noch einmal. |
| *„The build failed"* | Kopiere die roten Zeilen und [eröffne ein Issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *„…kann nicht geladen werden, da die Ausführung von Skripts deaktiviert ist"* | Du hast ein ZIP heruntergeladen statt `git clone` zu benutzen. Nimm `git clone` (Schritt 3). |
| Das Spielfenster ist schwarz oder das Bild stimmt nicht | Launcher → **Einstellungen** → Renderer → **Software**. Hilft das, melde es bitte mit deinem Grafikkartenmodell. |
| Kein Ton | Das Spiel nutzt das Standard-Wiedergabegerät von Windows: Prüf es in den Windows-Soundeinstellungen und starte das Spiel neu. |
| Der Controller tut nichts | Nur Xbox-kompatible (XInput) Controller werden erkannt; siehe [Steuerung](#steuerung). Die Tastatur geht immer. |
| Ein Knopf macht das Falsche | Launcher → **Steuerung** → **Standard-Controller wiederherstellen** (oder **Standard-Tastatur wiederherstellen**). |
| Es ruckelt | Senke die **Bildqualität** (Launcher → Einstellungen) und achte darauf, dass V-Sync an ist. |
| Nach dem Ändern von Einstellungen sieht alles falsch aus | Launcher → **Einstellungen** → **Zurücksetzen**. |

**Ein Problem melden:** Nach dem Schließen des Spiels beschreibt die Datei `game-native\native.log`, was passiert ist.
Häng sie mit deiner Windows-Version und deiner Grafikkarte an ein [Issue](https://github.com/awtocl/HotPursuit2PC/issues).
Das Log enthält keine persönlichen Daten außer Ordnerpfaden, die du entfernen kannst.

## Deinstallieren

Dieses Projekt installiert nichts in Windows. Zum Entfernen löschst du den Ordner `Dokumente\HotPursuit2PC` (die
Build-Werkzeuge liegen darin in `.cache`, die Kopie deiner Disc in `game-native\disc`). Sichere vorher
`game-native\saves`, wenn du deine Spielstände behalten willst. Eine vom Launcher angelegte Verknüpfung löschst du wie
jede andere. Die Werkzeuge aus Schritt 1 entfernst du unter *Einstellungen → Apps* wie jedes andere Programm.

---

Hängst du fest, oder ist hier etwas unklar? [Eröffne ein Issue](https://github.com/awtocl/HotPursuit2PC/issues) und sag
uns Bescheid — diese Anleitung soll bei allen funktionieren. Viel Spaß auf der Straße, und pass auf die Polizei auf!
