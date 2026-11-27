# Per iniziare — gioca a *Hot Pursuit 2* in modo nativo sul tuo PC

🌐 [English](GETTING-STARTED.md) · [Español](GETTING-STARTED.es.md) · [Português (Brasil)](GETTING-STARTED.pt-BR.md) · [Français](GETTING-STARTED.fr.md) · [Deutsch](GETTING-STARTED.de.md) · **Italiano**

Ciao! Questa guida ti porta dal tuo disco PS2 alle gare sul tuo PC, un passo alla volta. Scriverai pochi comandi e il
resto si fa da solo. Calcola **circa un'ora la prima volta** — quasi tutta attesa mentre il PC compila, quindi prenditi
pure uno spuntino. Poi, avviare il gioco è un doppio clic.

> [!IMPORTANT]
> Ti serve **il tuo disco originale** di *Need for Speed: Hot Pursuit 2* per PlayStation 2. Il progetto non include file
> del gioco e non ne scarica nessuno.

> [!WARNING]
> **Il salvataggio non è ancora testato.** Il gioco scrive su una memory card tenuta come file nella cartella del gioco,
> ma salvare e caricare una carriera non è stato verificato dall'inizio alla fine. Per ora non contare di conservare i
> progressi. Le gare singole si giocano dall'inizio alla fine.

---

## Cosa ti serve

| Ti serve | Dettagli |
|---|---|
| **Il disco** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **edizione nordamericana**. Il codice **SLUS-20362** è stampato sul disco e sul retro della custodia. Altre regioni (per esempio quella europea `SLES-50731`) e qualsiasi edizione con un altro programma non sono supportate — il launcher lo controlla per te. |
| **Un modo per leggerlo** | Un lettore DVD nel PC (o collegato). Se hai già fatto un'immagine `.iso` del tuo disco, va bene anche quella. |
| **Il PC** | Windows 10 o 11 a 64 bit. Una scheda video con Direct3D 11 (qualsiasi degli ultimi dieci anni). |
| **Spazio libero** | Circa **15 GB**: 3 GB per la copia del disco, 6 GB per gli strumenti di compilazione C++ (passo 1) e 6 GB per il progetto con il suo generatore di codice (passo 4). |
| **Un controller** *(consigliato)* | Un controller Xbox, o qualsiasi cosa che Windows veda come tale. Va bene anche la tastiera. |
| **Internet** | Per il passo 1 e per il primo avvio dell'installer (scarica circa 1–2 GB di strumenti e codice sorgente). |

## Passo 1 — Installa gli strumenti (una volta)

Il gioco viene compilato sul tuo PC, quindi servono tre strumenti gratuiti di Microsoft e Git. Il modo più semplice è
**winget**, incluso in Windows 10 e 11.

1. Apri il menu **Start**, scrivi `Terminale`, fai clic destro su **Terminale** e scegli **Esegui come amministratore**.
2. Copia queste tre righe, una alla volta, premendo Invio dopo ognuna. Ogni riga richiede qualche minuto; aspetta che
   finisca prima di passare alla successiva.

   ```powershell
   winget install --id Microsoft.PowerShell -e
   winget install --id Git.Git -e
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   La terza riga installa *Build Tools for Visual Studio 2022* con il carico di lavoro *Sviluppo di applicazioni desktop
   con C++* — il compilatore C++ che serve al progetto (circa 6 GB). Una finestra dell'installer di Visual Studio mostra
   l'avanzamento.
3. **Riavvia il PC** quando tutte e tre hanno finito.

<details>
<summary>Preferisci installarli a mano?</summary>

- **PowerShell 7:** dal [Microsoft Store](https://apps.microsoft.com/detail/9mz1snwt0n5d) o dalla
  [pagina di PowerShell](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) di Microsoft.
- **Git per Windows:** da [git-scm.com](https://git-scm.com/download/win); le opzioni predefinite vanno bene.
- **Build Tools for Visual Studio 2022:** da [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
  (sotto *Strumenti per Visual Studio*). Nell'installer, spunta **Sviluppo di applicazioni desktop con C++** e fai clic
  su *Installa*. Se hai già Visual Studio 2022 con quel carico di lavoro, non devi installare nulla.

</details>

## Passo 2 — Tieni pronto il disco

Il launcher copia il disco sul PC al passo 5, quindi dopo non deve restare nel lettore. Prima del passo 5, fai una di
queste cose:

- **Il tuo disco:** inseriscilo nel lettore DVD.
- **Un'immagine `.iso` del tuo disco:** fai doppio clic sul file `.iso` in Esplora file. Windows la mostra come un nuovo
  lettore DVD (puoi espellerla dopo il passo 5).
- **Hai già copiato i file del disco in una cartella:** nulla da fare; potrai scegliere quella cartella al passo 5. Deve
  contenere `SYSTEM.CNF`, `SLUS_203.62` e la cartella `NFSHP2`.

## Passo 3 — Scarica HotPursuit2PC (una volta)

1. Apri il menu **Start**, scrivi `PowerShell 7` e aprilo (non il vecchio "Windows PowerShell").
2. Copia queste righe, premendo Invio dopo ognuna:

   ```powershell
   cd $HOME\Documents
   git clone https://github.com/awtocl/HotPursuit2PC.git
   cd HotPursuit2PC
   ```

Il progetto ora è in `Documenti\HotPursuit2PC`. (Usa `git clone` e non *Download ZIP*: Windows blocca gli script che
arrivano da uno ZIP scaricato, e `git` rende anche facili gli aggiornamenti.)

## Passo 4 — Compila il gioco

Nella stessa finestra di PowerShell 7, scrivi:

```powershell
pwsh tools/install-native.ps1
```

Lo script passa per cinque fasi e ti dice a che punto è (i suoi messaggi sono in inglese):

| Fase | Cosa succede | Quanto dura |
|---|---|---|
| 1/5 Your disc | Ancora niente: il launcher ti chiede il disco al passo 5 | — |
| 2/5 LLVM | **Solo la prima volta:** scarica e compila LLVM, il generatore di codice usato dal ricompilatore | **15–40 min** |
| 3/5 Building | Compila HotPursuit2PC: launcher, gioco e ricompilatore | 3–10 min |
| 4/5 Preparing | Ancora niente: lo fa il launcher dopo aver verificato il disco | — |
| 5/5 Game folder | Mette i tre programmi in `game-native\` | secondi |

Durante le fasi 2 e 3 il PC può essere rumoroso e occupato; è normale. Quando finisce con **"Done …"** in verde, il
launcher si apre da solo.

## Passo 5 — Configura il gioco nel launcher (una volta)

La prima volta, il launcher ti guida in quattro brevi passi:

1. **Disco** — il tuo lettore DVD (o la `.iso` montata) compare da solo; scegli **Copia da …** per copiarlo nella
   cartella del gioco (circa 3 GB, pochi minuti). Oppure scegli **una cartella con i file del disco**. Se il disco è di
   un'altra regione o edizione, il launcher te lo dice qui.
2. **Verifica** — ogni file viene confrontato con il disco originale.
3. **Prepara** — il programma del gioco viene tradotto in codice per il tuo processore (circa un minuto).
4. **Pronto** — la qualità d'immagine è già impostata per il tuo schermo. Scegli **Gioca ora**.

![La schermata principale del launcher](assets/launcher-home.png)

## Passo 6 — Gioca

D'ora in poi apri **`HotPursuit2Launcher.exe`** in `Documenti\HotPursuit2PC\game-native` e scegli **Gioca**. Per un
collegamento sul desktop: launcher → **Disco e installazione** → **Collegamento sul desktop**.

Il launcher si usa con mouse, tastiera (frecce, Invio, Esc, Pag su/Pag giù) o controller Xbox (A seleziona, B indietro,
LB/RB pagine, Start gioca). Le sue pagine:

| Pagina | A cosa serve |
|---|---|
| **Gioca** | Avviare il gioco; vedere se disco e gioco sono a posto; il tuo tempo di gioco |
| **Impostazioni** | Immagine e schermo, ogni impostazione spiegata, con il consiglio per il tuo schermo |
| **Comandi** | Configurazione del controller Classica o Moderna, riassegnare qualsiasi tasto o pulsante, ripristinare i predefiniti |
| **Memory card** | Cosa ha salvato il gioco; farne un backup e ripristinarlo |
| **Screenshot** | Le immagini che hai scattato con **F12** in gioco |
| **Disco e installazione** | Verificare tutti i file del disco, preparare di nuovo il gioco, usare un'altra copia del disco, creare un collegamento |
| **Informazioni** | Cos'è questo progetto, i crediti, link a questa guida e alle issue |

Il gioco parte a schermo intero con i filmati iniziali; premi **Invio** (o Start sul controller) per saltarli. Il
launcher si nasconde mentre giochi e torna quando esci.

### Impostazioni consigliate

Cambiale nella pagina **Impostazioni** del launcher, o mentre giochi con **F1** (o **Select + Start** sul controller).
L'installazione ha già scelto la **Qualità immagine** (la risoluzione interna) per il tuo schermo; è quella che decide
quanto è nitida l'immagine:

| Il tuo schermo | Qualità immagine |
|---|---|
| 1920 × 1080 (Full HD) | **Qualità (4x)** |
| 2560 × 1440 | **Ultra (6x)** |
| 3840 × 2160 (4K) | **Massima (8x)** |

Più alto è più fluido e nitido (disegna più pixel di quelli del tuo schermo e ne fa la media). Se il gioco scatta,
scegli il valore precedente.

| Impostazione | Consigliato | Perché |
|---|---|---|
| Renderer | **GPU** | Il renderer software è più lento e resta solo per confronto |
| Frame rate | **60** | Il doppio dei 30 fps della PS2 in gara; il gioco mantiene la sua velocità normale |
| Modalità schermo | **Schermo intero** | Alt+Invio o F11 cambiano in qualsiasi momento |
| V-Sync | **Attivo** | Movimento più stabile, senza strappi |
| Filtro di scala | **Bilineare nitido** | Nitido e senza scalettature |
| Formato | **16:9** | Visuale più ampia in gara; i menu mantengono la forma (passare tra 16:9 e 4:3 richiede un riavvio) |
| Nitidezza | **Disattivo** o **Basso** | Valori più alti possono far tremolare i bordi |
| Scanline | **Disattivo** | A meno che ti piaccia l'effetto vecchio televisore |

Le impostazioni si salvano da sole in `game-native\hp2native.ini`.

### Comandi

Scegli una configurazione nella pagina **Comandi** del launcher (o in gioco con **F1** → *Configurazione controller*):

| Azione | Tastiera | Controller: Classica (PS2) | Controller: Moderna |
|---|---|---|---|
| Accelera | Z | A | RT (o A) |
| Frena / Retromarcia | A | X | LT |
| Sterza | ← → | Levetta sinistra / croce direzionale | Levetta sinistra / croce direzionale |
| Freno a mano | W | RB | B |
| Clacson / Sirena | X | B | Clic levetta sinistra |
| Cambia visuale | S | Y | Y |
| Guarda indietro | Q | LB | Clic levetta destra |
| Boost di velocità | E | Clic levetta destra | X |
| Marcia su / Zone Zoom | 2 | RT | RB |
| Marcia giù / Zone Freeze | 1 | LT | LB |
| Pausa | Invio, Spazio | Start | Start |
| Cambia HUD | Backspace, Maiusc destro | Back | Back |
| Menu impostazioni | F1 | Select + Start | Select + Start |
| Screenshot | F12 | – | – |

- **Classica** è il controller PS2 tasto per tasto. **Moderna** mette acceleratore e freno sui grilletti, come i giochi
  di guida di oggi.
- **Creane una tua:** nella pagina **Comandi**, scegli una casella e premi il tasto o il pulsante che vuoi. Ogni azione
  accetta due tasti e due pulsanti del controller. Esc annulla, Canc cancella un tasto. La tua prima modifica crea una
  configurazione *Personalizzata* partendo da quella che stavi usando.
- **Hai cambiato idea?** **Ripristina tastiera predefinita** e **Ripristina controller predefinito** tornano subito alle
  configurazioni originali. La tua configurazione resta salvata; scegli *Personalizzata* per riusarla.
- Le levette analogiche funzionano come quelle del DualShock 2. Windows non riconosce da solo i controller PlayStation
  come controller di gioco; funzionano tramite uno strumento che li presenta come controller Xbox, come il supporto
  controller di Steam.

### Lingua

Il launcher e il menu impostazioni in gioco sono in inglese, spagnolo, portoghese (Brasile), francese, tedesco e
italiano. La prima volta seguono la lingua di visualizzazione di Windows; cambiala nel launcher in **Impostazioni →
Lingua** o in gioco con **F1**. I testi del gioco restano in inglese: così è fatto il disco nordamericano.

### Uscire

**F1 → Esci dal gioco** (premi due volte per confermare), oppure **Alt+F4**.

## Aggiornare

Quando il progetto riceve novità, aggiorna e ricompila — impostazioni, salvataggi e copia del disco restano:

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1
```

LLVM non viene ricompilato, quindi ci vogliono solo pochi minuti. Poi il launcher mostra **Prepara e gioca**: il gioco
viene preparato di nuovo per la nuova versione (circa un minuto) e parte.

## Se qualcosa va storto

| Cosa vedi | Cosa fare |
|---|---|
| *"The C++ build tools were not found"* | Il passo 1 non è completo: installa i Build Tools con **Sviluppo di applicazioni desktop con C++**, riavvia il PC e riprova. |
| *"pwsh non è riconosciuto"* | PowerShell 7 non è installato, oppure il PC non è stato riavviato dopo il passo 1. |
| Il launcher non trova il disco | Aspetta qualche secondo dopo averlo inserito; con una `.iso`, fai prima doppio clic. Oppure copia tu i file del disco in una cartella e scegli **una cartella con i file del disco**. |
| *"Questa cartella non contiene un disco PlayStation 2"* o *"Manca …"* | Cartella sbagliata o copia incompleta. Scegli la cartella che contiene direttamente `SYSTEM.CNF`, `SLUS_203.62` e `NFSHP2`. |
| *"Questo disco è SLES-…"* o *"… non è la versione supportata"* | Il tuo disco è di un'altra regione o edizione. È supportata solo l'edizione nordamericana `SLUS-20362`. |
| *"… è diverso dal disco originale"* | La copia del disco è danneggiata (spesso un disco graffiato). Pulisci il disco e copialo di nuovo (**Disco e installazione** → **Cambia disco**). |
| *"Preparazione del gioco non riuscita"* | I dettagli sono in `game-native\prepare.log`; allegalo a una [issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"Il gioco si è fermato: …"* nella pagina Gioca | Scegli **Apri log** e **Segnala un problema**; allega `native.log`. |
| *"Downloading LLVM failed"* | Controlla la connessione a internet e riavvia l'installer. |
| *"The build failed"* | Copia le righe rosse e [apri una issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"…non può essere caricato perché l'esecuzione di script è disabilitata"* | Hai scaricato uno ZIP invece di usare `git clone`. Usa `git clone` (passo 3). |
| La finestra del gioco è nera, o l'immagine è sbagliata | Launcher → **Impostazioni** → Renderer → **Software**. Se aiuta, segnalalo indicando il modello della tua scheda video. |
| Niente audio | Il gioco usa il dispositivo di riproduzione predefinito di Windows: controllalo nelle impostazioni audio di Windows e riavvia il gioco. |
| Il controller non fa nulla | Sono riconosciuti solo controller compatibili Xbox (XInput); vedi [Comandi](#comandi). La tastiera funziona sempre. |
| Un pulsante fa la cosa sbagliata | Launcher → **Comandi** → **Ripristina controller predefinito** (o **Ripristina tastiera predefinita**). |
| Scatta | Abbassa la **Qualità immagine** (launcher → Impostazioni) e controlla che il V-Sync sia attivo. |
| Dopo aver cambiato impostazioni sembra tutto sbagliato | Launcher → **Impostazioni** → **Ripristina**. |

**Segnalare un problema:** dopo la chiusura del gioco, il file `game-native\native.log` descrive cosa è successo.
Allegalo a una [issue](https://github.com/awtocl/HotPursuit2PC/issues) con la tua versione di Windows e la tua scheda
video. Il log non contiene dati personali a parte i percorsi delle cartelle, che puoi cancellare.

## Disinstallare

Questo progetto non installa nulla in Windows. Per rimuoverlo, elimina la cartella `Documenti\HotPursuit2PC` (gli
strumenti di compilazione sono lì dentro, in `.cache`, e la copia del disco in `game-native\disc`). Prima, fai una copia
di `game-native\saves` se vuoi tenere i salvataggi. Un collegamento creato dal launcher si elimina come qualsiasi altro.
Gli strumenti del passo 1 si rimuovono in *Impostazioni → App* come qualsiasi altro programma.

---

Qualcosa non va, o non è chiaro? [Apri una issue](https://github.com/awtocl/HotPursuit2PC/issues) e faccelo
sapere — questa guida deve funzionare per tutti. Divertiti in strada, e occhio alla polizia!
