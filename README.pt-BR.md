<p align="center">
  <img src="docs/assets/banner.svg" alt="HotPursuit2PC" width="100%">
</p>

<p align="center">
  <a href="https://github.com/awtomi/HotPursuit2PC/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/awtomi/HotPursuit2PC/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <img alt="Plataforma: Windows x64" src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-1d3a5f">
  <img alt="Linguagem: C++20" src="https://img.shields.io/badge/C%2B%2B-20-1d3a5f">
  <a href="LICENSE"><img alt="Licença: MIT" src="https://img.shields.io/badge/license-MIT-ff921c"></a>
  <img alt="Status: versão em desenvolvimento" src="https://img.shields.io/badge/status-development%20build-ff921c">
  <a href="#uma-palavra-sobre-ia"><img alt="Feito com ajuda de IA" src="https://img.shields.io/badge/AI--assisted-Claude%20Code-6a5acd"></a>
</p>

<p align="center">
  <b>A versão de PlayStation 2 de <i>Need for Speed: Hot Pursuit 2</i>, rodando nativamente no Windows — feita a partir do seu próprio disco.</b>
</p>

<p align="center">
  <a href="https://github.com/awtomi/HotPursuit2PC/releases/latest"><b>Baixar</b></a> ·
  <a href="docs/GETTING-STARTED.pt-BR.md"><b>Primeiros passos</b></a> ·
  <a href="#início-rápido">Início rápido</a> ·
  <a href="#o-que-vem">O que vem</a> ·
  <a href="#controles">Controles</a> ·
  <a href="#problemas-conhecidos">Problemas conhecidos</a> ·
  <a href="#aviso-legal-e-procedência">Aviso legal</a> ·
  <a href="#uma-palavra-sobre-ia">Uma palavra sobre IA</a>
</p>

<p align="center">
  🌐 <a href="README.md">English</a> · <a href="README.es.md">Español</a> · <b>Português (Brasil)</b> ·
  <a href="README.fr.md">Français</a> · <a href="README.de.md">Deutsch</a> · <a href="README.it.md">Italiano</a>
</p>

> [!IMPORTANT]
> **Nenhum arquivo do jogo vem incluso, e nenhum é baixado.** Você precisa do **seu próprio disco** de *Need for Speed:
> Hot Pursuit 2* para PlayStation 2 (América do Norte, `SLUS-20362`).

> [!NOTE]
> Feito com muita ajuda de IA (Claude Code). Curiosidade ou desconfiança? Aqui está [o que eu penso sobre isso](#uma-palavra-sobre-ia).

---

## Por quê?

> A versão de PS2 é a melhor versão deste jogo, então é ela que este projeto traz para o PC.

*Hot Pursuit 2* saiu em vários consoles, e a versão de PS2 é a que o pessoal lembra: a dirigibilidade, os carros, as
pistas, a sensação. Isto não é um remake, nem a versão de PC de 2003 (que é bem diferente). É exatamente aquele jogo de
PS2, transformado num programa de Windows de verdade: código x64 nativo, a resolução do seu monitor, o seu controle — e
nenhum emulador no meio.

## O que vem

| | |
|---|---|
| **Sem emulador** | O código do próprio jogo é traduzido para código x64 nativo (com LLVM), no seu PC e a partir da sua cópia. O hardware do console é trocado por equivalentes de PC: seus arquivos no lugar do leitor de disco, sua placa de vídeo, um mixer de som, seu controle. |
| **Gráficos nítidos** | Direct3D 11 de 1x a 8x a resolução original (com supersampling quando passa da sua tela), escala nítida, antisserrilhado (FXAA), nitidez e scanlines opcionais, tela cheia sem bordas. |
| **16:9 e 60 fps** | Widescreen de fábrica (você vê mais da estrada; o HUD e os menus mantêm o formato) e 60 quadros por segundo, graças aos patches da comunidade para este jogo (veja [Créditos](#licença-e-créditos)). Os dois podem ser desligados. |
| **Todo o som** | As músicas do EA Trax, motores e efeitos, o rádio da polícia e o som dos vídeos. |
| **Um launcher** | Encontra seu disco, copia e verifica, prepara o jogo e lembra de tudo depois. E ainda: configurações com recomendação para a sua tela, backups do cartão de memória e uma galeria de capturas (F12). Funciona com mouse, teclado ou controle. |
| **Seus controles, do seu jeito** | Um layout de controle Clássico (PS2) e um Moderno (gatilhos), e qualquer tecla ou botão pode ser remapeado. Volte ao padrão com um clique. |
| **Seis idiomas** | O launcher e o menu do jogo falam inglês, espanhol, português, francês, alemão e italiano (na primeira vez, conforme o seu Windows). |
| **Configurações no jogo** | Aperte **F1** (ou Select+Start) para um menu de configurações no estilo do próprio jogo, com *Sair do jogo*. |
| **Seus arquivos são seus** | Seu disco só é lido. Tudo o que é feito a partir dele fica no seu PC. |

<p align="center"><img src="docs/assets/launcher-home.png" width="760" alt="O launcher do HotPursuit2PC: a página Jogar com o status do disco e do jogo, um grande botão JOGAR e as teclas do jogo"></p>

## Início rápido

**Baixe e jogue**, sem ferramentas de compilação:

1. Pegue o **`HotPursuit2PC-v0.2.0-win64.zip`** na **[versão mais recente](https://github.com/awtomi/HotPursuit2PC/releases/latest)** e descompacte onde quiser.
2. Abra o **`HotPursuit2Launcher.exe`**. Os programas não são assinados, então o Windows pode perguntar antes: *Mais
   informações* → *Executar assim mesmo*.
3. Coloque **seu próprio disco** no drive (ou monte a sua `.iso`). O launcher copia e verifica o disco e traduz o jogo
   **a partir da sua cópia, no seu PC** (cerca de um minuto). Depois é só apertar **Jogar**.

O que mudou em cada versão: [CHANGELOG.md](CHANGELOG.md) (em inglês).

> [!TIP]
> **Primeira vez fazendo algo assim?** Sem problema: o guia **[Primeiros passos](docs/GETTING-STARTED.pt-BR.md)** te
> leva passo a passo: o que você precisa, instalar as ferramentas, compilar, as melhores configurações e o que fazer se
> algo der errado.

**Ou compile você mesmo:** no Windows 10/11 x64 com *Build Tools for Visual Studio 2022* (carga de trabalho C++), Git e
PowerShell 7 instalados, e **seu próprio disco** no drive (ou a sua `.iso` montada):

```powershell
git clone https://github.com/awtomi/HotPursuit2PC.git
cd HotPursuit2PC
pwsh tools/install-native.ps1       # compila tudo e depois abre o launcher
```

Na primeira vez ele compila o LLVM (pegue um café: de 15 a 40 minutos), depois o projeto, e coloca o launcher, o jogo e
o recompilador em `game-native\`. Daí em diante é com o launcher: ele copia e verifica seu disco e traduz o jogo **a
partir da sua cópia, no seu PC** (cerca de um minuto). Depois disso, é só abrir o **`HotPursuit2Launcher.exe`**. Mais ou
menos uma hora na primeira vez; alguns minutos depois de uma atualização.

O download traz só os programas deste projeto. O jogo traduzido é sempre feito a partir da sua própria cópia, no seu
próprio PC: ele nunca é distribuído.

## Controles

Escolha um layout ou monte o seu:

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

*Clássico* é o controle do PS2 botão por botão; *Moderno* coloca acelerador e freio nos gatilhos, como os jogos de
corrida de hoje. Para mudar algo, abra a página **Controles** do launcher, escolha uma caixa e aperte a tecla ou o botão
que quiser (dois de cada por ação). **Restaurar teclado padrão** e **Restaurar controle padrão** trazem os layouts de
fábrica de volta quando você quiser, e o seu layout personalizado fica salvo para depois. O layout do controle também
pode ser trocado dentro do jogo com **F1**.

Também útil: **F1** menu de configurações, **F12** captura, **Alt+Enter** ou **F11** tela cheia, **Alt+F4** ou *Sair do
jogo* no menu de configurações para sair.

## Problemas conhecidos

É uma das primeiras versões e até agora só foi testada em um PC. Eis o que você pode encontrar:

- **Salvar não foi testado.** O jogo tem um cartão de memória (arquivos em `game-native\saves`), mas salvar e carregar
  uma carreira ainda não foi verificado do começo ao fim.
- **Som:** ainda sem reverberação e sem envelopes de volume de verdade, então alguns sons param meio de repente.
- **Gráficos:** ainda sem névoa de profundidade, mipmaps, dithering ou linhas; a névoa de distância nas corridas é pulada
  por enquanto.
- **Controles:** ainda sem vibração e sem volante. O jogo lê acelerador e freio como botões liga/desliga, então os
  gatilhos também funcionam como botões.
- **Idioma:** os textos do próprio jogo continuam em inglês (é assim que este disco norte-americano vem); o launcher e o
  menu de configurações seguem o seu idioma.
- A troca entre 16:9 e 4:3 só vale depois de reiniciar (o jogo monta o HUD uma vez só).
- Ainda não testado em placas AMD ou Intel, num Windows recém-instalado ou em sessões bem longas.

Achou outra coisa? [Abra uma issue](https://github.com/awtomi/HotPursuit2PC/issues): ajuda muito.

## Aviso legal e procedência

> [!NOTE]
> Esta tradução é para a sua conveniência. A versão de referência é a em inglês:
> [Legal position and provenance](README.md#legal-position-and-provenance).

Leia esta seção antes de usar ou redistribuir qualquer coisa.

**Não há nenhuma propriedade da Electronic Arts neste repositório.** Cada arquivo de código-fonte foi escrito para este
projeto e é publicado sob a licença MIT (veja [`LICENSE`](LICENSE)). O repositório não contém código, recursos, textos,
áudio, vídeo nem outro material de propriedade da Electronic Arts Inc., da Sony Interactive Entertainment ou de qualquer
outro detentor de direitos do jogo, nem nenhuma parte do executável do jogo em qualquer forma (original, desmontada,
descompilada ou traduzida).

Para que isso possa ser conferido, eis tudo no repositório que foi *derivado* do disco — tudo são fatos sobre o disco,
nada do seu conteúdo:

- [`converter/data/slus20362_rom_manifest.tsv`](converter/data/slus20362_rom_manifest.tsv): nomes, tamanhos e hashes
  SHA-256 dos 27 arquivos do disco, para diferenciar uma cópia boa de uma danificada ou diferente.
- [`assets-pipeline/data/hp2_slus20362_names.txt`](assets-pipeline/data/hp2_slus20362_names.txt): os nomes de 362
  arquivos dentro do pacote do jogo, recuperados para que os arquivos extraídos tenham nomes legíveis.
- [`tests/fixtures/slus20362_zfileset_inventory.tsv`](tests/fixtures/slus20362_zfileset_inventory.tsv): posições,
  tamanhos e hashes SHA-256 de cada arquivo dentro desse pacote, usados pelos testes.
- [`docs/dump-analysis.md`](docs/dump-analysis.md): uma descrição do formato desse contêiner e o inventário completo de
  arquivos.
- [`docs/recomp-analysis.md`](docs/recomp-analysis.md) e `recomp/tests/test_recomp_dump.cpp`: medições do programa do
  jogo (endereços e tamanhos de seções, quantidades de instruções, funções e tabelas de salto, alguns endereços de
  código). Nenhuma instrução ou byte do programa em si.
- [`recomp/include/hp2/game_patches.h`](recomp/include/hp2/game_patches.h): os endereços e valores dos patches de 16:9
  e 60 fps (veja [Créditos](#licença-e-créditos)).

**Você precisa trazer a sua própria cópia.** Para usar estas ferramentas você precisa de uma cópia de um exemplar do
jogo que *você* possui legalmente, feita por você a partir do seu próprio disco. Este projeto não fornece, não aponta e
não ajuda a encontrar arquivos do jogo, imagens de disco ou firmware de console, e não fará isso. Nenhuma BIOS de
PlayStation 2 é necessária ou usada. As leis sobre fazer e usar cópias de segurança de software que você possui variam
entre países; você é responsável por cumprir as que se aplicam a você.

**Como foi testado.** Todo o desenvolvimento e os testes foram feitos com uma única cópia: a que quem mantém o repositório
fez do disco original em sua posse. Nenhuma outra cópia do jogo, nenhuma imagem baixada e nenhum material da versão de
Windows foi usado em momento algum. As ferramentas abrem a cópia somente para leitura e nunca a modificam.

**A tradução acontece na sua máquina.** Tudo o que as ferramentas derivam do jogo — o código recompilado, os recursos
extraídos — é produzido no seu próprio computador a partir da sua própria cópia e fica lá. Não redistribua.

**Sem vínculo.** Este é um projeto independente, sem fins comerciais, de preservação e interoperabilidade. Ele não tem
vínculo com a Electronic Arts, a Sony ou os autores do software livre em que se apoia, nem é patrocinado ou endossado por
eles. *Need for Speed* e *Hot Pursuit 2* são marcas da Electronic Arts Inc.; *PlayStation* é uma marca da Sony
Interactive Entertainment Inc. São usadas aqui só para dizer com o que as ferramentas são compatíveis.

Se você é detentor de direitos e acha que algo aqui não deveria estar, abra uma issue.

## Uma palavra sobre IA

> Eu sei que muita gente odeia IA, e cedo ou tarde tudo o que foi tocado, mesmo que de leve, por um modelo de IA acaba
> sendo chamado de "AI slop". Minha posição é esta: a IA não vai embora. É uma ferramenta nova que nos deixa fazer coisas
> que antes nem achávamos possíveis, e não vou deixar uma ideologia me impedir de colocá-la no meu jeito de trabalhar.
> Ame ou odeie, a IA consegue escrever um código muito bom. Então aproveite para deixar sua vida um pouco mais fácil,
> descubra no caminho para que ela serve — e, no meu caso, mergulhe em projetos que trazem os jogos da minha infância
> para o PC.

Comecei o HotPursuit2PC para aprender na prática a criar software junto com uma IA, em algo difícil o bastante para ser
um teste de verdade. Assim foi a divisão do trabalho:

- **Um agente de programação com IA escreveu o código, os testes, os scripts e a documentação** — o Claude, da
  Anthropic, no Claude Code — sob a minha direção. Os commits levam uma linha `Co-Authored-By: Claude`.
- **Eu defini os objetivos e as regras** (estão em [`docs/brief.md`](docs/brief.md)), fiz a cópia do meu próprio disco,
  aprovei cada passo que saía do projeto (como baixar código de terceiros), testei e joguei cada versão e tomei as
  decisões — por exemplo, trocar a versão com emulador pela recompilação estática depois de jogá-la.
- **O que sabemos do jogo vem da minha própria cópia**, por inspeção e experimentos, com as evidências registradas em
  [`docs/dump-analysis.md`](docs/dump-analysis.md). O código-fonte do jogo nunca esteve disponível nem foi usado.

E, para ser justo com você: código escrito por IA pode errar com toda a confiança. Por isso o projeto se apoia no
processo, não na confiança: cada afirmação sobre o disco tem uma verificação ou um teste por trás, o que não foi provado
é marcado como **UNVERIFIED** e cada decisão fica registrada com as alternativas. Ninguém revisou cada linha à mão: dê
uma olhada antes de confiar.

## Para desenvolvedores

Como funciona por dentro, como compilar a partir do código, a estrutura do repositório e toda a documentação técnica
estão no [README em inglês](README.md#how-it-works) e em [`CONTRIBUTING.md`](CONTRIBUTING.md). As decisões de projeto
estão em [`docs/adr/`](docs/adr/).

## Próximos passos

- [ ] Saves confiáveis (como arquivos de PC)
- [ ] Reverberação e envelopes de verdade no mixer de som
- [ ] Névoa de profundidade e mipmaps no renderizador por GPU
- [ ] Vibração e volantes
- [ ] Textos e texturas mais nítidos
- [x] Um download pronto para jogar, sem ferramentas de compilação
- [ ] Movimento mais suave acima de 60 fps

É um caminho longo, então sem datas: fica pronto quando ficar.

## Licença e créditos

O código e a documentação deste repositório são publicados sob a [licença MIT](LICENSE).

- **Patches do jogo:** os valores de 16:9 e 60 fps vêm do arquivo de patches da comunidade para este disco em
  [PCSX2/pcsx2_patches](https://github.com/PCSX2/pcsx2_patches) (`SLUS-20362_1D2818AF.pnach`): *Widescreen 16:9* de
  **nemesis2000**, *HUD fixed for widescreen* de **Aero_** e *60 FPS* de **asasega**. Só os endereços e valores foram
  usados, reimplementados como patches que podem ser ligados e desligados. Obrigado!
- **LLVM** (Apache 2.0 com exceções LLVM): o gerador de código do recompilador.
- **Play!** de Jean-Philip Desjardins (BSD 2-Clause): o núcleo de emulador do launcher de referência e suas bibliotecas
  (detalhes em [`docs/prior-art.md`](docs/prior-art.md)). O jogo nativo não contém código de terceiros; ele usa o
  Direct3D 11, o XAudio2 e o XInput do Windows.
