# Primeros pasos — juega *Hot Pursuit 2* de forma nativa en tu PC

🌐 [English](GETTING-STARTED.md) · **Español** · [Português (Brasil)](GETTING-STARTED.pt-BR.md) · [Français](GETTING-STARTED.fr.md) · [Deutsch](GETTING-STARTED.de.md) · [Italiano](GETTING-STARTED.it.md)

¡Hola! Esta guía te lleva de tu disco de PS2 a correr en tu PC, paso a paso. Escribirás unos pocos comandos y el
resto se hace solo. Cuenta con **una hora aprox. la primera vez** — casi todo es esperar mientras tu PC compila, así que
puedes ir por algo para picar. Después, abrir el juego es un doble clic.

> [!IMPORTANT]
> Necesitas **tu propio disco original** de *Need for Speed: Hot Pursuit 2* para PlayStation 2. El proyecto no incluye
> archivos del juego ni descarga ninguno.

> [!WARNING]
> **Guardar partida todavía no está probado.** El juego escribe en una tarjeta de memoria guardada como archivos en la
> carpeta del juego, pero guardar y cargar una carrera profesional no se ha comprobado de principio a fin. No cuentes aún
> con conservar tu progreso. Las carreras sueltas se juegan de principio a fin.

---

## Qué necesitas

| Necesitas | Detalles |
|---|---|
| **El disco** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **edición norteamericana**. El código **SLUS-20362** está impreso en el disco y en la parte trasera de la caja. Otras regiones (por ejemplo la europea `SLES-50731`) y cualquier edición con otro programa no son compatibles — el launcher lo comprueba por ti. |
| **Una forma de leerlo** | Una unidad de DVD en tu PC (o conectada a él). Si ya hiciste una imagen `.iso` de tu propio disco, también sirve. |
| **El PC** | Windows 10 u 11 de 64 bits. Una tarjeta gráfica con Direct3D 11 (cualquiera de los últimos diez años). |
| **Espacio libre** | Unos **15 GB**: 3 GB para la copia de tu disco, 6 GB para las herramientas de compilación de C++ (paso 1) y 6 GB para el proyecto con su generador de código (paso 4). |
| **Un mando** *(recomendado)* | Un mando de Xbox, o cualquier cosa que Windows vea como uno. El teclado también funciona. |
| **Internet** | Para el paso 1 y para la primera ejecución del instalador (descarga unos 1–2 GB de herramientas y código fuente). |

## Paso 1 — Instala las herramientas (una vez)

El juego se compila en tu PC, así que necesita tres herramientas gratuitas de Microsoft y de Git. La forma más fácil es
**winget**, que viene con Windows 10 y 11.

1. Abre el menú **Inicio**, escribe `Terminal`, haz clic derecho en **Terminal** y elige **Ejecutar como administrador**.
2. Copia estas tres líneas, una a la vez, pulsando Enter después de cada una. Cada línea tarda unos minutos; espera a que
   termine antes de pasar a la siguiente.

   ```powershell
   winget install --id Microsoft.PowerShell -e
   winget install --id Git.Git -e
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   La tercera línea instala *Build Tools for Visual Studio 2022* con la carga de trabajo *Desarrollo para el escritorio
   con C++* — el compilador de C++ que necesita el proyecto (unos 6 GB). Una ventana del instalador de Visual Studio
   muestra el progreso.
3. **Reinicia el PC** cuando terminen las tres.

<details>
<summary>¿Prefieres instalarlas a mano?</summary>

- **PowerShell 7:** desde [Microsoft Store](https://apps.microsoft.com/detail/9mz1snwt0n5d) o la
  [página de PowerShell](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) de Microsoft.
- **Git para Windows:** desde [git-scm.com](https://git-scm.com/download/win); las opciones por defecto están bien.
- **Build Tools for Visual Studio 2022:** desde [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
  (en *Herramientas para Visual Studio*). En el instalador, marca **Desarrollo para el escritorio con C++** y haz clic en
  *Instalar*. Si ya tienes Visual Studio 2022 con esa carga de trabajo, no tienes que instalar nada.

</details>

## Paso 2 — Ten tu disco a mano

El launcher copia tu disco al PC en el paso 5, así que después el disco no tiene que quedarse en la unidad. Antes del
paso 5, haz una de estas cosas:

- **Tu disco:** mételo en la unidad de DVD.
- **Una imagen `.iso` de tu propio disco:** haz doble clic en el archivo `.iso` en el Explorador de archivos. Windows la
  muestra como una nueva unidad de DVD (puedes expulsarla después del paso 5).
- **Ya copiaste los archivos del disco a una carpeta:** no hay que hacer nada; podrás elegir esa carpeta en el paso 5.
  Debe contener `SYSTEM.CNF`, `SLUS_203.62` y la carpeta `NFSHP2`.

## Paso 3 — Descarga HotPursuit2PC (una vez)

1. Abre el menú **Inicio**, escribe `PowerShell 7` y ábrelo (no el antiguo "Windows PowerShell").
2. Copia estas líneas, pulsando Enter después de cada una:

   ```powershell
   cd $HOME\Documents
   git clone https://github.com/awtocl/HotPursuit2PC.git
   cd HotPursuit2PC
   ```

El proyecto queda en `Documentos\HotPursuit2PC`. (Usa `git clone` y no *Download ZIP*: Windows bloquea los scripts que
vienen de un ZIP descargado, y `git` además hace fácil actualizar.)

## Paso 4 — Compila el juego

En la misma ventana de PowerShell 7, escribe:

```powershell
pwsh tools/install-native.ps1
```

El script avanza por cinco pasos y te dice en cuál está (sus mensajes están en inglés):

| Paso | Qué pasa | Cuánto tarda |
|---|---|---|
| 1/5 Your disc | Nada todavía: el launcher te pide el disco en el paso 5 | — |
| 2/5 LLVM | **Solo la primera vez:** descarga y compila LLVM, el generador de código que usa el recompilador | **15–40 min** |
| 3/5 Building | Compila HotPursuit2PC: el launcher, el juego y el recompilador | 3–10 min |
| 4/5 Preparing | Nada todavía: lo hace el launcher después de comprobar tu disco | — |
| 5/5 Game folder | Deja los tres programas en `game-native\` | segundos |

Puede que tu PC haga ruido y vaya cargado durante los pasos 2 y 3; es normal. Cuando termina con **"Done …"** en verde,
el launcher se abre solo.

## Paso 5 — Configura el juego en el launcher (una vez)

La primera vez, el launcher te guía por cuatro pasos cortos:

1. **Disco** — tu unidad de DVD (o la `.iso` montada) aparece sola; elige **Copiar desde …** para copiarla a la carpeta
   del juego (unos 3 GB, unos minutos). O elige **una carpeta con los archivos del disco**. Si el disco es de otra región
   o edición, el launcher te lo dice aquí.
2. **Comprobar** — cada archivo se compara con el disco original.
3. **Preparar** — el programa del juego se traduce a código para tu procesador (un minuto aprox.).
4. **Listo** — la calidad de imagen ya está ajustada para tu pantalla. Elige **Jugar ahora**.

![La pantalla principal del launcher](assets/launcher-home.png)

## Paso 6 — A jugar

A partir de ahora, abre **`HotPursuit2Launcher.exe`** en `Documentos\HotPursuit2PC\game-native` y elige **Jugar**. Para
un acceso directo en el escritorio: launcher → **Disco e instalación** → **Acceso directo**.

El launcher funciona con el mouse, el teclado (flechas, Enter, Esc, Re Pág/Av Pág) o un mando de Xbox (A elegir, B
atrás, LB/RB páginas, Start juega). Sus páginas:

| Página | Para qué sirve |
|---|---|
| **Jugar** | Abrir el juego; ver si el disco y el juego están en orden; tu tiempo de juego |
| **Ajustes** | La imagen y la pantalla, cada ajuste explicado, con la recomendación para tu pantalla |
| **Controles** | Esquema de mando Clásico o Moderno, reasignar cualquier tecla o botón, restaurar los originales |
| **Tarjeta de memoria** | Lo que el juego ha guardado; hacer una copia de seguridad y restaurarla |
| **Capturas** | Las imágenes que tomaste con **F12** en el juego |
| **Disco e instalación** | Comprobar todos los archivos del disco, preparar el juego de nuevo, usar otra copia del disco, crear un acceso directo |
| **Acerca de** | Qué es este proyecto, créditos, enlaces a esta guía y a los issues |

El juego arranca en pantalla completa con los videos de introducción; pulsa **Enter** (o Start en el mando) para
saltarlos. El launcher se oculta mientras juegas y vuelve cuando sales.

### Ajustes recomendados

Cámbialos en la página **Ajustes** del launcher, o mientras juegas con **F1** (o **Select + Start** en el mando). La
instalación ya eligió la **Calidad de imagen** (la resolución interna) para tu pantalla; es lo que decide lo nítida que
se ve la imagen:

| Tu pantalla | Calidad de imagen |
|---|---|
| 1920 × 1080 (Full HD) | **Calidad (4x)** |
| 2560 × 1440 | **Ultra (6x)** |
| 3840 × 2160 (4K) | **Máximo (8x)** |

Más alto es más suave y nítido (dibuja más píxeles de los que tiene tu pantalla y los promedia). Si alguna vez el juego
va a tirones, elige el valor anterior.

| Ajuste | Recomendado | Por qué |
|---|---|---|
| Renderizador | **GPU** | El de software es más lento y se conserva para comparar |
| Fotogramas por segundo | **60** | El doble de los 30 fps de la PS2 en carrera; el juego mantiene su velocidad normal |
| Modo de pantalla | **Pantalla completa** | Alt+Enter o F11 cambian en cualquier momento |
| Sincronización vertical | **Activado** | Movimiento más estable, sin cortes |
| Filtro de escalado | **Bilineal nítido** | Nítido y sin bordes dentados |
| Relación de aspecto | **16:9** | Más campo de visión en carrera; los menús mantienen su forma (cambiar entre 16:9 y 4:3 requiere reiniciar) |
| Nitidez | **Desactivado** o **Bajo** | Valores más altos pueden hacer que los bordes parpadeen |
| Líneas de barrido | **Desactivado** | Salvo que te guste el aspecto de televisor antiguo |

Los ajustes se guardan solos en `game-native\hp2native.ini`.

### Controles

Elige un esquema en la página **Controles** del launcher (o en el juego con **F1** → *Esquema del mando*):

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
| Menú de ajustes | F1 | Select + Start | Select + Start |
| Captura | F12 | – | – |

- **Clásico** es el mando de PS2 botón por botón. **Moderno** pone el acelerador y el freno en los gatillos, como los
  juegos de carreras actuales.
- **Crea el tuyo:** en la página **Controles**, elige una casilla y pulsa la tecla o el botón que quieras. Cada acción
  admite dos teclas y dos botones del mando. Esc cancela, Supr borra una tecla. Tu primer cambio crea un esquema
  *Personalizado* a partir del que estabas usando.
- **¿Te arrepentiste?** **Restaurar teclado original** y **Restaurar mando original** vuelven directamente a los
  esquemas de fábrica. Tu esquema personalizado queda guardado; elige *Personalizado* para volver a usarlo.
- Los sticks analógicos funcionan como los del DualShock 2. Windows no reconoce por sí solo los mandos de PlayStation
  como mandos de juego; funcionan con una herramienta que los presenta como mando de Xbox, como el soporte de mandos de
  Steam.

### Idioma

El launcher y el menú de ajustes del juego están en inglés, español, portugués (Brasil), francés, alemán e italiano. La
primera vez siguen el idioma de tu Windows; cámbialo en el launcher en **Ajustes → Idioma** o en el juego con **F1**. Los
textos propios del juego siguen en inglés: así viene el disco norteamericano.

### Salir

**F1 → Salir del juego** (púlsalo dos veces para confirmar), o **Alt+F4**.

## Actualizar

Cuando el proyecto tenga novedades, actualiza y vuelve a compilar — tus ajustes, partidas y copia del disco se mantienen:

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1
```

LLVM no se vuelve a compilar, así que tarda solo unos minutos. Luego el launcher muestra **Preparar y jugar**: el juego
se prepara de nuevo para la versión nueva (un minuto aprox.) y arranca.

## Si algo sale mal

| Lo que ves | Qué hacer |
|---|---|
| *"The C++ build tools were not found"* | El paso 1 no está completo: instala las Build Tools con **Desarrollo para el escritorio con C++**, reinicia el PC y vuelve a intentarlo. |
| *"pwsh no se reconoce"* | PowerShell 7 no está instalado, o no reiniciaste el PC después del paso 1. |
| El launcher no encuentra tu disco | Espera unos segundos después de meterlo; con una `.iso`, haz doble clic en ella primero. O copia tú mismo los archivos del disco a una carpeta y elige **una carpeta con los archivos del disco**. |
| *"Esta carpeta no contiene un disco de PlayStation 2"* o *"Falta …"* | Elegiste la carpeta equivocada o la copia está incompleta. Elige la carpeta que contiene directamente `SYSTEM.CNF`, `SLUS_203.62` y `NFSHP2`. |
| *"Este disco es SLES-…"* o *"… no es la versión compatible"* | Tu disco es de otra región o edición. Solo es compatible la edición norteamericana `SLUS-20362`. |
| *"… no coincide con el disco original"* | La copia del disco está dañada (a menudo por un disco rayado). Limpia el disco y cópialo de nuevo (**Disco e instalación** → **Cambiar disco**). |
| *"Falló la preparación del juego"* | Los detalles están en `game-native\prepare.log`; adjúntalo en un [issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"El juego se detuvo: …"* en la página Jugar | Elige **Abrir registro** e **Informar de un problema**; adjunta `native.log`. |
| *"Downloading LLVM failed"* | Revisa tu conexión a internet y vuelve a ejecutar el instalador. |
| *"The build failed"* | Copia las líneas en rojo y [abre un issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"…no se puede cargar porque la ejecución de scripts está deshabilitada"* | Descargaste un ZIP en lugar de usar `git clone`. Usa `git clone` (paso 3). |
| La ventana del juego se ve negra o la imagen está mal | Launcher → **Ajustes** → Renderizador → **Software**. Si eso ayuda, infórmalo indicando tu tarjeta gráfica. |
| No hay sonido | El juego usa el dispositivo de reproducción predeterminado de Windows: revísalo en la configuración de sonido de Windows y reinicia el juego. |
| El mando no hace nada | Solo se reconocen mandos compatibles con Xbox (XInput); ver [Controles](#controles). El teclado siempre funciona. |
| Un botón hace lo que no debe | Launcher → **Controles** → **Restaurar mando original** (o **Restaurar teclado original**). |
| Va a tirones | Baja la **Calidad de imagen** (launcher → Ajustes) y asegúrate de que la sincronización vertical esté activada. |
| Todo se ve mal después de cambiar ajustes | Launcher → **Ajustes** → **Restablecer**. |

**Informar de un problema:** después de cerrar el juego, el archivo `game-native\native.log` describe lo que pasó.
Adjúntalo en un [issue](https://github.com/awtocl/HotPursuit2PC/issues) con tu versión de Windows y tu tarjeta gráfica.
El registro no contiene datos personales aparte de rutas de carpetas, que puedes borrar.

## Desinstalar

Este proyecto no instala nada en Windows. Para quitarlo, borra la carpeta `Documentos\HotPursuit2PC` (las herramientas
de compilación están dentro, en `.cache`, y la copia de tu disco en `game-native\disc`). Antes, haz una copia de
`game-native\saves` si quieres conservar tus partidas. Un acceso directo creado por el launcher se borra como cualquier
otro. Las herramientas del paso 1 se quitan en *Configuración → Aplicaciones* como cualquier otro programa.

---

¿Algo se trabó, o hay algo que no está claro? [Abre un issue](https://github.com/awtocl/HotPursuit2PC/issues) y cuéntanos —
esta guía debería funcionarle a todo el mundo. ¡Diviértete en la carretera, y cuidado con la policía!
