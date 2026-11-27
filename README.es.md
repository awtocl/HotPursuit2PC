<p align="center">
  <img src="docs/assets/banner.svg" alt="HotPursuit2PC" width="100%">
</p>

<p align="center">
  <a href="https://github.com/awtocl/HotPursuit2PC/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/awtocl/HotPursuit2PC/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <img alt="Plataforma: Windows x64" src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-1d3a5f">
  <img alt="Lenguaje: C++20" src="https://img.shields.io/badge/C%2B%2B-20-1d3a5f">
  <a href="LICENSE"><img alt="Licencia: MIT" src="https://img.shields.io/badge/license-MIT-ff921c"></a>
  <img alt="Estado: versión en desarrollo" src="https://img.shields.io/badge/status-development%20build-ff921c">
  <a href="#una-palabra-sobre-la-ia"><img alt="Hecho con ayuda de IA" src="https://img.shields.io/badge/AI--assisted-Claude%20Code-6a5acd"></a>
</p>

<p align="center">
  <b>La versión de PlayStation 2 de <i>Need for Speed: Hot Pursuit 2</i>, funcionando de forma nativa en Windows — creada a partir de tu propio disco.</b>
</p>

<p align="center">
  <a href="docs/GETTING-STARTED.es.md"><b>Primeros pasos</b></a> ·
  <a href="#inicio-rápido">Inicio rápido</a> ·
  <a href="#qué-trae">Qué trae</a> ·
  <a href="#controles">Controles</a> ·
  <a href="#problemas-conocidos">Problemas conocidos</a> ·
  <a href="#aviso-legal-y-procedencia">Aviso legal</a> ·
  <a href="#una-palabra-sobre-la-ia">Una palabra sobre la IA</a>
</p>

<p align="center">
  🌐 <a href="README.md">English</a> · <b>Español</b> · <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <a href="README.fr.md">Français</a> · <a href="README.de.md">Deutsch</a> · <a href="README.it.md">Italiano</a>
</p>

> [!IMPORTANT]
> **No incluye archivos del juego ni descarga ninguno.** Necesitas **tu propio disco** de *Need for Speed: Hot
> Pursuit 2* para PlayStation 2 (Norteamérica, `SLUS-20362`).

> [!NOTE]
> Hecho con mucha ayuda de una IA (Claude Code). ¿Curiosidad o desconfianza? Aquí está [lo que pienso al respecto](#una-palabra-sobre-la-ia).

---

## ¿Por qué?

> La versión de PS2 es la mejor versión de este juego, así que es la que este proyecto trae al PC.

*Hot Pursuit 2* salió en varias consolas, y la de PS2 es la que la gente recuerda: el manejo, los autos, las pistas, la
sensación. Esto no es un remake, ni tampoco la versión de PC de 2003 (que es bastante distinta). Es exactamente ese
juego de PS2, convertido en un programa de Windows de verdad: código x64 nativo, la resolución de tu monitor, tu mando
— y ningún emulador de por medio.

## Qué trae

| | |
|---|---|
| **Sin emulador** | El código del propio juego se traduce a código x64 nativo (con LLVM), en tu PC y desde tu copia. El hardware de la consola se reemplaza por equivalentes de PC: tus archivos en lugar del lector de discos, tu tarjeta gráfica, un mezclador de sonido, tu mando. |
| **Gráficos nítidos** | Direct3D 11 de 1x a 8x la resolución original (con supermuestreo cuando supera la de tu pantalla), escalado nítido, nitidez y líneas de barrido opcionales, pantalla completa sin bordes. |
| **16:9 y 60 fps** | Panorámico de serie (ves más de la carretera; el HUD y los menús conservan su forma) y 60 fotogramas por segundo, gracias a los parches de la comunidad para este juego (ver [Créditos](#licencia-y-créditos)). Ambos se pueden desactivar. |
| **Todo el sonido** | La música de EA Trax, motores y efectos, la radio de la policía y el sonido de los videos. |
| **Un launcher** | Encuentra tu disco, lo copia y lo comprueba, prepara el juego y lo recuerda todo. Además: ajustes con una recomendación para tu pantalla, copias de seguridad de la tarjeta de memoria y una galería de capturas (F12). Funciona con mouse, teclado o mando. |
| **Tus controles, a tu manera** | Un esquema de mando Clásico (PS2) y uno Moderno (gatillos), y cada tecla y botón se puede reasignar. Vuelve a los valores originales con un clic. |
| **Seis idiomas** | El launcher y el menú del juego hablan inglés, español, portugués, francés, alemán e italiano (la primera vez, según tu Windows). |
| **Ajustes dentro del juego** | Pulsa **F1** (o Select+Start) para abrir un menú de ajustes con el estilo del propio juego, que incluye *Salir del juego*. |
| **Tus archivos son tuyos** | Tu disco solo se lee. Todo lo que se crea a partir de él se queda en tu PC. |

<p align="center"><img src="docs/assets/launcher-home.png" width="760" alt="El launcher de HotPursuit2PC: la página Jugar con el estado del disco y del juego, un gran botón JUGAR y las teclas del juego"></p>

## Inicio rápido

> [!TIP]
> **¿Primera vez que haces algo así?** Tranquilidad: la guía **[Primeros pasos](docs/GETTING-STARTED.es.md)** te lleva paso
> a paso: qué necesitas, cómo instalar las herramientas, compilar, los mejores ajustes y qué hacer si algo falla.

**La versión corta:** en Windows 10/11 x64 con *Build Tools for Visual Studio 2022* (carga de trabajo de C++), Git y
PowerShell 7 instalados, y **tu propio disco** en la unidad (o tu `.iso` montada):

```powershell
git clone https://github.com/awtocl/HotPursuit2PC.git
cd HotPursuit2PC
pwsh tools/install-native.ps1       # compila todo y luego abre el launcher
```

La primera vez compila LLVM (prepárate un café: de 15 a 40 minutos), luego el proyecto, y deja el launcher, el juego y
el recompilador en `game-native\`. Desde ahí sigue el launcher: copia y comprueba tu disco y traduce el juego **desde tu
copia, en tu PC** (un minuto aprox.). Después, basta con abrir **`HotPursuit2Launcher.exe`**. Cerca de una hora la
primera vez; unos minutos tras una actualización.

Todavía no hay descargas listas para usar. El juego traducido siempre se crea desde tu propia copia y en tu propio PC:
nunca se distribuye.

## Controles

Elige un esquema o crea el tuyo:

| Acción | Teclado | Mando: Clásico (PS2) | Mando: Moderno |
|---|---|---|---|
| Acelerar | Z | A | RT (o A) |
| Frenar / Marcha atrás | A | X | LT |
| Girar | ← → | Stick izquierdo / cruceta | Stick izquierdo / cruceta |
| Freno de mano | W | RB | B |
| Claxon / Sirena | X | B | Clic del stick izquierdo |
| Cambiar vista | S | Y | Y |
| Mirar atrás | Q | LB | Clic del stick derecho |
| Impulso de velocidad | E | Clic del stick derecho | X |
| Subir marcha / Zone Zoom | 2 | RT | RB |
| Bajar marcha / Zone Freeze | 1 | LT | LB |
| Pausa | Enter, Espacio | Start | Start |
| Cambiar HUD | Retroceso, Mayús derecha | Back | Back |

*Clásico* es el mando de PS2 botón por botón; *Moderno* pone el acelerador y el freno en los gatillos, como los juegos
de carreras actuales. Para cambiar algo, abre la página **Controles** del launcher, elige una casilla y pulsa la tecla o
el botón que quieras (dos de cada uno por acción). **Restaurar teclado original** y **Restaurar mando original** traen
de vuelta los esquemas de fábrica cuando quieras, y tu esquema personalizado queda guardado para después. El esquema del
mando también se puede cambiar dentro del juego con **F1**.

También útil: **F1** menú de ajustes, **F12** captura, **Alt+Enter** o **F11** pantalla completa, **Alt+F4** o *Salir
del juego* en el menú de ajustes para salir.

## Problemas conocidos

Es una versión en desarrollo y por ahora solo se ha probado en un PC. Esto es lo que te puedes encontrar:

- **Guardar partida no está probado.** El juego tiene una tarjeta de memoria (archivos en `game-native\saves`), pero
  todavía no se ha comprobado de principio a fin guardar y cargar una carrera profesional.
- **Sonido:** aún sin reverberación ni envolventes de volumen reales, así que algunos sonidos se cortan un poco de golpe.
- **Gráficos:** aún sin niebla de profundidad, mipmaps, tramado ni líneas; la niebla de distancia en carrera se omite por
  ahora.
- **Controles:** aún sin vibración ni volante. El juego lee el acelerador y el freno como botones de encendido/apagado,
  así que los gatillos también funcionan como botones.
- **Idioma:** los textos propios del juego siguen en inglés (así viene este disco norteamericano); el launcher y el menú
  de ajustes siguen tu idioma.
- El cambio entre 16:9 y 4:3 se aplica tras reiniciar (el juego coloca su HUD una sola vez).
- Aún sin probar con gráficas AMD o Intel, en un Windows recién instalado o en sesiones muy largas.

¿Encontraste algo más? [Abre un issue](https://github.com/awtocl/HotPursuit2PC/issues): ayuda mucho.

## Aviso legal y procedencia

> [!NOTE]
> Esta traducción es para tu comodidad. La versión de referencia es la inglesa:
> [Legal position and provenance](README.md#legal-position-and-provenance).

Lee esta sección antes de usar o redistribuir cualquier cosa.

**En este repositorio no hay ninguna propiedad de Electronic Arts.** Cada archivo de código fuente se escribió para este
proyecto y se publica bajo la licencia MIT (ver [`LICENSE`](LICENSE)). El repositorio no contiene código, recursos,
textos, audio, video ni otro material propiedad de Electronic Arts Inc., Sony Interactive Entertainment ni de ningún
otro titular de derechos del juego, ni ninguna parte del ejecutable del juego en ninguna forma (original,
desensamblada, descompilada o traducida).

Para que se pueda comprobar, esto es todo lo del repositorio que se *derivó* del disco — todo son datos sobre el disco,
nada de su contenido:

- [`converter/data/slus20362_rom_manifest.tsv`](converter/data/slus20362_rom_manifest.tsv): nombres, tamaños y hashes
  SHA-256 de los 27 archivos del disco, para distinguir una copia buena de una dañada o distinta.
- [`assets-pipeline/data/hp2_slus20362_names.txt`](assets-pipeline/data/hp2_slus20362_names.txt): los nombres de 362
  archivos dentro del archivo comprimido del juego, recuperados para que los archivos extraídos tengan nombres legibles.
- [`tests/fixtures/slus20362_zfileset_inventory.tsv`](tests/fixtures/slus20362_zfileset_inventory.tsv): posiciones,
  tamaños y hashes SHA-256 de cada archivo dentro de ese archivo comprimido, usados por las pruebas.
- [`docs/dump-analysis.md`](docs/dump-analysis.md): una descripción del formato de ese contenedor y el inventario
  completo de archivos.
- [`docs/recomp-analysis.md`](docs/recomp-analysis.md) y `recomp/tests/test_recomp_dump.cpp`: mediciones del programa
  del juego (direcciones y tamaños de secciones, cantidades de instrucciones, funciones y tablas de saltos, unas pocas
  direcciones de código). Ninguna instrucción ni byte del programa en sí.
- [`recomp/include/hp2/game_patches.h`](recomp/include/hp2/game_patches.h): las direcciones y valores de los parches de
  16:9 y 60 fps (ver [Créditos](#licencia-y-créditos)).

**Tienes que aportar tu propia copia.** Para usar estas herramientas necesitas una copia de un ejemplar del juego que
*tú* posees legalmente, hecha por ti desde tu propio disco. Este proyecto no proporciona, no enlaza ni ayuda a
encontrar archivos del juego, imágenes de disco ni firmware de consola, y no lo hará. No se necesita ni se usa ninguna
BIOS de PlayStation 2. Las leyes sobre hacer y usar copias de seguridad de software que posees varían entre países; eres
responsable de cumplir las que te correspondan.

**Cómo se probó.** Todo el desarrollo y las pruebas se hicieron con una sola copia: la que hizo quien mantiene el
repositorio a partir del disco original que tiene. No se usó en ningún momento otra copia del juego, ninguna imagen descargada ni material
de la versión de Windows. Las herramientas abren la copia en modo de solo lectura y nunca la modifican.

**La traducción ocurre en tu equipo.** Todo lo que las herramientas derivan del juego — el código recompilado, los
recursos extraídos — se produce en tu propio ordenador a partir de tu propia copia y se queda ahí. No lo redistribuyas.

**Sin afiliación.** Este es un proyecto independiente, sin fines comerciales, de preservación e interoperabilidad. No
está afiliado, patrocinado ni respaldado por Electronic Arts, Sony ni los autores del software libre en el que se basa.
*Need for Speed* y *Hot Pursuit 2* son marcas de Electronic Arts Inc.; *PlayStation* es una marca de Sony Interactive
Entertainment Inc. Se usan aquí solo para indicar con qué son compatibles las herramientas.

Si eres titular de derechos y crees que algo de aquí no debería estar, abre un issue.

## Una palabra sobre la IA

> Sé que mucha gente odia la IA, y tarde o temprano todo lo que haya tocado aunque sea un poco un modelo de IA termina
> llamándose "AI slop". Mi postura es esta: la IA no se va a ir. Es una herramienta nueva que nos permite hacer cosas que
> antes no creíamos posibles, y no voy a dejar que una ideología me impida incorporarla a mi forma de trabajar. Te guste
> o no, la IA puede escribir código bastante bueno. Ya que está, úsala para hacerte la vida un poco más fácil, aprende
> para qué sirve en el camino — y, en mi caso, métete en proyectos que traen los juegos de mi infancia al PC.

Empecé HotPursuit2PC para aprender en la práctica a crear software junto a una IA, con algo lo bastante difícil como
para ser una prueba real. Así se repartió el trabajo:

- **Un agente de programación con IA escribió el código, las pruebas, los scripts y la documentación** — Claude, de
  Anthropic, en Claude Code — bajo mi dirección. Los commits llevan una línea `Co-Authored-By: Claude`.
- **Yo fijé los objetivos y las reglas** (están en [`docs/brief.md`](docs/brief.md)), hice la copia de mi propio disco,
  aprobé cada paso que salía del proyecto (como descargar código de terceros), probé y jugué cada versión, y tomé las
  decisiones — por ejemplo, dejar la versión con emulador por la recompilación estática después de jugarla.
- **Lo que sabemos del juego sale de mi propia copia**, por inspección y experimentos, con la evidencia anotada en
  [`docs/dump-analysis.md`](docs/dump-analysis.md). El código fuente del juego nunca estuvo disponible ni se usó.

Y, para ser justos contigo: el código escrito por IA puede equivocarse con total seguridad. Por eso el proyecto se apoya
en el proceso, no en la confianza: cada afirmación sobre el disco está respaldada por una comprobación o una prueba, lo
no demostrado se marca **UNVERIFIED** y cada decisión queda escrita con sus alternativas. Nadie ha revisado cada línea a
mano: échale un vistazo antes de confiar en él.

## Para desarrolladores

Cómo funciona por dentro, cómo compilar desde el código, la estructura del repositorio y toda la documentación técnica
están en el [README en inglés](README.md#how-it-works) y en [`CONTRIBUTING.md`](CONTRIBUTING.md). Las decisiones de diseño
están en [`docs/adr/`](docs/adr/).

## Hoja de ruta

- [ ] Guardar partidas de forma fiable (como archivos de PC)
- [ ] Reverberación y envolventes reales en el mezclador de sonido
- [ ] Niebla de profundidad y mipmaps en el renderizador por GPU
- [ ] Vibración y volantes
- [ ] Texto y texturas más nítidos
- [ ] Una descarga lista para usar, sin herramientas de compilación
- [ ] Movimiento más suave por encima de 60 fps

Es un camino largo, así que sin fechas: estará cuando esté.

## Licencia y créditos

El código y la documentación de este repositorio se publican bajo la [licencia MIT](LICENSE).

- **Parches del juego:** los valores de 16:9 y 60 fps vienen del archivo de parches de la comunidad para este disco en
  [PCSX2/pcsx2_patches](https://github.com/PCSX2/pcsx2_patches) (`SLUS-20362_1D2818AF.pnach`): *Widescreen 16:9* de
  **nemesis2000**, *HUD fixed for widescreen* de **Aero_** y *60 FPS* de **asasega**. Solo se usaron las direcciones y
  los valores, reimplementados como parches activables. ¡Gracias!
- **LLVM** (Apache 2.0 con excepciones LLVM): el generador de código del recompilador.
- **Play!** de Jean-Philip Desjardins (BSD 2-Clause): el núcleo de emulador del launcher de referencia y sus bibliotecas
  (detalles en [`docs/prior-art.md`](docs/prior-art.md)). El juego nativo no contiene código de terceros; usa Direct3D 11,
  XAudio2 y XInput de Windows.
