# Primeiros passos — jogue *Hot Pursuit 2* nativamente no seu PC

🌐 [English](GETTING-STARTED.md) · [Español](GETTING-STARTED.es.md) · **Português (Brasil)** · [Français](GETTING-STARTED.fr.md) · [Deutsch](GETTING-STARTED.de.md) · [Italiano](GETTING-STARTED.it.md)

Olá! Este guia leva você do seu disco de PS2 até correr no seu PC, um passo de cada vez. Você vai digitar alguns
comandos e o resto acontece sozinho. Conte com **cerca de uma hora na primeira vez** — quase tudo é esperar o PC
compilar, então pode pegar um lanche. Depois disso, abrir o jogo é só um clique duplo.

> [!IMPORTANT]
> Você precisa do **seu próprio disco original** de *Need for Speed: Hot Pursuit 2* para PlayStation 2. O projeto não
> inclui arquivos do jogo e não baixa nenhum.

> [!WARNING]
> **Salvar ainda não foi testado.** O jogo grava num cartão de memória guardado como arquivos na pasta do jogo, mas salvar
> e carregar uma carreira não foi verificado do começo ao fim. Não conte ainda com manter seu progresso. Corridas avulsas
> funcionam do começo ao fim.

---

## O jeito rápido: baixar

Desde a versão 0.2.0 existe um download pronto para jogar, então você pode pular a instalação das ferramentas e a
compilação (passos 1, 3 e 4):

1. Baixe o **`HotPursuit2PC-v0.2.0-win64.zip`** na [versão mais recente](https://github.com/awtocl/HotPursuit2PC/releases/latest).
2. Clique com o botão direito nele → **Extrair tudo…** e escolha a pasta que quiser (por exemplo
   `C:\Jogos\HotPursuit2PC`).
3. Abra a pasta e dê um clique duplo em **`HotPursuit2Launcher.exe`**. O Windows pode dizer que protegeu o seu PC (os
   programas não são assinados): clique em **Mais informações** e depois em **Executar assim mesmo**.
4. Depois siga o **passo 2** (seu disco) e o **passo 5** (a preparação no launcher) mais abaixo.

Você vai precisar de uns 4 GB livres, quase tudo para a cópia do seu disco. Prefere compilar você mesmo ou quer as
mudanças mais recentes? Siga todos os passos abaixo.

## O que você precisa

| Você precisa | Detalhes |
|---|---|
| **O disco** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **edição norte-americana**. O código **SLUS-20362** está impresso no disco e no verso da caixa. Outras regiões (por exemplo a europeia `SLES-50731`) e qualquer edição com outro programa não são compatíveis — o launcher confere isso para você. |
| **Um jeito de ler o disco** | Um drive de DVD no PC (ou ligado a ele). Se você já fez uma imagem `.iso` do seu próprio disco, ela também serve. |
| **O PC** | Windows 10 ou 11, 64 bits. Uma placa de vídeo com Direct3D 11 (qualquer uma dos últimos dez anos). |
| **Espaço livre** | Cerca de **15 GB**: 3 GB para a cópia do disco, 6 GB para as ferramentas de compilação C++ (passo 1) e 6 GB para o projeto com o gerador de código (passo 4). |
| **Um controle** *(recomendado)* | Um controle de Xbox, ou qualquer coisa que o Windows veja como um. O teclado também funciona. |
| **Internet** | Para o passo 1 e para a primeira execução do instalador (baixa cerca de 1–2 GB de ferramentas e código-fonte). |

## Passo 1 — Instale as ferramentas (uma vez)

O jogo é compilado no seu PC, então ele precisa de três ferramentas gratuitas da Microsoft e do Git. O jeito mais fácil
é o **winget**, que já vem no Windows 10 e 11.

1. Abra o menu **Iniciar**, digite `Terminal`, clique com o botão direito em **Terminal** e escolha **Executar como
   administrador**.
2. Copie estas três linhas, uma por vez, apertando Enter depois de cada uma. Cada linha leva alguns minutos; espere
   terminar antes de ir para a próxima.

   ```powershell
   winget install --id Microsoft.PowerShell -e
   winget install --id Git.Git -e
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   A terceira linha instala o *Build Tools for Visual Studio 2022* com a carga de trabalho *Desenvolvimento para desktop
   com C++* — o compilador C++ que o projeto precisa (cerca de 6 GB). Uma janela do instalador do Visual Studio mostra o
   progresso.
3. **Reinicie o PC** quando as três terminarem.

<details>
<summary>Prefere instalar à mão?</summary>

- **PowerShell 7:** pela [Microsoft Store](https://apps.microsoft.com/detail/9mz1snwt0n5d) ou pela
  [página do PowerShell](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) da Microsoft.
- **Git para Windows:** em [git-scm.com](https://git-scm.com/download/win); as opções padrão servem.
- **Build Tools for Visual Studio 2022:** em [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
  (em *Ferramentas para o Visual Studio*). No instalador, marque **Desenvolvimento para desktop com C++** e clique em
  *Instalar*. Se você já tem o Visual Studio 2022 com essa carga de trabalho, não precisa instalar nada.

</details>

## Passo 2 — Deixe o disco à mão

O launcher copia o disco para o PC no passo 5, então depois ele não precisa ficar no drive. Antes do passo 5, faça uma
destas coisas:

- **Seu disco:** coloque-o no drive de DVD.
- **Uma imagem `.iso` do seu próprio disco:** clique duas vezes no arquivo `.iso` no Explorador de Arquivos. O Windows a
  mostra como um novo drive de DVD (você pode ejetá-la depois do passo 5).
- **Você já copiou os arquivos do disco para uma pasta:** nada a fazer; você pode escolher essa pasta no passo 5. Ela
  precisa conter `SYSTEM.CNF`, `SLUS_203.62` e a pasta `NFSHP2`.

## Passo 3 — Baixe o HotPursuit2PC (uma vez)

1. Abra o menu **Iniciar**, digite `PowerShell 7` e abra-o (não o antigo "Windows PowerShell").
2. Copie estas linhas, apertando Enter depois de cada uma:

   ```powershell
   cd $HOME\Documents
   git clone https://github.com/awtocl/HotPursuit2PC.git
   cd HotPursuit2PC
   ```

O projeto fica em `Documentos\HotPursuit2PC`. (Use `git clone` e não *Download ZIP*: o Windows bloqueia scripts que vêm
de um ZIP baixado, e o `git` ainda facilita atualizar.)

## Passo 4 — Compile o jogo

Na mesma janela do PowerShell 7, digite:

```powershell
pwsh tools/install-native.ps1
```

O script passa por cinco etapas e diz em qual está (as mensagens dele são em inglês):

| Etapa | O que acontece | Quanto tempo |
|---|---|---|
| 1/5 Your disc | Nada ainda: o launcher pede o disco no passo 5 | — |
| 2/5 LLVM | **Só na primeira vez:** baixa e compila o LLVM, o gerador de código que o recompilador usa | **15–40 min** |
| 3/5 Building | Compila o HotPursuit2PC: o launcher, o jogo e o recompilador | 3–10 min |
| 4/5 Preparing | Nada ainda: o launcher faz isso depois de verificar o disco | — |
| 5/5 Game folder | Coloca os três programas em `game-native\` | segundos |

O PC pode ficar barulhento e ocupado nas etapas 2 e 3; é normal. Quando terminar com **"Done …"** em verde, o launcher
abre sozinho.

## Passo 5 — Configure o jogo no launcher (uma vez)

Na primeira vez, o launcher guia você por quatro passos rápidos:

1. **Disco** — o seu drive de DVD (ou a `.iso` montada) aparece sozinho; escolha **Copiar de …** para copiá-lo para a
   pasta do jogo (cerca de 3 GB, alguns minutos). Ou escolha **uma pasta com os arquivos do disco**. Se o disco for de
   outra região ou edição, o launcher avisa aqui.
2. **Verificar** — cada arquivo é comparado com o disco original.
3. **Preparar** — o programa do jogo é traduzido para código do seu processador (cerca de um minuto).
4. **Pronto** — a qualidade de imagem já está ajustada para a sua tela. Escolha **Jogar agora**.

![A tela inicial do launcher](assets/launcher-home.png)

## Passo 6 — Jogue

Daqui em diante, abra o **`HotPursuit2Launcher.exe`** em `Documentos\HotPursuit2PC\game-native` e escolha **Jogar**.
Para um atalho na área de trabalho: launcher → **Disco e instalação** → **Atalho na área de trabalho**.

O launcher funciona com mouse, teclado (setas, Enter, Esc, Page Up/Page Down) ou controle de Xbox (A seleciona, B volta,
LB/RB páginas, Start joga). As páginas:

| Página | Para que serve |
|---|---|
| **Jogar** | Abrir o jogo; ver se o disco e o jogo estão em ordem; seu tempo de jogo |
| **Configurações** | Imagem e tela, cada opção explicada, com a recomendação para a sua tela |
| **Controles** | Layout de controle Clássico ou Moderno, remapear qualquer tecla ou botão, restaurar o padrão |
| **Cartão de memória** | O que o jogo salvou; fazer backup e restaurar |
| **Capturas de tela** | As imagens que você tirou com **F12** no jogo |
| **Disco e instalação** | Verificar todos os arquivos do disco, preparar o jogo de novo, usar outra cópia do disco, criar um atalho |
| **Sobre** | O que é este projeto, créditos, links para este guia e para as issues |

O jogo começa em tela cheia com os vídeos de abertura; aperte **Enter** (ou Start no controle) para pulá-los. O launcher
se esconde enquanto você joga e volta quando você sai.

### Configurações recomendadas

Mude tudo na página **Configurações** do launcher, ou durante o jogo com **F1** (ou **Select + Start** no controle). A
instalação já escolheu a **Qualidade de imagem** (a resolução interna) para a sua tela; é ela que define a nitidez:

| Sua tela | Qualidade de imagem |
|---|---|
| 1920 × 1080 (Full HD) | **Qualidade (4x)** |
| 2560 × 1440 | **Ultra (6x)** |
| 3840 × 2160 (4K) | **Máximo (8x)** |

Mais alto é mais suave e nítido (desenha mais pixels do que a sua tela tem e faz a média). Se o jogo engasgar, escolha o
valor anterior.

| Opção | Recomendado | Por quê |
|---|---|---|
| Renderizador | **GPU** | O de software é mais lento e fica só para comparação |
| Taxa de quadros | **60** | O dobro dos 30 fps do PS2 nas corridas; o jogo mantém a velocidade normal |
| Modo de exibição | **Tela cheia** | Alt+Enter ou F11 alternam a qualquer momento |
| V-Sync | **Ligado** | Movimento mais estável, sem cortes |
| Filtro de escala | **Bilinear nítido** | Nítido e sem serrilhado |
| Proporção | **16:9** | Visão mais ampla nas corridas; os menus mantêm o formato (trocar entre 16:9 e 4:3 exige reiniciar) |
| Nitidez | **Desligado** ou **Baixo** | Valores maiores podem fazer as bordas tremerem |
| Scanlines | **Desligado** | A não ser que você goste do visual de TV de tubo |
| Antisserrilhado | **FXAA** | Suaviza as bordas serrilhadas; nas qualidades de imagem mais altas a diferença é pequena |

As configurações são salvas sozinhas em `game-native\hp2native.ini`.

### Controles

Escolha um layout na página **Controles** do launcher (ou no jogo com **F1** → *Layout do controle*):

| Ação | Teclado | Controle: Clássico (PS2) | Controle: Moderno |
|---|---|---|---|
| Acelerar | Z | A | RT (ou A) |
| Frear / Ré | A | X | LT |
| Virar | ← → | Analógico esquerdo / direcional | Analógico esquerdo / direcional |
| Freio de mão | W | RB | B |
| Buzina / Sirene | X | B | Clique do analógico esquerdo |
| Mudar câmera | S | Y | Y |
| Olhar para trás | Q | LB | Clique do analógico direito |
| Turbo | E | Clique do analógico direito | X |
| Subir marcha / Zone Zoom | 2 | RT | RB |
| Reduzir marcha / Zone Freeze | 1 | LT | LB |
| Pausa | Enter, Espaço | Start | Start |
| Alternar HUD | Backspace, Shift direito | Back | Back |
| Menu de configurações | F1 | Select + Start | Select + Start |
| Captura de tela | F12 | – | – |

- **Clássico** é o controle do PS2 botão por botão. **Moderno** coloca acelerador e freio nos gatilhos, como os jogos de
  corrida de hoje.
- **Monte o seu:** na página **Controles**, escolha uma caixa e aperte a tecla ou o botão que quiser. Cada ação aceita
  duas teclas e dois botões do controle. Esc cancela, Delete apaga uma tecla. A primeira mudança cria um layout
  *Personalizado* a partir do que você estava usando.
- **Mudou de ideia?** **Restaurar teclado padrão** e **Restaurar controle padrão** voltam direto aos layouts de fábrica.
  O seu layout personalizado fica salvo; escolha *Personalizado* para usá-lo de novo.
- Os analógicos funcionam como os do DualShock 2. O Windows não reconhece sozinho controles de PlayStation como
  controles de jogo; eles funcionam por meio de uma ferramenta que os apresenta como controle de Xbox, como o suporte a
  controles do Steam.

### Idioma

O launcher e o menu de configurações do jogo estão em inglês, espanhol, português (Brasil), francês, alemão e italiano.
Na primeira vez, eles seguem o idioma do seu Windows; mude no launcher em **Configurações → Idioma** ou no jogo com
**F1**. Os textos do próprio jogo continuam em inglês: é assim que vem o disco norte-americano.

### Sair

**F1 → Sair do jogo** (aperte duas vezes para confirmar), ou **Alt+F4**.

## Atualizar

Quando o projeto tiver novidades, atualize e compile de novo — suas configurações, saves e cópia do disco continuam lá:

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1
```

O LLVM não é compilado de novo, então leva só alguns minutos. Depois o launcher mostra **Preparar e jogar**: o jogo é
preparado de novo para a nova versão (cerca de um minuto) e começa.

## Se algo der errado

| O que você vê | O que fazer |
|---|---|
| *"The C++ build tools were not found"* | O passo 1 não está completo: instale as Build Tools com **Desenvolvimento para desktop com C++**, reinicie o PC e tente de novo. |
| *"pwsh não é reconhecido"* | O PowerShell 7 não está instalado, ou o PC não foi reiniciado depois do passo 1. |
| O launcher não encontra o disco | Espere alguns segundos depois de colocá-lo; com uma `.iso`, clique duas vezes nela antes. Ou copie você mesmo os arquivos do disco para uma pasta e escolha **uma pasta com os arquivos do disco**. |
| *"Esta pasta não contém um disco de PlayStation 2"* ou *"Falta …"* | Você escolheu a pasta errada, ou a cópia está incompleta. Escolha a pasta que contém diretamente `SYSTEM.CNF`, `SLUS_203.62` e `NFSHP2`. |
| *"Este disco é SLES-…"* ou *"… não é a versão compatível"* | O seu disco é de outra região ou edição. Só a edição norte-americana `SLUS-20362` é compatível. |
| *"… difere do disco original"* | A cópia do disco está danificada (muitas vezes por um disco riscado). Limpe o disco e copie de novo (**Disco e instalação** → **Trocar disco**). |
| *"A preparação do jogo falhou"* | Os detalhes estão em `game-native\prepare.log`; anexe-o a uma [issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"O jogo parou: …"* na página Jogar | Escolha **Abrir log** e **Relatar um problema**; anexe o `native.log`. |
| *"Downloading LLVM failed"* | Confira sua conexão com a internet e rode o instalador de novo. |
| *"The build failed"* | Copie as linhas vermelhas e [abra uma issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"…não pode ser carregado porque a execução de scripts foi desabilitada"* | Você baixou um ZIP em vez de usar `git clone`. Use `git clone` (passo 3). |
| A janela do jogo fica preta, ou a imagem está errada | Launcher → **Configurações** → Renderizador → **Software**. Se isso ajudar, relate informando o modelo da sua placa de vídeo. |
| Sem som | O jogo usa o dispositivo de reprodução padrão do Windows: confira nas configurações de som do Windows e reinicie o jogo. |
| O controle não faz nada | Só controles compatíveis com Xbox (XInput) são reconhecidos; veja [Controles](#controles). O teclado sempre funciona. |
| Um botão faz a coisa errada | Launcher → **Controles** → **Restaurar controle padrão** (ou **Restaurar teclado padrão**). |
| Engasga | Diminua a **Qualidade de imagem** (launcher → Configurações) e confira se o V-Sync está ligado. |
| Tudo ficou estranho depois de mudar configurações | Launcher → **Configurações** → **Restaurar padrão**. |

**Relatar um problema:** depois que o jogo fecha, o arquivo `game-native\native.log` descreve o que aconteceu. Anexe-o a
uma [issue](https://github.com/awtocl/HotPursuit2PC/issues) com a sua versão do Windows e a sua placa de vídeo. O log não
tem dados pessoais além de caminhos de pastas, que você pode apagar.

## Desinstalar

Este projeto não instala nada no Windows. Para removê-lo, apague a pasta `Documentos\HotPursuit2PC` (as ferramentas de
compilação ficam dentro dela, em `.cache`, e a cópia do seu disco em `game-native\disc`). Antes, faça uma cópia de
`game-native\saves` se quiser guardar seus saves. Um atalho criado pelo launcher pode ser apagado como qualquer outro. As
ferramentas do passo 1 são removidas em *Configurações → Aplicativos* como qualquer outro programa.

---

Travou, ou algo aqui não ficou claro? [Abra uma issue](https://github.com/awtocl/HotPursuit2PC/issues) e conte pra gente —
este guia deve funcionar para todo mundo. Divirta-se na estrada, e cuidado com a polícia!
