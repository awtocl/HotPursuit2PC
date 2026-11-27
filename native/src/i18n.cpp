// Languages of the port's own text (ADR 0017). See i18n.h. Columns: English, Spanish, Portuguese (Brazil), French,
// German, Italian. Placeholders {0}, {1}, ... stay as they are. Add a row for every new English string shown to players.
#include "i18n.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace i18n {
namespace {

struct Row {
  const wchar_t* t[kLangCount];
};

const Row kRows[] = {
    // ---- pages and navigation ----
    {L"Play", L"Jugar", L"Jogar", L"Jouer", L"Spielen", L"Gioca"},
    {L"Settings", L"Ajustes", L"Configurações", L"Paramètres", L"Einstellungen", L"Impostazioni"},
    {L"Controls", L"Controles", L"Controles", L"Commandes", L"Steuerung", L"Comandi"},
    {L"Memory Card", L"Tarjeta de memoria", L"Cartão de memória", L"Carte mémoire", L"Memory Card", L"Memory card"},
    {L"Screenshots", L"Capturas", L"Capturas de tela", L"Captures d'écran", L"Screenshots", L"Screenshot"},
    {L"Disc & Setup", L"Disco e instalación", L"Disco e instalação", L"Disque et installation", L"Disc & Einrichtung", L"Disco e installazione"},
    {L"About", L"Acerca de", L"Sobre", L"À propos", L"Info", L"Informazioni"},
    {L"Setup", L"Instalación", L"Instalação", L"Installation", L"Einrichtung", L"Installazione"},
    {L"Choose", L"Elegir", L"Selecionar", L"Choisir", L"Auswählen", L"Seleziona"},
    {L"Go back", L"Atrás", L"Voltar", L"Retour", L"Zurück", L"Indietro"},
    {L"Change", L"Cambiar", L"Alterar", L"Modifier", L"Ändern", L"Cambia"},
    {L"Pages", L"Páginas", L"Páginas", L"Pages", L"Seiten", L"Pagine"},
    // ---- ticker ----
    {L"Your disc is only read. Nothing is downloaded and nothing leaves this PC.",
     L"Tu disco solo se lee. No se descarga nada y nada sale de este PC.",
     L"Seu disco só é lido. Nada é baixado e nada sai deste PC.",
     L"Votre disque est seulement lu. Rien n'est téléchargé et rien ne quitte ce PC.",
     L"Deine Disc wird nur gelesen. Nichts wird heruntergeladen, nichts verlässt diesen PC.",
     L"Il tuo disco viene solo letto. Non si scarica nulla e nulla lascia questo PC."},
    {L"Checking the game files...", L"Comprobando los archivos del juego...", L"Verificando os arquivos do jogo...",
     L"Vérification des fichiers du jeu...", L"Spieldateien werden geprüft...", L"Controllo dei file del gioco..."},
    {L"The game needs preparing (about a minute) - press Play.", L"Hay que preparar el juego (un minuto aprox.): pulsa Jugar.",
     L"O jogo precisa ser preparado (cerca de um minuto): aperte Jogar.", L"Le jeu doit être préparé (environ une minute) : appuyez sur Jouer.",
     L"Das Spiel muss vorbereitet werden (etwa eine Minute) – drück auf Spielen.", L"Il gioco va preparato (circa un minuto): premi Gioca."},
    {L"Ready to race! In the game: F1 settings, F12 screenshot, Alt+Enter full screen.",
     L"¡Listo para correr! En el juego: F1 ajustes, F12 captura, Alt+Intro pantalla completa.",
     L"Pronto para correr! No jogo: F1 configurações, F12 captura, Alt+Enter tela cheia.",
     L"Prêt à courir ! En jeu : F1 paramètres, F12 capture, Alt+Entrée plein écran.",
     L"Bereit zum Rennen! Im Spiel: F1 Einstellungen, F12 Screenshot, Alt+Enter Vollbild.",
     L"Pronto a correre! In gioco: F1 impostazioni, F12 screenshot, Alt+Invio schermo intero."},
    {L"Select a box and press a key or button for it. Esc cancels, Delete clears a key.",
     L"Elige una casilla y pulsa una tecla o botón. Esc cancela, Supr borra una tecla.",
     L"Escolha uma caixa e aperte uma tecla ou botão. Esc cancela, Delete apaga uma tecla.",
     L"Choisissez une case et appuyez sur une touche ou un bouton. Échap annule, Suppr efface une touche.",
     L"Wähle ein Feld und drück eine Taste oder einen Knopf. Esc bricht ab, Entf löscht eine Taste.",
     L"Scegli una casella e premi un tasto o un pulsante. Esc annulla, Canc cancella un tasto."},
    // ---- toasts ----
    {L"Now using your custom controller layout - Restore Defaults goes back",
     L"Ahora usas tu esquema de mando personalizado; «Restaurar» vuelve al original",
     L"Agora você usa seu layout de controle personalizado; \"Restaurar\" volta ao padrão",
     L"Disposition manette personnalisée active – « Rétablir » revient à l'original",
     L"Jetzt mit deiner eigenen Controller-Belegung – „Zurücksetzen“ stellt den Standard wieder her",
     L"Ora usi la tua configurazione del controller – «Ripristina» torna a quella originale"},
    {L"Now using your custom keyboard layout - Restore Defaults goes back",
     L"Ahora usas tu esquema de teclado personalizado; «Restaurar» vuelve al original",
     L"Agora você usa seu layout de teclado personalizado; \"Restaurar\" volta ao padrão",
     L"Disposition clavier personnalisée active – « Rétablir » revient à l'original",
     L"Jetzt mit deiner eigenen Tastaturbelegung – „Zurücksetzen“ stellt den Standard wieder her",
     L"Ora usi la tua configurazione della tastiera – «Ripristina» torna a quella originale"},
    {L"Keyboard back to the default layout (your custom one is kept)",
     L"Teclado de vuelta al esquema original (el tuyo se guarda)", L"Teclado de volta ao layout padrão (o seu fica guardado)",
     L"Clavier revenu à la disposition d'origine (la vôtre est conservée)", L"Tastatur wieder auf Standard (deine Belegung bleibt gespeichert)",
     L"Tastiera tornata alla configurazione originale (la tua resta salvata)"},
    {L"Controller back to the Classic layout (your custom one is kept)",
     L"Mando de vuelta al esquema Clásico (el tuyo se guarda)", L"Controle de volta ao layout Clássico (o seu fica guardado)",
     L"Manette revenue à la disposition Classique (la vôtre est conservée)", L"Controller wieder auf Klassisch (deine Belegung bleibt gespeichert)",
     L"Controller tornato alla configurazione Classica (la tua resta salvata)"},
    {L"Settings reset (picture quality as recommended for your screen)",
     L"Ajustes restablecidos (calidad de imagen recomendada para tu pantalla)",
     L"Configurações restauradas (qualidade de imagem recomendada para sua tela)",
     L"Paramètres réinitialisés (qualité d'image recommandée pour votre écran)",
     L"Einstellungen zurückgesetzt (Bildqualität wie für deinen Bildschirm empfohlen)",
     L"Impostazioni ripristinate (qualità d'immagine consigliata per il tuo schermo)"},
    {L"Shortcut \"Hot Pursuit 2 (PC)\" placed on the desktop", L"Acceso directo «Hot Pursuit 2 (PC)» creado en el escritorio",
     L"Atalho \"Hot Pursuit 2 (PC)\" criado na área de trabalho", L"Raccourci « Hot Pursuit 2 (PC) » créé sur le bureau",
     L"Verknüpfung „Hot Pursuit 2 (PC)“ auf dem Desktop erstellt", L"Collegamento «Hot Pursuit 2 (PC)» creato sul desktop"},
    {L"The shortcut could not be made", L"No se pudo crear el acceso directo", L"Não foi possível criar o atalho",
     L"Le raccourci n'a pas pu être créé", L"Die Verknüpfung konnte nicht erstellt werden", L"Impossibile creare il collegamento"},
    {L"All files match the original disc", L"Todos los archivos coinciden con el disco original", L"Todos os arquivos batem com o disco original",
     L"Tous les fichiers correspondent au disque d'origine", L"Alle Dateien stimmen mit der Original-Disc überein",
     L"Tutti i file corrispondono al disco originale"},
    {L"The game is prepared", L"El juego está preparado", L"O jogo está preparado", L"Le jeu est prêt", L"Das Spiel ist vorbereitet",
     L"Il gioco è pronto"},
    // ---- Play page ----
    {L"Preparing...", L"Preparando...", L"Preparando...", L"Préparation...", L"Wird vorbereitet...", L"Preparazione..."},
    {L"Prepare & Play", L"Preparar y jugar", L"Preparar e jogar", L"Préparer et jouer", L"Vorbereiten & spielen", L"Prepara e gioca"},
    {L"Game Folder", L"Carpeta del juego", L"Pasta do jogo", L"Dossier du jeu", L"Spielordner", L"Cartella del gioco"},
    {L"Last played {0}  ·  {1} behind the wheel", L"Última partida {0}  ·  {1} al volante", L"Última partida {0}  ·  {1} ao volante",
     L"Dernière partie {0}  ·  {1} au volant", L"Zuletzt gespielt {0}  ·  {1} am Steuer", L"Ultima partita {0}  ·  {1} al volante"},
    {L"Not played yet - have a good race!", L"Aún sin jugar: ¡buena carrera!", L"Ainda não jogado – boa corrida!",
     L"Pas encore joué – bonne course !", L"Noch nicht gespielt – gutes Rennen!", L"Non ancora giocato – buona corsa!"},
    {L"Status", L"Estado", L"Status", L"État", L"Status", L"Stato"},
    {L"Disc checked", L"Disco comprobado", L"Disco verificado", L"Disque vérifié", L"Disc geprüft", L"Disco verificato"},
    {L"Disc found", L"Disco encontrado", L"Disco encontrado", L"Disque trouvé", L"Disc gefunden", L"Disco trovato"},
    {L"Game prepared", L"Juego preparado", L"Jogo preparado", L"Jeu prêt", L"Spiel vorbereitet", L"Gioco pronto"},
    {L"Checking the game...", L"Comprobando el juego...", L"Verificando o jogo...", L"Vérification du jeu...", L"Spiel wird geprüft...",
     L"Controllo del gioco..."},
    {L"Not prepared yet", L"Aún sin preparar", L"Ainda não preparado", L"Pas encore préparé", L"Noch nicht vorbereitet", L"Non ancora preparato"},
    {L"New version: prepare again", L"Nueva versión: preparar de nuevo", L"Nova versão: preparar de novo", L"Nouvelle version : préparer à nouveau",
     L"Neue Version: neu vorbereiten", L"Nuova versione: prepara di nuovo"},
    {L"Something went wrong", L"Algo salió mal", L"Algo deu errado", L"Un problème est survenu", L"Etwas ist schiefgelaufen", L"Qualcosa è andato storto"},
    {L"Open Log", L"Abrir registro", L"Abrir log", L"Ouvrir le journal", L"Log öffnen", L"Apri log"},
    {L"Report a Problem", L"Informar de un problema", L"Relatar um problema", L"Signaler un problème", L"Problem melden", L"Segnala un problema"},
    {L"In the game", L"En el juego", L"No jogo", L"En jeu", L"Im Spiel", L"In gioco"},
    {L"Settings and Quit game", L"Ajustes y salir del juego", L"Configurações e sair do jogo", L"Paramètres et quitter le jeu",
     L"Einstellungen und Spiel beenden", L"Impostazioni ed esci dal gioco"},
    {L"Screenshot", L"Captura", L"Captura de tela", L"Capture d'écran", L"Screenshot", L"Screenshot"},
    {L"Full screen", L"Pantalla completa", L"Tela cheia", L"Plein écran", L"Vollbild", L"Schermo intero"},
    {L"Settings (controller)", L"Ajustes (mando)", L"Configurações (controle)", L"Paramètres (manette)", L"Einstellungen (Controller)",
     L"Impostazioni (controller)"},
    {L"The game stopped: {0}", L"El juego se detuvo: {0}", L"O jogo parou: {0}", L"Le jeu s'est arrêté : {0}", L"Das Spiel wurde beendet: {0}",
     L"Il gioco si è fermato: {0}"},
    // ---- Settings page ----
    {L"Picture", L"Imagen", L"Imagem", L"Image", L"Bild", L"Immagine"},
    {L"Launcher", L"Launcher", L"Launcher", L"Lanceur", L"Launcher", L"Launcher"},
    {L"General", L"General", L"Geral", L"Général", L"Allgemein", L"Generale"},
    {L"Picture Quality", L"Calidad de imagen", L"Qualidade de imagem", L"Qualité d'image", L"Bildqualität", L"Qualità immagine"},
    {L"After the Game Closes", L"Al cerrar el juego", L"Ao fechar o jogo", L"À la fermeture du jeu", L"Nach dem Spiel", L"Alla chiusura del gioco"},
    {L"Close the launcher", L"Cerrar el launcher", L"Fechar o launcher", L"Fermer le lanceur", L"Launcher schließen", L"Chiudi il launcher"},
    {L"Show the launcher", L"Mostrar el launcher", L"Mostrar o launcher", L"Afficher le lanceur", L"Launcher anzeigen", L"Mostra il launcher"},
    {L"Recommended", L"Recomendado", L"Recomendado", L"Recommandé", L"Empfohlen", L"Consigliato"},
    {L"Reset to Defaults", L"Restablecer", L"Restaurar padrão", L"Réinitialiser", L"Zurücksetzen", L"Ripristina"},
    {L"Recommended: {0}.", L"Recomendado: {0}.", L"Recomendado: {0}.", L"Recommandé : {0}.", L"Empfohlen: {0}.", L"Consigliato: {0}."},
    {L"{0} for your {1} × {2} screen", L"{0} para tu pantalla de {1} × {2}", L"{0} para sua tela de {1} × {2}",
     L"{0} pour votre écran {1} × {2}", L"{0} für deinen Bildschirm mit {1} × {2}", L"{0} per il tuo schermo {1} × {2}"},
    {L"Pick a setting to see what it does.", L"Elige un ajuste para ver qué hace.", L"Escolha uma opção para ver o que ela faz.",
     L"Choisissez un paramètre pour voir ce qu'il fait.", L"Wähle eine Einstellung, um zu sehen, was sie tut.",
     L"Scegli un'impostazione per vedere cosa fa."},
    {L"Saved in hp2native.ini in the game folder. In the game, F1 changes them while you play.",
     L"Se guardan en hp2native.ini, en la carpeta del juego. En el juego, F1 los cambia mientras juegas.",
     L"Ficam salvas em hp2native.ini, na pasta do jogo. No jogo, F1 altera tudo enquanto você joga.",
     L"Enregistrés dans hp2native.ini, dans le dossier du jeu. En jeu, F1 les modifie pendant que vous jouez.",
     L"Gespeichert in hp2native.ini im Spielordner. Im Spiel änderst du sie mit F1 während des Spielens.",
     L"Salvate in hp2native.ini nella cartella del gioco. In gioco, F1 le cambia mentre giochi."},
    {L"Renders the game at a multiple of the PS2's resolution and averages it down to your screen (supersampling): "
     L"higher is sharper and smoother, and needs a faster graphics card. If the game stutters, choose a lower one.",
     L"Dibuja el juego a un múltiplo de la resolución de la PS2 y lo reduce a tu pantalla (supermuestreo): más alto es más "
     L"nítido y suave, y pide una tarjeta gráfica más rápida. Si el juego va a tirones, elige una más baja.",
     L"Desenha o jogo num múltiplo da resolução do PS2 e reduz para a sua tela (supersampling): mais alto é mais nítido e "
     L"suave, e exige uma placa de vídeo mais rápida. Se o jogo engasgar, escolha uma menor.",
     L"Affiche le jeu à un multiple de la résolution de la PS2 puis le réduit à votre écran (suréchantillonnage) : plus "
     L"haut est plus net et plus fluide, mais demande une carte graphique plus rapide. Si le jeu saccade, choisissez plus bas.",
     L"Berechnet das Spiel in einem Vielfachen der PS2-Auflösung und rechnet es auf deinen Bildschirm herunter "
     L"(Supersampling): höher ist schärfer und ruhiger, braucht aber eine schnellere Grafikkarte. Ruckelt das Spiel, wähl weniger.",
     L"Disegna il gioco a un multiplo della risoluzione PS2 e lo riduce al tuo schermo (supersampling): più alto è più "
     L"nitido e fluido, ma richiede una scheda video più veloce. Se il gioco scatta, scegli un valore più basso."},
    {L"60 doubles the PlayStation 2's 30 frames per second in races, with the game at its normal speed. 30 is the original.",
     L"60 duplica los 30 fotogramas por segundo de la PlayStation 2 en carrera, con el juego a su velocidad normal. 30 es el original.",
     L"60 dobra os 30 quadros por segundo do PlayStation 2 nas corridas, com o jogo na velocidade normal. 30 é o original.",
     L"60 double les 30 images par seconde de la PlayStation 2 en course, le jeu gardant sa vitesse normale. 30 est l'original.",
     L"60 verdoppelt die 30 Bilder pro Sekunde der PlayStation 2 im Rennen, bei normaler Spielgeschwindigkeit. 30 ist das Original.",
     L"60 raddoppia i 30 fotogrammi al secondo della PlayStation 2 in gara, con il gioco alla velocità normale. 30 è l'originale."},
    {L"16:9 shows a wider view in races and keeps the menus' shape. Changing between 16:9 and 4:3 takes effect the next "
     L"time the game starts.",
     L"16:9 muestra una vista más amplia en carrera y respeta la forma de los menús. El cambio entre 16:9 y 4:3 se aplica "
     L"la próxima vez que se inicie el juego.",
     L"16:9 mostra uma visão mais ampla nas corridas e mantém o formato dos menus. A troca entre 16:9 e 4:3 vale a partir "
     L"da próxima vez que o jogo abrir.",
     L"16:9 offre une vue plus large en course et garde la forme des menus. Le passage entre 16:9 et 4:3 s'applique au "
     L"prochain lancement du jeu.",
     L"16:9 zeigt im Rennen mehr von der Strecke und lässt die Menüs in Form. Ein Wechsel zwischen 16:9 und 4:3 gilt ab dem "
     L"nächsten Spielstart.",
     L"16:9 mostra una visuale più ampia in gara e mantiene la forma dei menu. Il passaggio tra 16:9 e 4:3 vale dal prossimo "
     L"avvio del gioco."},
    {L"Full screen covers the whole monitor. In the game, Alt+Enter or F11 switches at any time.",
     L"Pantalla completa ocupa todo el monitor. En el juego, Alt+Intro o F11 cambia en cualquier momento.",
     L"Tela cheia ocupa o monitor inteiro. No jogo, Alt+Enter ou F11 alterna a qualquer momento.",
     L"Le plein écran couvre tout le moniteur. En jeu, Alt+Entrée ou F11 bascule à tout moment.",
     L"Vollbild füllt den ganzen Monitor. Im Spiel schaltet Alt+Enter oder F11 jederzeit um.",
     L"Schermo intero occupa tutto il monitor. In gioco, Alt+Invio o F11 cambia in qualsiasi momento."},
    {L"On: steady motion without tearing. Off: slightly lower delay; the picture may tear.",
     L"Activado: movimiento estable sin cortes. Desactivado: algo menos de retraso; la imagen puede cortarse.",
     L"Ligado: movimento estável, sem cortes. Desligado: um pouco menos de atraso; a imagem pode rasgar.",
     L"Activé : mouvement stable sans déchirure. Désactivé : un peu moins de latence ; l'image peut se déchirer.",
     L"An: ruhige Bewegung ohne Tearing. Aus: etwas weniger Verzögerung; das Bild kann zerreißen.",
     L"Attivo: movimento stabile senza strappi. Disattivo: un po' meno ritardo; l'immagine può strapparsi."},
    {L"How the picture is scaled to your screen. Sharp bilinear keeps it crisp without jagged edges.",
     L"Cómo se escala la imagen a tu pantalla. Bilineal nítido la mantiene definida sin bordes dentados.",
     L"Como a imagem é ajustada à sua tela. Bilinear nítido mantém tudo definido sem serrilhado.",
     L"Comment l'image est mise à l'échelle de votre écran. Bilinéaire net la garde précise sans escaliers.",
     L"Wie das Bild auf deinen Bildschirm skaliert wird. Scharf bilinear hält es klar ohne Treppenkanten.",
     L"Come l'immagine viene adattata allo schermo. Bilineare nitido la mantiene definita senza scalettature."},
    {L"Adds contrast to edges. Off or Low suits most screens; higher values can make edges shimmer.",
     L"Añade contraste a los bordes. Desactivado o Bajo va bien en casi todas las pantallas; más alto puede hacer que los bordes parpadeen.",
     L"Adiciona contraste às bordas. Desligado ou Baixo serve para quase todas as telas; valores maiores podem fazer as bordas tremerem.",
     L"Ajoute du contraste aux contours. Désactivé ou Faible convient à la plupart des écrans ; plus haut peut faire scintiller les bords.",
     L"Verstärkt den Kontrast an Kanten. Aus oder Niedrig passt zu den meisten Bildschirmen; höhere Werte können Kanten flimmern lassen.",
     L"Aggiunge contrasto ai bordi. Disattivo o Basso va bene per quasi tutti gli schermi; valori più alti possono far tremolare i bordi."},
    {L"Darkens alternate lines like a CRT television.", L"Oscurece líneas alternas como un televisor de tubo.",
     L"Escurece linhas alternadas como uma TV de tubo.", L"Assombrit une ligne sur deux comme un téléviseur cathodique.",
     L"Dunkelt jede zweite Zeile ab wie ein Röhrenfernseher.", L"Scurisce le righe alterne come un televisore a tubo catodico."},
    {L"GPU draws the game with your graphics card (fast, with every picture option). Software is a slower reference kept "
     L"for comparison.",
     L"GPU dibuja el juego con tu tarjeta gráfica (rápido, con todas las opciones de imagen). Software es una referencia "
     L"más lenta que se conserva para comparar.",
     L"GPU desenha o jogo com a sua placa de vídeo (rápido, com todas as opções de imagem). Software é uma referência mais "
     L"lenta mantida para comparação.",
     L"GPU dessine le jeu avec votre carte graphique (rapide, avec toutes les options d'image). Logiciel est une référence "
     L"plus lente conservée pour comparer.",
     L"GPU zeichnet das Spiel mit deiner Grafikkarte (schnell, mit allen Bildoptionen). Software ist eine langsamere "
     L"Referenz zum Vergleichen.",
     L"GPU disegna il gioco con la tua scheda video (veloce, con tutte le opzioni d'immagine). Software è un riferimento "
     L"più lento tenuto per confronto."},
    {L"What happens when you quit the game: come back to this launcher, or close it too.",
     L"Qué pasa al salir del juego: volver a este launcher o cerrarlo también.",
     L"O que acontece ao sair do jogo: voltar a este launcher ou fechá-lo também.",
     L"Ce qui se passe quand vous quittez le jeu : revenir à ce lanceur, ou le fermer aussi.",
     L"Was nach dem Beenden des Spiels passiert: zurück zu diesem Launcher oder ihn auch schließen.",
     L"Cosa succede quando esci dal gioco: tornare a questo launcher o chiudere anche lui."},
    {L"The language of the launcher and of the in-game settings menu. The game's own text stays as it is on your disc.",
     L"El idioma del launcher y del menú de ajustes del juego. Los textos propios del juego quedan como están en tu disco.",
     L"O idioma do launcher e do menu de configurações do jogo. Os textos do próprio jogo ficam como estão no seu disco.",
     L"La langue du lanceur et du menu des paramètres en jeu. Les textes du jeu lui-même restent tels qu'ils sont sur votre disque.",
     L"Die Sprache des Launchers und des Einstellungsmenüs im Spiel. Die Texte des Spiels selbst bleiben so wie auf deiner Disc.",
     L"La lingua del launcher e del menu impostazioni in gioco. I testi del gioco restano come sono sul tuo disco."},
    // setting names and values (shared with the in-game menu)
    {L"Renderer", L"Renderizador", L"Renderizador", L"Rendu", L"Renderer", L"Renderer"},
    {L"Internal resolution", L"Resolución interna", L"Resolução interna", L"Résolution interne", L"Interne Auflösung", L"Risoluzione interna"},
    {L"Frame rate", L"Fotogramas por segundo", L"Taxa de quadros", L"Images par seconde", L"Bildrate", L"Frame rate"},
    {L"Display mode", L"Modo de pantalla", L"Modo de exibição", L"Mode d'affichage", L"Anzeigemodus", L"Modalità schermo"},
    {L"V-Sync", L"Sincronización vertical", L"V-Sync", L"Synchro verticale", L"V-Sync", L"V-Sync"},
    {L"Scaling filter", L"Filtro de escalado", L"Filtro de escala", L"Filtre de mise à l'échelle", L"Skalierungsfilter", L"Filtro di scala"},
    {L"Aspect ratio", L"Relación de aspecto", L"Proporção", L"Format d'image", L"Seitenverhältnis", L"Formato"},
    {L"Sharpening", L"Nitidez", L"Nitidez", L"Netteté", L"Schärfe", L"Nitidezza"},
    {L"Scanlines", L"Líneas de barrido", L"Scanlines", L"Lignes de balayage", L"Scanlines", L"Scanline"},
    {L"Controller layout", L"Esquema del mando", L"Layout do controle", L"Disposition manette", L"Controller-Belegung", L"Configurazione controller"},
    {L"Language", L"Idioma", L"Idioma", L"Langue", L"Sprache", L"Lingua"},
    {L"Software", L"Software", L"Software", L"Logiciel", L"Software", L"Software"},
    {L"1x (native)", L"1x (nativa)", L"1x (nativa)", L"1x (native)", L"1x (nativ)", L"1x (nativa)"},
    {L"30 (original)", L"30 (original)", L"30 (original)", L"30 (original)", L"30 (Original)", L"30 (originale)"},
    {L"Window", L"Ventana", L"Janela", L"Fenêtre", L"Fenster", L"Finestra"},
    {L"Off", L"Desactivado", L"Desligado", L"Désactivé", L"Aus", L"Disattivo"},
    {L"On", L"Activado", L"Ligado", L"Activé", L"An", L"Attivo"},
    {L"Sharp bilinear", L"Bilineal nítido", L"Bilinear nítido", L"Bilinéaire net", L"Scharf bilinear", L"Bilineare nitido"},
    {L"Bilinear", L"Bilineal", L"Bilinear", L"Bilinéaire", L"Bilinear", L"Bilineare"},
    {L"Nearest", L"Vecino más próximo", L"Vizinho mais próximo", L"Plus proche voisin", L"Nächster Nachbar", L"Vicino più prossimo"},
    {L"4:3 integer scale", L"4:3 escala entera", L"4:3 escala inteira", L"4:3 échelle entière", L"4:3 ganzzahlig", L"4:3 scala intera"},
    {L"Stretch", L"Estirar", L"Esticar", L"Étirer", L"Strecken", L"Allunga"},
    {L"Low", L"Bajo", L"Baixo", L"Faible", L"Niedrig", L"Basso"},
    {L"Medium", L"Medio", L"Médio", L"Moyen", L"Mittel", L"Medio"},
    {L"High", L"Alto", L"Alto", L"Élevé", L"Hoch", L"Alto"},
    {L"Light", L"Suave", L"Leve", L"Léger", L"Leicht", L"Leggero"},
    {L"Strong", L"Fuerte", L"Forte", L"Fort", L"Stark", L"Forte"},
    {L"Classic (PS2)", L"Clásico (PS2)", L"Clássico (PS2)", L"Classique (PS2)", L"Klassisch (PS2)", L"Classica (PS2)"},
    {L"Modern (triggers)", L"Moderno (gatillos)", L"Moderno (gatilhos)", L"Moderne (gâchettes)", L"Modern (Trigger)", L"Moderna (grilletti)"},
    {L"Custom", L"Personalizado", L"Personalizado", L"Personnalisé", L"Eigene", L"Personalizzata"},
    {L"Default", L"Predeterminado", L"Padrão", L"Par défaut", L"Standard", L"Predefinita"},
    {L"Original (1x)", L"Original (1x)", L"Original (1x)", L"Original (1x)", L"Original (1x)", L"Originale (1x)"},
    {L"Performance (2x)", L"Rendimiento (2x)", L"Desempenho (2x)", L"Performance (2x)", L"Leistung (2x)", L"Prestazioni (2x)"},
    {L"Balanced (3x)", L"Equilibrado (3x)", L"Equilibrado (3x)", L"Équilibré (3x)", L"Ausgewogen (3x)", L"Bilanciata (3x)"},
    {L"Quality (4x)", L"Calidad (4x)", L"Qualidade (4x)", L"Qualité (4x)", L"Qualität (4x)", L"Qualità (4x)"},
    {L"Ultra (6x)", L"Ultra (6x)", L"Ultra (6x)", L"Ultra (6x)", L"Ultra (6x)", L"Ultra (6x)"},
    {L"Maximum (8x)", L"Máximo (8x)", L"Máximo (8x)", L"Maximum (8x)", L"Maximum (8x)", L"Massima (8x)"},
    // ---- in-game settings menu ----
    {L"Quit Game", L"Salir del juego", L"Sair do jogo", L"Quitter le jeu", L"Spiel beenden", L"Esci dal gioco"},
    {L"Exit to Windows", L"Volver a Windows", L"Voltar ao Windows", L"Retour à Windows", L"Zurück zu Windows", L"Torna a Windows"},
    {L"Press again to quit", L"Pulsa otra vez para salir", L"Aperte de novo para sair", L"Appuyez encore pour quitter",
     L"Nochmal drücken zum Beenden", L"Premi di nuovo per uscire"},
    {L"(restart)", L"(reiniciar)", L"(reiniciar)", L"(redémarrer)", L"(Neustart)", L"(riavvio)"},
    {L"↑↓ Select    ←→ / Enter / ✕ Change    F1 / Esc / Select+Start Close",
     L"↑↓ Elegir    ←→ / Intro / ✕ Cambiar    F1 / Esc / Select+Start Cerrar",
     L"↑↓ Selecionar    ←→ / Enter / ✕ Alterar    F1 / Esc / Select+Start Fechar",
     L"↑↓ Choisir    ←→ / Entrée / ✕ Modifier    F1 / Échap / Select+Start Fermer",
     L"↑↓ Auswählen    ←→ / Enter / ✕ Ändern    F1 / Esc / Select+Start Schließen",
     L"↑↓ Seleziona    ←→ / Invio / ✕ Cambia    F1 / Esc / Select+Start Chiudi"},
    // ---- Controls page ----
    {L"Controller", L"Mando", L"Controle", L"Manette", L"Controller", L"Controller"},
    {L"Keyboard", L"Teclado", L"Teclado", L"Clavier", L"Tastatur", L"Tastiera"},
    {L"Action (the game's Configuration A)", L"Acción (configuración A del juego)", L"Ação (configuração A do jogo)",
     L"Action (configuration A du jeu)", L"Aktion (Konfiguration A des Spiels)", L"Azione (configurazione A del gioco)"},
    {L"Key", L"Tecla", L"Tecla", L"Touche", L"Taste", L"Tasto"},
    {L"Key 2", L"Tecla 2", L"Tecla 2", L"Touche 2", L"Taste 2", L"Tasto 2"},
    {L"Controller 2", L"Mando 2", L"Controle 2", L"Manette 2", L"Controller 2", L"Controller 2"},
    {L"press a button", L"pulsa un botón", L"aperte um botão", L"appuyez sur un bouton", L"Knopf drücken", L"premi un pulsante"},
    {L"press a key", L"pulsa una tecla", L"aperte uma tecla", L"appuyez sur une touche", L"Taste drücken", L"premi un tasto"},
    {L"Restore Default Keyboard", L"Restaurar teclado original", L"Restaurar teclado padrão", L"Rétablir le clavier par défaut",
     L"Standard-Tastatur wiederherstellen", L"Ripristina tastiera predefinita"},
    {L"Restore Default Controller", L"Restaurar mando original", L"Restaurar controle padrão", L"Rétablir la manette par défaut",
     L"Standard-Controller wiederherstellen", L"Ripristina controller predefinito"},
    {L"Accelerate", L"Acelerar", L"Acelerar", L"Accélérer", L"Gas geben", L"Accelera"},
    {L"Brake / Reverse", L"Frenar / Marcha atrás", L"Frear / Ré", L"Freiner / Marche arrière", L"Bremsen / Rückwärts", L"Frena / Retromarcia"},
    {L"Steer left", L"Girar a la izquierda", L"Virar à esquerda", L"Tourner à gauche", L"Nach links lenken", L"Sterza a sinistra"},
    {L"Steer right", L"Girar a la derecha", L"Virar à direita", L"Tourner à droite", L"Nach rechts lenken", L"Sterza a destra"},
    {L"Handbrake", L"Freno de mano", L"Freio de mão", L"Frein à main", L"Handbremse", L"Freno a mano"},
    {L"Horn / Siren", L"Claxon / Sirena", L"Buzina / Sirene", L"Klaxon / Sirène", L"Hupe / Sirene", L"Clacson / Sirena"},
    {L"Change view", L"Cambiar vista", L"Mudar câmera", L"Changer de vue", L"Ansicht wechseln", L"Cambia visuale"},
    {L"Look back", L"Mirar atrás", L"Olhar para trás", L"Regarder derrière", L"Zurückschauen", L"Guarda indietro"},
    {L"Speed boost", L"Impulso de velocidad", L"Turbo", L"Boost de vitesse", L"Speed-Boost", L"Boost di velocità"},
    {L"Shift up / Zone Zoom", L"Subir marcha / Zone Zoom", L"Subir marcha / Zone Zoom", L"Rapport sup. / Zone Zoom",
     L"Hochschalten / Zone Zoom", L"Marcia su / Zone Zoom"},
    {L"Shift down / Zone Freeze", L"Bajar marcha / Zone Freeze", L"Reduzir marcha / Zone Freeze", L"Rapport inf. / Zone Freeze",
     L"Runterschalten / Zone Freeze", L"Marcia giù / Zone Freeze"},
    {L"Pause", L"Pausa", L"Pausa", L"Pause", L"Pause", L"Pausa"},
    {L"Cycle HUD", L"Cambiar HUD", L"Alternar HUD", L"Changer de HUD", L"HUD wechseln", L"Cambia HUD"},
    {L"Menu up", L"Menú arriba", L"Menu acima", L"Menu haut", L"Menü hoch", L"Menu su"},
    {L"Menu down", L"Menú abajo", L"Menu abaixo", L"Menu bas", L"Menü runter", L"Menu giù"},
    // ---- Memory card page ----
    {L"On the Card", L"En la tarjeta", L"No cartão", L"Sur la carte", L"Auf der Karte", L"Sulla scheda"},
    {L"Nothing yet. Saves show up here once the game has saved (career progress, settings).",
     L"Nada todavía. Las partidas aparecen aquí cuando el juego guarda (progreso, ajustes).",
     L"Nada ainda. Os saves aparecem aqui quando o jogo salvar (progresso, configurações).",
     L"Rien pour l'instant. Les sauvegardes apparaissent ici dès que le jeu a sauvegardé (progression, paramètres).",
     L"Noch nichts. Spielstände erscheinen hier, sobald das Spiel gespeichert hat (Karriere, Einstellungen).",
     L"Ancora niente. I salvataggi compaiono qui quando il gioco salva (carriera, impostazioni)."},
    {L"Back Up Now", L"Copiar ahora", L"Fazer backup", L"Sauvegarder", L"Jetzt sichern", L"Backup ora"},
    {L"Open Folder", L"Abrir carpeta", L"Abrir pasta", L"Ouvrir le dossier", L"Ordner öffnen", L"Apri cartella"},
    {L"Backups", L"Copias de seguridad", L"Backups", L"Sauvegardes", L"Sicherungen", L"Backup"},
    {L"No backups yet. A backup is a copy of the whole card, and restoring one keeps the current card as a backup too - so "
     L"nothing gets lost.",
     L"Aún no hay copias. Una copia es la tarjeta entera, y al restaurar una la tarjeta actual también se guarda como copia: "
     L"no se pierde nada.",
     L"Nenhum backup ainda. Um backup é uma cópia do cartão inteiro, e restaurar um guarda o cartão atual como backup também "
     L"– nada se perde.",
     L"Aucune sauvegarde pour l'instant. Une sauvegarde copie toute la carte, et en restaurer une conserve aussi la carte "
     L"actuelle – rien ne se perd.",
     L"Noch keine Sicherungen. Eine Sicherung kopiert die ganze Karte, und beim Wiederherstellen wird die aktuelle Karte "
     L"ebenfalls gesichert – nichts geht verloren.",
     L"Nessun backup ancora. Un backup copia l'intera scheda, e ripristinarne uno salva anche la scheda attuale – non si "
     L"perde nulla."},
    {L"Press again to restore", L"Pulsa otra vez para restaurar", L"Aperte de novo para restaurar", L"Appuyez encore pour restaurer",
     L"Nochmal drücken zum Wiederherstellen", L"Premi di nuovo per ripristinare"},
    {L"Restore", L"Restaurar", L"Restaurar", L"Restaurer", L"Wiederherstellen", L"Ripristina"},
    {L"Backup saved: {0}", L"Copia guardada: {0}", L"Backup salvo: {0}", L"Sauvegarde enregistrée : {0}", L"Sicherung gespeichert: {0}",
     L"Backup salvato: {0}"},
    {L"The memory card is empty: nothing to back up.", L"La tarjeta está vacía: no hay nada que copiar.",
     L"O cartão está vazio: nada para salvar.", L"La carte est vide : rien à sauvegarder.", L"Die Karte ist leer: nichts zu sichern.",
     L"La scheda è vuota: niente da salvare."},
    {L"Restored the backup of {0}", L"Restaurada la copia del {0}", L"Backup de {0} restaurado", L"Sauvegarde du {0} restaurée",
     L"Sicherung vom {0} wiederhergestellt", L"Ripristinato il backup del {0}"},
    {L"That backup is empty.", L"Esa copia está vacía.", L"Esse backup está vazio.", L"Cette sauvegarde est vide.", L"Diese Sicherung ist leer.",
     L"Quel backup è vuoto."},
    // ---- Screenshots page ----
    {L"No screenshots yet", L"Aún no hay capturas", L"Nenhuma captura ainda", L"Pas encore de captures", L"Noch keine Screenshots",
     L"Ancora nessuno screenshot"},
    {L"Press F12 while you play. Your newest one also shows up on the Play page.",
     L"Pulsa F12 mientras juegas. La más reciente también aparece en la página Jugar.",
     L"Aperte F12 enquanto joga. A mais recente também aparece na página Jogar.",
     L"Appuyez sur F12 pendant que vous jouez. La plus récente s'affiche aussi sur la page Jouer.",
     L"Drück beim Spielen F12. Der neueste erscheint auch auf der Seite Spielen.",
     L"Premi F12 mentre giochi. Il più recente compare anche nella pagina Gioca."},
    {L"Taken with F12 in the game.", L"Hechas con F12 en el juego.", L"Feitas com F12 no jogo.", L"Prises avec F12 en jeu.",
     L"Mit F12 im Spiel aufgenommen.", L"Scattati con F12 in gioco."},
    {L"Row {0}-{1} of {2}", L"Fila {0}-{1} de {2}", L"Linha {0}-{1} de {2}", L"Rangée {0}-{1} sur {2}", L"Reihe {0}-{1} von {2}",
     L"Riga {0}-{1} di {2}"},
    {L"{0}   ·   {1} of {2}   ·   ← → browse   ·   Esc close", L"{0}   ·   {1} de {2}   ·   ← → navegar   ·   Esc cerrar",
     L"{0}   ·   {1} de {2}   ·   ← → navegar   ·   Esc fechar", L"{0}   ·   {1} sur {2}   ·   ← → parcourir   ·   Échap fermer",
     L"{0}   ·   {1} von {2}   ·   ← → blättern   ·   Esc schließen", L"{0}   ·   {1} di {2}   ·   ← → sfoglia   ·   Esc chiudi"},
    // ---- progress ----
    {L"{0} of {1}  ·  {2} MB/s", L"{0} de {1}  ·  {2} MB/s", L"{0} de {1}  ·  {2} MB/s", L"{0} sur {1}  ·  {2} Mo/s",
     L"{0} von {1}  ·  {2} MB/s", L"{0} di {1}  ·  {2} MB/s"},
    {L"about {0} left", L"quedan unos {0}", L"faltam cerca de {0}", L"environ {0} restantes", L"noch etwa {0}", L"circa {0} rimasti"},
    {L"Copying {0}", L"Copiando {0}", L"Copiando {0}", L"Copie de {0}", L"Kopiere {0}", L"Copia di {0}"},
    {L"Checking {0}", L"Comprobando {0}", L"Verificando {0}", L"Vérification de {0}", L"Prüfe {0}", L"Controllo di {0}"},
    {L"Reading the game's program", L"Leyendo el programa del juego", L"Lendo o programa do jogo", L"Lecture du programme du jeu",
     L"Lese das Spielprogramm", L"Lettura del programma del gioco"},
    {L"Translating the game's program", L"Traduciendo el programa del juego", L"Traduzindo o programa do jogo",
     L"Traduction du programme du jeu", L"Übersetze das Spielprogramm", L"Traduzione del programma del gioco"},
    {L"Compiling the game's code for your processor", L"Compilando el código del juego para tu procesador",
     L"Compilando o código do jogo para o seu processador", L"Compilation du code du jeu pour votre processeur",
     L"Kompiliere den Spielcode für deinen Prozessor", L"Compilazione del codice del gioco per il tuo processore"},
    {L"Linking", L"Enlazando", L"Vinculando", L"Édition des liens", L"Verlinke", L"Collegamento"},
    // ---- times ----
    {L"just now", L"hace un momento", L"agora mesmo", L"à l'instant", L"gerade eben", L"poco fa"},
    {L"{0} min ago", L"hace {0} min", L"há {0} min", L"il y a {0} min", L"vor {0} Min.", L"{0} min fa"},
    {L"{0} h ago", L"hace {0} h", L"há {0} h", L"il y a {0} h", L"vor {0} Std.", L"{0} h fa"},
    {L"yesterday", L"ayer", L"ontem", L"hier", L"gestern", L"ieri"},
    {L"{0} days ago", L"hace {0} días", L"há {0} dias", L"il y a {0} jours", L"vor {0} Tagen", L"{0} giorni fa"},
    {L"a month ago", L"hace un mes", L"há um mês", L"il y a un mois", L"vor einem Monat", L"un mese fa"},
    {L"{0} months ago", L"hace {0} meses", L"há {0} meses", L"il y a {0} mois", L"vor {0} Monaten", L"{0} mesi fa"},
    {L"less than a minute", L"menos de un minuto", L"menos de um minuto", L"moins d'une minute", L"weniger als eine Minute",
     L"meno di un minuto"},
    {L"{0} h {1} min", L"{0} h {1} min", L"{0} h {1} min", L"{0} h {1} min", L"{0} Std. {1} Min.", L"{0} h {1} min"},
    {L"{0} min", L"{0} min", L"{0} min", L"{0} min", L"{0} Min.", L"{0} min"},
    // ---- Disc & setup page ----
    {L"Disc Folder", L"Carpeta del disco", L"Pasta do disco", L"Dossier du disque", L"Disc-Ordner", L"Cartella del disco"},
    {L"Edition", L"Edición", L"Edição", L"Édition", L"Ausgabe", L"Edizione"},
    {L"Disc Files", L"Archivos del disco", L"Arquivos do disco", L"Fichiers du disque", L"Disc-Dateien", L"File del disco"},
    {L"Game", L"Juego", L"Jogo", L"Jeu", L"Spiel", L"Gioco"},
    {L"Recompiler", L"Recompilador", L"Recompilador", L"Recompilateur", L"Recompiler", L"Ricompilatore"},
    {L"(none)", L"(ninguna)", L"(nenhuma)", L"(aucun)", L"(keiner)", L"(nessuna)"},
    {L"North America, SLUS-20362 (supported)", L"Norteamérica, SLUS-20362 (compatible)", L"América do Norte, SLUS-20362 (compatível)",
     L"Amérique du Nord, SLUS-20362 (pris en charge)", L"Nordamerika, SLUS-20362 (unterstützt)", L"Nord America, SLUS-20362 (supportata)"},
    {L"All files match the original disc (checked {0})", L"Todos los archivos coinciden con el original (comprobado {0})",
     L"Todos os arquivos batem com o original (verificado {0})", L"Tous les fichiers correspondent à l'original (vérifié {0})",
     L"Alle Dateien stimmen mit dem Original überein (geprüft {0})", L"Tutti i file corrispondono all'originale (verificato {0})"},
    {L"Not checked file by file yet", L"Aún no comprobado archivo por archivo", L"Ainda não verificado arquivo por arquivo",
     L"Pas encore vérifié fichier par fichier", L"Noch nicht Datei für Datei geprüft", L"Non ancora verificato file per file"},
    {L"Prepared for this version", L"Preparado para esta versión", L"Preparado para esta versão", L"Préparé pour cette version",
     L"Für diese Version vorbereitet", L"Preparato per questa versione"},
    {L"Checking...", L"Comprobando...", L"Verificando...", L"Vérification...", L"Prüfe...", L"Controllo..."},
    {L"Made by another version: prepare again", L"Hecho por otra versión: preparar de nuevo", L"Feito por outra versão: preparar de novo",
     L"Fait par une autre version : préparer à nouveau", L"Von einer anderen Version: neu vorbereiten", L"Fatto da un'altra versione: prepara di nuovo"},
    {L"hp2recomp.exe not found", L"No se encuentra hp2recomp.exe", L"hp2recomp.exe não encontrado", L"hp2recomp.exe introuvable",
     L"hp2recomp.exe nicht gefunden", L"hp2recomp.exe non trovato"},
    {L"Cancel", L"Cancelar", L"Cancelar", L"Annuler", L"Abbrechen", L"Annulla"},
    {L"Check All Files", L"Comprobar archivos", L"Verificar arquivos", L"Vérifier les fichiers", L"Alle Dateien prüfen", L"Verifica i file"},
    {L"Prepare Again", L"Preparar de nuevo", L"Preparar de novo", L"Préparer à nouveau", L"Neu vorbereiten", L"Prepara di nuovo"},
    {L"Change Disc...", L"Cambiar disco...", L"Trocar disco...", L"Changer de disque...", L"Disc wechseln...", L"Cambia disco..."},
    {L"Open Game Folder", L"Abrir carpeta del juego", L"Abrir pasta do jogo", L"Ouvrir le dossier du jeu", L"Spielordner öffnen",
     L"Apri cartella del gioco"},
    {L"Desktop Shortcut", L"Acceso directo", L"Atalho na área de trabalho", L"Raccourci sur le bureau", L"Desktop-Verknüpfung",
     L"Collegamento sul desktop"},
    {L"Your disc is only ever read. Nothing is downloaded and nothing leaves this PC.",
     L"Tu disco solo se lee. No se descarga nada y nada sale de este PC.", L"Seu disco só é lido. Nada é baixado e nada sai deste PC.",
     L"Votre disque est seulement lu. Rien n'est téléchargé et rien ne quitte ce PC.",
     L"Deine Disc wird nur gelesen. Nichts wird heruntergeladen, nichts verlässt diesen PC.",
     L"Il tuo disco viene solo letto. Non si scarica nulla e nulla lascia questo PC."},
    // ---- About page ----
    {L"What this is", L"Qué es esto", L"O que é isto", L"De quoi s'agit-il", L"Was das ist", L"Cos'è"},
    {L"Made with AI", L"Hecho con IA", L"Feito com IA", L"Fait avec l'IA", L"Mit KI gemacht", L"Fatto con l'IA"},
    {L"Credits", L"Créditos", L"Créditos", L"Crédits", L"Credits", L"Crediti"},
    {L"Legal", L"Aviso legal", L"Aviso legal", L"Mentions légales", L"Rechtliches", L"Note legali"},
    {L"HotPursuit2PC runs Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) natively on Windows. The game's program on "
     L"your own disc is translated into PC code right on your computer: no emulator, and no game files come with it.",
     L"HotPursuit2PC ejecuta Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) de forma nativa en Windows. El programa del "
     L"juego de tu propio disco se traduce a código de PC en tu ordenador: sin emulador y sin archivos del juego incluidos.",
     L"HotPursuit2PC roda Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) nativamente no Windows. O programa do jogo do "
     L"seu próprio disco é traduzido para código de PC no seu computador: sem emulador e sem arquivos do jogo inclusos.",
     L"HotPursuit2PC fait tourner Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) nativement sous Windows. Le programme du "
     L"jeu de votre propre disque est traduit en code PC sur votre ordinateur : pas d'émulateur, et aucun fichier du jeu fourni.",
     L"HotPursuit2PC lässt Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) nativ unter Windows laufen. Das Spielprogramm "
     L"deiner eigenen Disc wird direkt auf deinem PC in PC-Code übersetzt: kein Emulator, und keine Spieldateien dabei.",
     L"HotPursuit2PC fa girare Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) nativamente su Windows. Il programma del "
     L"gioco dal tuo disco viene tradotto in codice PC sul tuo computer: niente emulatore e nessun file del gioco incluso."},
    {L"I started this project to get hands-on with building software together with AI. Almost all of the code was written "
     L"by an AI coding agent (Claude, in Claude Code) under my direction. AI isn't going away - it's a tool, and a pretty "
     L"good one for bringing the games of my childhood to the PC. More in the project's README.",
     L"Empecé este proyecto para aprender a crear software junto a una IA. Casi todo el código lo escribió un agente de "
     L"programación con IA (Claude, en Claude Code) bajo mi dirección. La IA no se va a ir: es una herramienta, y bastante "
     L"buena para traer los juegos de mi infancia al PC. Más en el README del proyecto.",
     L"Comecei este projeto para aprender a criar software junto com uma IA. Quase todo o código foi escrito por um agente de "
     L"programação com IA (Claude, no Claude Code) sob minha direção. A IA não vai embora: é uma ferramenta, e das boas para "
     L"trazer os jogos da minha infância para o PC. Mais no README do projeto.",
     L"J'ai lancé ce projet pour apprendre à créer des logiciels avec une IA. Presque tout le code a été écrit par un agent "
     L"de programmation IA (Claude, dans Claude Code) sous ma direction. L'IA ne va pas disparaître : c'est un outil, et un "
     L"plutôt bon pour amener les jeux de mon enfance sur PC. Plus dans le README du projet.",
     L"Ich habe dieses Projekt gestartet, um Software zusammen mit KI zu bauen. Fast der ganze Code stammt von einem "
     L"KI-Programmieragenten (Claude, in Claude Code) unter meiner Leitung. KI verschwindet nicht – sie ist ein Werkzeug, und "
     L"ein ziemlich gutes, um die Spiele meiner Kindheit auf den PC zu bringen. Mehr im README des Projekts.",
     L"Ho iniziato questo progetto per imparare a creare software insieme a un'IA. Quasi tutto il codice è stato scritto da "
     L"un agente di programmazione IA (Claude, in Claude Code) sotto la mia direzione. L'IA non se ne andrà: è uno strumento, "
     L"e anche ottimo per portare i giochi della mia infanzia su PC. Altro nel README del progetto."},
    {L"Widescreen and 60 fps values from the PCSX2 community patches by nemesis2000, Aero_ and asasega - thank you! Code "
     L"generation by LLVM (Apache 2.0 with LLVM exceptions). The Play! emulator (BSD 2-Clause) was a reference during "
     L"development; none of it is in the native game.",
     L"Valores de pantalla panorámica y 60 fps de los parches de la comunidad de PCSX2 de nemesis2000, Aero_ y asasega: ¡gracias! "
     L"Generación de código con LLVM (Apache 2.0 con excepciones LLVM). El emulador Play! (BSD 2-Clause) sirvió de referencia "
     L"durante el desarrollo; nada de él está en el juego nativo.",
     L"Valores de widescreen e 60 fps dos patches da comunidade PCSX2 de nemesis2000, Aero_ e asasega – obrigado! Geração de "
     L"código pelo LLVM (Apache 2.0 com exceções LLVM). O emulador Play! (BSD 2-Clause) foi referência durante o "
     L"desenvolvimento; nada dele está no jogo nativo.",
     L"Valeurs écran large et 60 fps issues des patchs de la communauté PCSX2 de nemesis2000, Aero_ et asasega – merci ! "
     L"Génération de code par LLVM (Apache 2.0 avec exceptions LLVM). L'émulateur Play! (BSD 2-Clause) a servi de référence "
     L"pendant le développement ; rien de lui n'est dans le jeu natif.",
     L"Breitbild- und 60-fps-Werte aus den PCSX2-Community-Patches von nemesis2000, Aero_ und asasega – danke! Codeerzeugung "
     L"mit LLVM (Apache 2.0 mit LLVM-Ausnahmen). Der Emulator Play! (BSD 2-Clause) war während der Entwicklung eine Referenz; "
     L"nichts davon steckt im nativen Spiel.",
     L"Valori widescreen e 60 fps dalle patch della community PCSX2 di nemesis2000, Aero_ e asasega – grazie! Generazione del "
     L"codice con LLVM (Apache 2.0 con eccezioni LLVM). L'emulatore Play! (BSD 2-Clause) è stato un riferimento durante lo "
     L"sviluppo; niente di esso è nel gioco nativo."},
    {L"Not affiliated with, sponsored by or endorsed by Electronic Arts or Sony. Need for Speed and Hot Pursuit 2 are "
     L"trademarks of Electronic Arts Inc.; PlayStation is a trademark of Sony Interactive Entertainment.",
     L"Sin relación con Electronic Arts ni Sony, ni patrocinado ni respaldado por ellas. Need for Speed y Hot Pursuit 2 son "
     L"marcas de Electronic Arts Inc.; PlayStation es una marca de Sony Interactive Entertainment.",
     L"Sem vínculo com a Electronic Arts ou a Sony, nem patrocinado ou endossado por elas. Need for Speed e Hot Pursuit 2 são "
     L"marcas da Electronic Arts Inc.; PlayStation é uma marca da Sony Interactive Entertainment.",
     L"Ni affilié à Electronic Arts ou Sony, ni sponsorisé ou approuvé par eux. Need for Speed et Hot Pursuit 2 sont des "
     L"marques d'Electronic Arts Inc. ; PlayStation est une marque de Sony Interactive Entertainment.",
     L"Nicht mit Electronic Arts oder Sony verbunden und nicht von ihnen gesponsert oder unterstützt. Need for Speed und Hot "
     L"Pursuit 2 sind Marken von Electronic Arts Inc.; PlayStation ist eine Marke von Sony Interactive Entertainment.",
     L"Non affiliato, sponsorizzato o approvato da Electronic Arts o Sony. Need for Speed e Hot Pursuit 2 sono marchi di "
     L"Electronic Arts Inc.; PlayStation è un marchio di Sony Interactive Entertainment."},
    {L"Project Page", L"Página del proyecto", L"Página do projeto", L"Page du projet", L"Projektseite", L"Pagina del progetto"},
    {L"Getting Started", L"Primeros pasos", L"Primeiros passos", L"Bien démarrer", L"Erste Schritte", L"Per iniziare"},
    {L"HotPursuit2PC {0}  ·  MIT license", L"HotPursuit2PC {0}  ·  licencia MIT", L"HotPursuit2PC {0}  ·  licença MIT",
     L"HotPursuit2PC {0}  ·  licence MIT", L"HotPursuit2PC {0}  ·  MIT-Lizenz", L"HotPursuit2PC {0}  ·  licenza MIT"},
    // ---- setup ----
    {L"Disc", L"Disco", L"Disco", L"Disque", L"Disc", L"Disco"},
    {L"Check", L"Comprobar", L"Verificar", L"Vérifier", L"Prüfen", L"Verifica"},
    {L"Prepare", L"Preparar", L"Preparar", L"Préparer", L"Vorbereiten", L"Prepara"},
    {L"Ready", L"Listo", L"Pronto", L"Prêt", L"Fertig", L"Pronto"},
    {L"Welcome!", L"¡Hola!", L"Olá!", L"Bienvenue !", L"Willkommen!", L"Ciao!"},
    {L"Let's get Hot Pursuit 2 ready to race on this PC. It takes a few minutes, just once.\n\nYou'll need your own disc of "
     L"the North American release (SLUS-20362) - in a DVD drive, or as an .iso image of your disc. Its files are copied and "
     L"checked, then the game's program is translated for your processor. Nothing is downloaded.",
     L"Vamos a dejar Hot Pursuit 2 listo para correr en este PC. Lleva unos minutos, solo una vez.\n\nNecesitas tu propio "
     L"disco de la edición norteamericana (SLUS-20362), en una unidad de DVD o como imagen .iso de tu disco. Sus archivos se "
     L"copian y se comprueban, y luego el programa del juego se traduce para tu procesador. No se descarga nada.",
     L"Vamos deixar Hot Pursuit 2 pronto para correr neste PC. Leva alguns minutos, só uma vez.\n\nVocê precisa do seu "
     L"próprio disco da edição norte-americana (SLUS-20362), num drive de DVD ou como imagem .iso do seu disco. Os arquivos "
     L"são copiados e verificados, e depois o programa do jogo é traduzido para o seu processador. Nada é baixado.",
     L"Préparons Hot Pursuit 2 pour la course sur ce PC. Cela prend quelques minutes, une seule fois.\n\nIl vous faut votre "
     L"propre disque de l'édition nord-américaine (SLUS-20362), dans un lecteur DVD ou en image .iso de votre disque. Ses "
     L"fichiers sont copiés et vérifiés, puis le programme du jeu est traduit pour votre processeur. Rien n'est téléchargé.",
     L"Machen wir Hot Pursuit 2 auf diesem PC rennbereit. Das dauert ein paar Minuten, nur einmal.\n\nDu brauchst deine "
     L"eigene Disc der nordamerikanischen Ausgabe (SLUS-20362) – im DVD-Laufwerk oder als .iso-Abbild deiner Disc. Ihre "
     L"Dateien werden kopiert und geprüft, dann wird das Spielprogramm für deinen Prozessor übersetzt. Nichts wird heruntergeladen.",
     L"Prepariamo Hot Pursuit 2 per correre su questo PC. Ci vogliono pochi minuti, una volta sola.\n\nTi serve il tuo disco "
     L"dell'edizione nordamericana (SLUS-20362), in un lettore DVD o come immagine .iso del tuo disco. I file vengono copiati "
     L"e verificati, poi il programma del gioco viene tradotto per il tuo processore. Non si scarica nulla."},
    {L"Let's Go", L"¡Vamos!", L"Vamos lá", L"C'est parti", L"Los geht's", L"Andiamo"},
    {L"Your Disc", L"Tu disco", L"Seu disco", L"Votre disque", L"Deine Disc", L"Il tuo disco"},
    {L"Insert your disc, or double-click your .iso file in File Explorer so Windows shows it as a drive. It shows up here by "
     L"itself.",
     L"Mete tu disco, o haz doble clic en tu archivo .iso en el Explorador para que Windows lo muestre como unidad. Aparece "
     L"aquí solo.",
     L"Coloque seu disco ou clique duas vezes no seu arquivo .iso no Explorador para o Windows mostrá-lo como unidade. Ele "
     L"aparece aqui sozinho.",
     L"Insérez votre disque, ou double-cliquez sur votre fichier .iso dans l'Explorateur pour que Windows l'affiche comme un "
     L"lecteur. Il apparaît ici tout seul.",
     L"Leg deine Disc ein oder doppelklicke deine .iso-Datei im Explorer, damit Windows sie als Laufwerk zeigt. Sie "
     L"erscheint hier von selbst.",
     L"Inserisci il disco, o fai doppio clic sul tuo file .iso in Esplora file perché Windows lo mostri come unità. Comparirà "
     L"qui da solo."},
    {L"Copy from {0}", L"Copiar desde {0}", L"Copiar de {0}", L"Copier depuis {0}", L"Kopieren von {0}", L"Copia da {0}"},
    {L"Your disc or a mounted .iso", L"Tu disco o una .iso montada", L"Seu disco ou uma .iso montada", L"Votre disque ou une .iso montée",
     L"Deine Disc oder eine eingebundene .iso", L"Il tuo disco o una .iso montata"},
    {L"A removable drive", L"Una unidad extraíble", L"Uma unidade removível", L"Un lecteur amovible", L"Ein Wechseldatenträger",
     L"Un'unità rimovibile"},
    {L"{0}  ·  copies {1} to the game folder (recommended)", L"{0}  ·  copia {1} a la carpeta del juego (recomendado)",
     L"{0}  ·  copia {1} para a pasta do jogo (recomendado)", L"{0}  ·  copie {1} dans le dossier du jeu (recommandé)",
     L"{0}  ·  kopiert {1} in den Spielordner (empfohlen)", L"{0}  ·  copia {1} nella cartella del gioco (consigliato)"},
    {L"Looking for the disc in your drives...", L"Buscando el disco en tus unidades...", L"Procurando o disco nas suas unidades...",
     L"Recherche du disque dans vos lecteurs...", L"Suche die Disc in deinen Laufwerken...", L"Cerco il disco nelle tue unità..."},
    {L"Choose a Folder With the Disc's Files...", L"Elegir una carpeta con los archivos del disco...",
     L"Escolher uma pasta com os arquivos do disco...", L"Choisir un dossier avec les fichiers du disque...",
     L"Ordner mit den Dateien der Disc wählen...", L"Scegli una cartella con i file del disco..."},
    {L"Copying Your Disc", L"Copiando tu disco", L"Copiando seu disco", L"Copie de votre disque", L"Deine Disc wird kopiert",
     L"Copia del tuo disco"},
    {L"The files go to the game folder, so the disc doesn't have to stay in the drive.",
     L"Los archivos van a la carpeta del juego, así el disco no tiene que quedarse en la unidad.",
     L"Os arquivos vão para a pasta do jogo, assim o disco não precisa ficar no drive.",
     L"Les fichiers vont dans le dossier du jeu : le disque n'a pas besoin de rester dans le lecteur.",
     L"Die Dateien kommen in den Spielordner, damit die Disc nicht im Laufwerk bleiben muss.",
     L"I file vanno nella cartella del gioco, così il disco non deve restare nel lettore."},
    {L"Checking the Files", L"Comprobando los archivos", L"Verificando os arquivos", L"Vérification des fichiers", L"Dateien werden geprüft",
     L"Controllo dei file"},
    {L"Every file is compared with the original disc, so the game runs exactly as it should.",
     L"Cada archivo se compara con el disco original, para que el juego funcione exactamente como debe.",
     L"Cada arquivo é comparado com o disco original, para o jogo rodar exatamente como deve.",
     L"Chaque fichier est comparé au disque d'origine, pour que le jeu tourne exactement comme il faut.",
     L"Jede Datei wird mit der Original-Disc verglichen, damit das Spiel genau so läuft, wie es soll.",
     L"Ogni file viene confrontato con il disco originale, così il gioco funziona esattamente come deve."},
    {L"Preparing the Game", L"Preparando el juego", L"Preparando o jogo", L"Préparation du jeu", L"Das Spiel wird vorbereitet",
     L"Preparazione del gioco"},
    {L"The game's program is translated into code for your processor. This takes about a minute and only happens again "
     L"after an update.",
     L"El programa del juego se traduce a código para tu procesador. Tarda un minuto aprox. y solo se repite tras una "
     L"actualización.",
     L"O programa do jogo é traduzido para código do seu processador. Leva cerca de um minuto e só se repete depois de uma "
     L"atualização.",
     L"Le programme du jeu est traduit en code pour votre processeur. Cela prend environ une minute et ne se refait qu'après "
     L"une mise à jour.",
     L"Das Spielprogramm wird in Code für deinen Prozessor übersetzt. Das dauert etwa eine Minute und passiert erst nach "
     L"einem Update wieder.",
     L"Il programma del gioco viene tradotto in codice per il tuo processore. Ci vuole circa un minuto e si ripete solo dopo "
     L"un aggiornamento."},
    {L"Try Again", L"Reintentar", L"Tentar de novo", L"Réessayer", L"Nochmal versuchen", L"Riprova"},
    {L"Ready to Race!", L"¡Listo para correr!", L"Pronto para correr!", L"Prêt à courir !", L"Bereit zum Rennen!", L"Pronto a correre!"},
    {L"Everything is in place. You can change any of this later in the launcher.",
     L"Todo está listo. Puedes cambiar cualquier cosa luego en el launcher.", L"Tudo pronto. Você pode mudar qualquer coisa depois no launcher.",
     L"Tout est en place. Vous pourrez tout changer plus tard dans le lanceur.", L"Alles bereit. Du kannst später alles im Launcher ändern.",
     L"Tutto pronto. Puoi cambiare qualsiasi cosa più tardi nel launcher."},
    {L"Disc checked: North America, SLUS-20362", L"Disco comprobado: Norteamérica, SLUS-20362", L"Disco verificado: América do Norte, SLUS-20362",
     L"Disque vérifié : Amérique du Nord, SLUS-20362", L"Disc geprüft: Nordamerika, SLUS-20362", L"Disco verificato: Nord America, SLUS-20362"},
    {L"Game prepared for your processor", L"Juego preparado para tu procesador", L"Jogo preparado para o seu processador",
     L"Jeu préparé pour votre processeur", L"Spiel für deinen Prozessor vorbereitet", L"Gioco preparato per il tuo processore"},
    {L"Picture quality: {0}, {1} fps, {2}, {3}", L"Calidad de imagen: {0}, {1} fps, {2}, {3}", L"Qualidade de imagem: {0}, {1} fps, {2}, {3}",
     L"Qualité d'image : {0}, {1} fps, {2}, {3}", L"Bildqualität: {0}, {1} fps, {2}, {3}", L"Qualità immagine: {0}, {1} fps, {2}, {3}"},
    {L"full screen", L"pantalla completa", L"tela cheia", L"plein écran", L"Vollbild", L"schermo intero"},
    {L"in a window", L"en ventana", L"em janela", L"en fenêtre", L"im Fenster", L"in finestra"},
    {L"Play Now", L"Jugar ahora", L"Jogar agora", L"Jouer maintenant", L"Jetzt spielen", L"Gioca ora"},
    {L"Open the Launcher", L"Abrir el launcher", L"Abrir o launcher", L"Ouvrir le lanceur", L"Launcher öffnen", L"Apri il launcher"},
    // ---- disc checks and job messages ----
    {L"The folder was not found.", L"No se encontró la carpeta.", L"A pasta não foi encontrada.", L"Le dossier est introuvable.",
     L"Der Ordner wurde nicht gefunden.", L"La cartella non è stata trovata."},
    {L"This folder does not hold the contents of a PlayStation 2 disc (SYSTEM.CNF is missing).",
     L"Esta carpeta no contiene un disco de PlayStation 2 (falta SYSTEM.CNF).", L"Esta pasta não contém um disco de PlayStation 2 (falta SYSTEM.CNF).",
     L"Ce dossier ne contient pas un disque PlayStation 2 (SYSTEM.CNF manquant).",
     L"Dieser Ordner enthält keine PlayStation-2-Disc (SYSTEM.CNF fehlt).", L"Questa cartella non contiene un disco PlayStation 2 (manca SYSTEM.CNF)."},
    {L"SLUS_203.62 is missing: this is not the North American Hot Pursuit 2 disc.",
     L"Falta SLUS_203.62: este no es el disco norteamericano de Hot Pursuit 2.", L"Falta SLUS_203.62: este não é o disco norte-americano de Hot Pursuit 2.",
     L"SLUS_203.62 manque : ce n'est pas le disque nord-américain de Hot Pursuit 2.",
     L"SLUS_203.62 fehlt: Das ist nicht die nordamerikanische Hot-Pursuit-2-Disc.",
     L"Manca SLUS_203.62: questo non è il disco nordamericano di Hot Pursuit 2."},
    {L"This disc is {0}. Only the North American release (SLUS-20362) is supported.",
     L"Este disco es {0}. Solo es compatible la edición norteamericana (SLUS-20362).",
     L"Este disco é {0}. Só a edição norte-americana (SLUS-20362) é compatível.",
     L"Ce disque est {0}. Seule l'édition nord-américaine (SLUS-20362) est prise en charge.",
     L"Diese Disc ist {0}. Nur die nordamerikanische Ausgabe (SLUS-20362) wird unterstützt.",
     L"Questo disco è {0}. È supportata solo l'edizione nordamericana (SLUS-20362)."},
    {L"The NFSHP2 folder is missing: the copy of the disc is incomplete.", L"Falta la carpeta NFSHP2: la copia del disco está incompleta.",
     L"Falta a pasta NFSHP2: a cópia do disco está incompleta.", L"Le dossier NFSHP2 manque : la copie du disque est incomplète.",
     L"Der Ordner NFSHP2 fehlt: Die Kopie der Disc ist unvollständig.", L"Manca la cartella NFSHP2: la copia del disco è incompleta."},
    {L"SLUS_203.62 is not the supported version (another edition or a damaged copy).",
     L"SLUS_203.62 no es la versión compatible (otra edición o una copia dañada).",
     L"SLUS_203.62 não é a versão compatível (outra edição ou cópia danificada).",
     L"SLUS_203.62 n'est pas la version prise en charge (autre édition ou copie endommagée).",
     L"SLUS_203.62 ist nicht die unterstützte Version (andere Ausgabe oder beschädigte Kopie).",
     L"SLUS_203.62 non è la versione supportata (altra edizione o copia danneggiata)."},
    {L"Copy cancelled.", L"Copia cancelada.", L"Cópia cancelada.", L"Copie annulée.", L"Kopieren abgebrochen.", L"Copia annullata."},
    {L"Check cancelled.", L"Comprobación cancelada.", L"Verificação cancelada.", L"Vérification annulée.", L"Prüfung abgebrochen.",
     L"Verifica annullata."},
    {L"Cancelled.", L"Cancelado.", L"Cancelado.", L"Annulé.", L"Abgebrochen.", L"Annullato."},
    {L"{0} is missing from the disc folder.", L"Falta {0} en la carpeta del disco.", L"Falta {0} na pasta do disco.",
     L"{0} manque dans le dossier du disque.", L"{0} fehlt im Disc-Ordner.", L"Manca {0} nella cartella del disco."},
    {L"{0} has the wrong size: the copy is incomplete or damaged.", L"{0} tiene un tamaño incorrecto: la copia está incompleta o dañada.",
     L"{0} tem o tamanho errado: a cópia está incompleta ou danificada.", L"{0} n'a pas la bonne taille : la copie est incomplète ou endommagée.",
     L"{0} hat die falsche Größe: Die Kopie ist unvollständig oder beschädigt.", L"{0} ha la dimensione sbagliata: la copia è incompleta o danneggiata."},
    {L"{0} could not be read.", L"No se pudo leer {0}.", L"Não foi possível ler {0}.", L"Impossible de lire {0}.",
     L"{0} konnte nicht gelesen werden.", L"Impossibile leggere {0}."},
    {L"{0} differs from the original disc: copy the disc again.", L"{0} no coincide con el disco original: copia el disco de nuevo.",
     L"{0} difere do disco original: copie o disco de novo.", L"{0} diffère du disque d'origine : copiez à nouveau le disque.",
     L"{0} weicht von der Original-Disc ab: Kopiere die Disc erneut.", L"{0} è diverso dal disco originale: copia di nuovo il disco."},
    {L"Preparing the game failed: {0} (details in prepare.log)", L"Falló la preparación del juego: {0} (detalles en prepare.log)",
     L"A preparação do jogo falhou: {0} (detalhes em prepare.log)", L"La préparation du jeu a échoué : {0} (détails dans prepare.log)",
     L"Die Vorbereitung des Spiels ist fehlgeschlagen: {0} (Details in prepare.log)",
     L"Preparazione del gioco non riuscita: {0} (dettagli in prepare.log)"},
    {L"The game ended without a message (it may have crashed).", L"El juego terminó sin mensaje (puede que se haya colgado).",
     L"O jogo terminou sem mensagem (pode ter travado).", L"Le jeu s'est terminé sans message (il a peut-être planté).",
     L"Das Spiel endete ohne Meldung (vielleicht abgestürzt).", L"Il gioco si è chiuso senza messaggi (potrebbe essersi bloccato)."},
};

std::atomic<int> g_lang{kEnglish};
std::once_flag g_built;
std::unordered_map<std::wstring, const Row*> g_index;

}  // namespace

const wchar_t* LangName(int lang) {
  static const wchar_t* names[kLangCount] = {L"English", L"Español", L"Português (Brasil)", L"Français", L"Deutsch", L"Italiano"};
  return names[std::clamp(lang, 0, kLangCount - 1)];
}

int Detect() {
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_SPANISH: return kSpanish;
    case LANG_PORTUGUESE: return kPortuguese;
    case LANG_FRENCH: return kFrench;
    case LANG_GERMAN: return kGerman;
    case LANG_ITALIAN: return kItalian;
    default: return kEnglish;
  }
}

void Set(int lang) { g_lang = std::clamp(lang, 0, kLangCount - 1); }
int Get() { return g_lang; }

const wchar_t* T(const wchar_t* english) {
  const int lang = g_lang;
  if (lang == kEnglish || !english) return english;
  std::call_once(g_built, [] {
    for (const Row& r : kRows) g_index.emplace(r.t[0], &r);
  });
  const auto it = g_index.find(english);
  return it == g_index.end() || !it->second->t[lang] ? english : it->second->t[lang];
}

std::wstring F(const wchar_t* english, std::initializer_list<std::wstring> args) {
  std::wstring s = T(english);
  int i = 0;
  for (const std::wstring& a : args) {
    const std::wstring tag = L"{" + std::to_wstring(i++) + L"}";
    for (std::size_t at; (at = s.find(tag)) != std::wstring::npos;) s.replace(at, tag.size(), a);
  }
  return s;
}

}  // namespace i18n
