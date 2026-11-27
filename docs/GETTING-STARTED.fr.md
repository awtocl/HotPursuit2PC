# Bien démarrer — jouez à *Hot Pursuit 2* en natif sur votre PC

🌐 [English](GETTING-STARTED.md) · [Español](GETTING-STARTED.es.md) · [Português (Brasil)](GETTING-STARTED.pt-BR.md) · **Français** · [Deutsch](GETTING-STARTED.de.md) · [Italiano](GETTING-STARTED.it.md)

Bienvenue ! Ce guide vous emmène de votre disque PS2 jusqu'à la course sur votre PC, une étape à la fois. Vous taperez
quelques commandes, et le reste se fait tout seul. Comptez **environ une heure la première fois** — surtout de l'attente
pendant que votre PC compile, alors prévoyez un en-cas. Ensuite, lancer le jeu se fait d'un double-clic.

> [!IMPORTANT]
> Il vous faut **votre propre disque original** de *Need for Speed: Hot Pursuit 2* pour PlayStation 2. Le projet ne
> fournit aucun fichier du jeu et n'en télécharge aucun.

> [!WARNING]
> **La sauvegarde n'est pas encore testée.** Le jeu écrit sur une carte mémoire conservée sous forme de fichiers dans le
> dossier du jeu, mais sauvegarder et recharger une carrière n'a pas été vérifié du début à la fin. Ne comptez pas encore
> sur la conservation de votre progression. Les courses simples se jouent du début à la fin.

---

## La méthode rapide : télécharger

Depuis la version 0.2.0, il existe un téléchargement prêt à jouer : vous pouvez sauter l'installation des outils et la
compilation (étapes 1, 3 et 4) :

1. Téléchargez **`HotPursuit2PC-v0.2.0-win64.zip`** depuis la [dernière version](https://github.com/awtocl/HotPursuit2PC/releases/latest).
2. Clic droit dessus → **Extraire tout…**, puis choisissez le dossier qui vous plaît (par exemple
   `C:\Jeux\HotPursuit2PC`).
3. Ouvrez le dossier et double-cliquez sur **`HotPursuit2Launcher.exe`**. Windows peut dire qu'il a protégé votre PC
   (les programmes ne sont pas signés) : cliquez sur **Informations complémentaires**, puis sur **Exécuter quand même**.
4. Suivez ensuite l'**étape 2** (votre disque) et l'**étape 5** (la préparation dans le lanceur) ci-dessous.

Il vous faut environ 4 Go libres, surtout pour la copie de votre disque. Vous préférez compiler vous-même, ou vous
voulez les toutes dernières modifications ? Suivez toutes les étapes ci-dessous.

## Ce qu'il vous faut

| Il vous faut | Détails |
|---|---|
| **Le disque** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **édition nord-américaine**. Le code **SLUS-20362** est imprimé sur le disque et au dos du boîtier. Les autres régions (par exemple l'européenne `SLES-50731`) et toute édition avec un autre programme ne sont pas prises en charge — le lanceur vérifie cela pour vous. |
| **De quoi le lire** | Un lecteur DVD dans votre PC (ou branché dessus). Si vous avez déjà fait une image `.iso` de votre propre disque, elle convient aussi. |
| **Le PC** | Windows 10 ou 11, 64 bits. Une carte graphique Direct3D 11 (n'importe laquelle des dix dernières années). |
| **Espace disque** | Environ **15 Go** : 3 Go pour la copie de votre disque, 6 Go pour les outils de compilation C++ (étape 1) et 6 Go pour le projet et son générateur de code (étape 4). |
| **Une manette** *(recommandée)* | Une manette Xbox, ou tout ce que Windows reconnaît comme telle. Le clavier fonctionne aussi. |
| **Internet** | Pour l'étape 1 et pour le premier lancement de l'installateur (il télécharge environ 1 à 2 Go d'outils et de code source). |

## Étape 1 — Installez les outils (une fois)

Le jeu est compilé sur votre PC, il lui faut donc trois outils gratuits de Microsoft et de Git. Le plus simple est
**winget**, fourni avec Windows 10 et 11.

1. Ouvrez le menu **Démarrer**, tapez `Terminal`, faites un clic droit sur **Terminal** et choisissez **Exécuter en tant
   qu'administrateur**.
2. Copiez ces trois lignes, une à la fois, en appuyant sur Entrée après chacune. Chaque ligne prend quelques minutes ;
   attendez qu'elle se termine avant de passer à la suivante.

   ```powershell
   winget install --id Microsoft.PowerShell -e
   winget install --id Git.Git -e
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   La troisième ligne installe *Build Tools for Visual Studio 2022* avec la charge de travail *Développement Desktop en
   C++* — le compilateur C++ dont le projet a besoin (environ 6 Go). Une fenêtre de l'installateur Visual Studio affiche
   la progression.
3. **Redémarrez le PC** une fois les trois terminées.

<details>
<summary>Vous préférez les installer à la main ?</summary>

- **PowerShell 7 :** depuis le [Microsoft Store](https://apps.microsoft.com/detail/9mz1snwt0n5d) ou la
  [page PowerShell](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) de Microsoft.
- **Git pour Windows :** sur [git-scm.com](https://git-scm.com/download/win) ; les options par défaut conviennent.
- **Build Tools for Visual Studio 2022 :** sur [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
  (sous *Outils pour Visual Studio*). Dans l'installateur, cochez **Développement Desktop en C++** et cliquez sur
  *Installer*. Si vous avez déjà Visual Studio 2022 avec cette charge de travail, il n'y a rien à installer.

</details>

## Étape 2 — Préparez votre disque

Le lanceur copie votre disque sur le PC à l'étape 5 ; ensuite, le disque n'a pas besoin de rester dans le lecteur.
Avant l'étape 5, faites l'une de ces choses :

- **Votre disque :** mettez-le dans le lecteur DVD.
- **Une image `.iso` de votre propre disque :** double-cliquez sur le fichier `.iso` dans l'Explorateur de fichiers.
  Windows l'affiche comme un nouveau lecteur DVD (vous pourrez l'éjecter après l'étape 5).
- **Vous avez déjà copié les fichiers du disque dans un dossier :** rien à faire ; vous pourrez choisir ce dossier à
  l'étape 5. Il doit contenir `SYSTEM.CNF`, `SLUS_203.62` et le dossier `NFSHP2`.

## Étape 3 — Téléchargez HotPursuit2PC (une fois)

1. Ouvrez le menu **Démarrer**, tapez `PowerShell 7` et ouvrez-le (pas l'ancien « Windows PowerShell »).
2. Copiez ces lignes, en appuyant sur Entrée après chacune :

   ```powershell
   cd $HOME\Documents
   git clone https://github.com/awtocl/HotPursuit2PC.git
   cd HotPursuit2PC
   ```

Le projet se trouve maintenant dans `Documents\HotPursuit2PC`. (Utilisez `git clone` plutôt que *Download ZIP* :
Windows bloque les scripts issus d'un ZIP téléchargé, et `git` rend aussi les mises à jour faciles.)

## Étape 4 — Compilez le jeu

Dans la même fenêtre PowerShell 7, tapez :

```powershell
pwsh tools/install-native.ps1
```

Le script passe par cinq étapes et vous dit où il en est (ses messages sont en anglais) :

| Étape | Ce qui se passe | Durée |
|---|---|---|
| 1/5 Your disc | Rien pour l'instant : le lanceur vous demande votre disque à l'étape 5 | — |
| 2/5 LLVM | **La première fois seulement :** télécharge et compile LLVM, le générateur de code du recompilateur | **15–40 min** |
| 3/5 Building | Compile HotPursuit2PC : le lanceur, le jeu et le recompilateur | 3–10 min |
| 4/5 Preparing | Rien pour l'instant : le lanceur s'en charge après avoir vérifié votre disque | — |
| 5/5 Game folder | Place les trois programmes dans `game-native\` | quelques secondes |

Votre PC peut faire du bruit et être très occupé pendant les étapes 2 et 3 ; c'est normal. Quand tout se termine par
**« Done … »** en vert, le lanceur s'ouvre tout seul.

## Étape 5 — Configurez le jeu dans le lanceur (une fois)

La première fois, le lanceur vous guide en quatre étapes courtes :

1. **Disque** — votre lecteur DVD (ou l'`.iso` montée) apparaît tout seul ; choisissez **Copier depuis …** pour le copier
   dans le dossier du jeu (environ 3 Go, quelques minutes). Ou choisissez **un dossier avec les fichiers du disque**. Si
   le disque est d'une autre région ou édition, le lanceur vous le dit ici.
2. **Vérifier** — chaque fichier est comparé au disque d'origine.
3. **Préparer** — le programme du jeu est traduit en code pour votre processeur (environ une minute).
4. **Prêt** — la qualité d'image est déjà réglée pour votre écran. Choisissez **Jouer maintenant**.

![L'écran d'accueil du lanceur](assets/launcher-home.png)

## Étape 6 — Jouez

Désormais, ouvrez **`HotPursuit2Launcher.exe`** dans `Documents\HotPursuit2PC\game-native` et choisissez **Jouer**. Pour
un raccourci sur le bureau : lanceur → **Disque et installation** → **Raccourci sur le bureau**.

Le lanceur se pilote à la souris, au clavier (flèches, Entrée, Échap, Page préc./Page suiv.) ou à la manette Xbox (A
choisir, B retour, LB/RB pages, Start lance le jeu). Ses pages :

| Page | À quoi elle sert |
|---|---|
| **Jouer** | Lancer le jeu ; voir si le disque et le jeu sont en ordre ; votre temps de jeu |
| **Paramètres** | L'image et l'affichage, chaque réglage expliqué, avec la recommandation pour votre écran |
| **Commandes** | Disposition de manette Classique ou Moderne, réassigner n'importe quelle touche ou bouton, rétablir les valeurs d'origine |
| **Carte mémoire** | Ce que le jeu a sauvegardé ; en faire une sauvegarde et la restaurer |
| **Captures d'écran** | Les images prises avec **F12** dans le jeu |
| **Disque et installation** | Vérifier tous les fichiers du disque, préparer à nouveau le jeu, utiliser une autre copie du disque, créer un raccourci |
| **À propos** | Ce qu'est ce projet, les crédits, des liens vers ce guide et vers les issues |

Le jeu démarre en plein écran avec les vidéos d'introduction ; appuyez sur **Entrée** (ou Start sur la manette) pour les
passer. Le lanceur se cache pendant que vous jouez et revient quand vous quittez.

### Réglages recommandés

Modifiez-les sur la page **Paramètres** du lanceur, ou en jouant avec **F1** (ou **Select + Start** sur la manette).
L'installation a déjà choisi la **Qualité d'image** (la résolution interne) pour votre écran ; c'est elle qui décide de
la netteté de l'image :

| Votre écran | Qualité d'image |
|---|---|
| 1920 × 1080 (Full HD) | **Qualité (4x)** |
| 2560 × 1440 | **Ultra (6x)** |
| 3840 × 2160 (4K) | **Maximum (8x)** |

Plus haut, c'est plus fluide et plus net (le jeu dessine plus de pixels que votre écran n'en a et en fait la moyenne).
Si le jeu saccade, choisissez la valeur inférieure.

| Réglage | Recommandé | Pourquoi |
|---|---|---|
| Rendu | **GPU** | Le rendu logiciel est plus lent et conservé pour comparer |
| Images par seconde | **60** | Deux fois les 30 images par seconde de la PS2 en course ; le jeu garde sa vitesse normale |
| Mode d'affichage | **Plein écran** | Alt+Entrée ou F11 basculent à tout moment |
| Synchro verticale | **Activé** | Mouvement plus stable, sans déchirure |
| Filtre de mise à l'échelle | **Bilinéaire net** | Net et sans escaliers |
| Format d'image | **16:9** | Vue plus large en course ; les menus gardent leur forme (passer de 16:9 à 4:3 demande un redémarrage) |
| Netteté | **Désactivé** ou **Faible** | Des valeurs plus hautes peuvent faire scintiller les contours |
| Lignes de balayage | **Désactivé** | Sauf si vous aimez le look des vieux téléviseurs |
| Anticrénelage | **FXAA** | Adoucit les bords en escalier ; aux qualités d'image les plus hautes, la différence est faible |

Les réglages sont enregistrés automatiquement dans `game-native\hp2native.ini`.

### Commandes

Choisissez une disposition sur la page **Commandes** du lanceur (ou en jeu avec **F1** → *Disposition manette*) :

| Action | Clavier | Manette : Classique (PS2) | Manette : Moderne |
|---|---|---|---|
| Accélérer | Z | A | RT (ou A) |
| Freiner / Marche arrière | A | X | LT |
| Diriger | ← → | Stick gauche / croix | Stick gauche / croix |
| Frein à main | W | RB | B |
| Klaxon / Sirène | X | B | Clic du stick gauche |
| Changer de vue | S | Y | Y |
| Regarder derrière | Q | LB | Clic du stick droit |
| Boost de vitesse | E | Clic du stick droit | X |
| Rapport sup. / Zone Zoom | 2 | RT | RB |
| Rapport inf. / Zone Freeze | 1 | LT | LB |
| Pause | Entrée, Espace | Start | Start |
| Changer de HUD | Retour arrière, Maj droite | Back | Back |
| Menu des paramètres | F1 | Select + Start | Select + Start |
| Capture d'écran | F12 | – | – |

- **Classique** reprend la manette PS2 bouton pour bouton. **Moderne** met l'accélérateur et le frein sur les
  gâchettes, comme les jeux de course actuels.
- **Créez la vôtre :** sur la page **Commandes**, choisissez une case et appuyez sur la touche ou le bouton voulu.
  Chaque action accepte deux touches et deux boutons de manette. Échap annule, Suppr efface une touche. Votre premier
  changement crée une disposition *Personnalisée* à partir de celle que vous utilisiez.
- **Vous changez d'avis ?** **Rétablir le clavier par défaut** et **Rétablir la manette par défaut** reviennent
  directement aux dispositions d'origine. Votre disposition personnalisée reste enregistrée ; choisissez *Personnalisé*
  pour la retrouver.
- Les sticks analogiques fonctionnent comme ceux de la DualShock 2. Windows ne reconnaît pas seul les manettes
  PlayStation comme manettes de jeu ; elles fonctionnent via un outil qui les présente comme une manette Xbox, comme la
  prise en charge des manettes de Steam.

### Langue

Le lanceur et le menu des paramètres en jeu existent en anglais, espagnol, portugais (Brésil), français, allemand et
italien. La première fois, ils suivent la langue d'affichage de Windows ; changez-la dans le lanceur sous **Paramètres →
Langue** ou en jeu avec **F1**. Les textes du jeu lui-même restent en anglais : c'est ainsi qu'est livré le disque
nord-américain.

### Quitter

**F1 → Quitter le jeu** (appuyez deux fois pour confirmer), ou **Alt+F4**.

## Mettre à jour

Quand le projet reçoit des nouveautés, mettez à jour et recompilez — vos réglages, sauvegardes et la copie du disque
restent en place :

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1
```

LLVM n'est pas recompilé, donc cela ne prend que quelques minutes. Le lanceur affiche ensuite **Préparer et jouer** : le
jeu est préparé à nouveau pour la nouvelle version (environ une minute) puis démarre.

## En cas de problème

| Ce que vous voyez | Que faire |
|---|---|
| *« The C++ build tools were not found »* | L'étape 1 n'est pas terminée : installez les Build Tools avec **Développement Desktop en C++**, redémarrez le PC et réessayez. |
| *« pwsh n'est pas reconnu »* | PowerShell 7 n'est pas installé, ou le PC n'a pas été redémarré après l'étape 1. |
| Le lanceur ne trouve pas votre disque | Attendez quelques secondes après l'avoir inséré ; pour une `.iso`, double-cliquez d'abord dessus. Ou copiez vous-même les fichiers du disque dans un dossier et choisissez **un dossier avec les fichiers du disque**. |
| *« Ce dossier ne contient pas un disque PlayStation 2 »* ou *« … manque »* | Mauvais dossier, ou copie incomplète. Choisissez le dossier qui contient directement `SYSTEM.CNF`, `SLUS_203.62` et `NFSHP2`. |
| *« Ce disque est SLES-… »* ou *« … n'est pas la version prise en charge »* | Votre disque est d'une autre région ou édition. Seule l'édition nord-américaine `SLUS-20362` est prise en charge. |
| *« … diffère du disque d'origine »* | La copie du disque est endommagée (souvent un disque rayé). Nettoyez le disque et copiez-le à nouveau (**Disque et installation** → **Changer de disque**). |
| *« La préparation du jeu a échoué »* | Les détails sont dans `game-native\prepare.log` ; joignez-le à une [issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *« Le jeu s'est arrêté : … »* sur la page Jouer | Choisissez **Ouvrir le journal** et **Signaler un problème** ; joignez `native.log`. |
| *« Downloading LLVM failed »* | Vérifiez votre connexion internet et relancez l'installateur. |
| *« The build failed »* | Copiez les lignes rouges et [ouvrez une issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *« …ne peut pas être chargé car l'exécution de scripts est désactivée »* | Vous avez téléchargé un ZIP au lieu d'utiliser `git clone`. Utilisez `git clone` (étape 3). |
| La fenêtre du jeu est noire, ou l'image est fausse | Lanceur → **Paramètres** → Rendu → **Logiciel**. Si cela aide, signalez-le en indiquant votre modèle de carte graphique. |
| Pas de son | Le jeu utilise le périphérique de lecture par défaut de Windows : vérifiez-le dans les paramètres de son de Windows, puis relancez le jeu. |
| La manette ne fait rien | Seules les manettes compatibles Xbox (XInput) sont reconnues ; voir [Commandes](#commandes). Le clavier fonctionne toujours. |
| Un bouton fait la mauvaise chose | Lanceur → **Commandes** → **Rétablir la manette par défaut** (ou **Rétablir le clavier par défaut**). |
| Ça saccade | Baissez la **Qualité d'image** (lanceur → Paramètres) et vérifiez que la synchro verticale est activée. |
| Tout semble faux après avoir changé des réglages | Lanceur → **Paramètres** → **Réinitialiser**. |

**Signaler un problème :** après la fermeture du jeu, le fichier `game-native\native.log` décrit ce qui s'est passé.
Joignez-le à une [issue](https://github.com/awtocl/HotPursuit2PC/issues) avec votre version de Windows et votre carte
graphique. Le journal ne contient aucune donnée personnelle à part des chemins de dossiers, que vous pouvez effacer.

## Désinstaller

Ce projet n'installe rien dans Windows. Pour l'enlever, supprimez le dossier `Documents\HotPursuit2PC` (les outils de
compilation sont dedans, dans `.cache`, et la copie de votre disque dans `game-native\disc`). Avant, sauvegardez
`game-native\saves` si vous voulez garder vos sauvegardes. Un raccourci créé par le lanceur se supprime comme n'importe
quel autre. Les outils de l'étape 1 se désinstallent dans *Paramètres → Applications* comme tout autre programme.

---

Un souci, ou quelque chose n'est pas clair ? [Ouvrez une issue](https://github.com/awtocl/HotPursuit2PC/issues) et
dites-le-nous — ce guide doit marcher pour tout le monde. Amusez-vous bien sur la route, et gare aux flics !
