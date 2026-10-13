# Metal Gear Ghost Babel Recomp

Roda **Metal Gear Solid: Ghost Babel** (Game Boy Color) como programa nativo
no seu PC — com as mesmas entradas e a mesma emulação de hardware, sem
emulador por cima e sem nada do jogo dentro do repositório. A ROM é a sua.

```
git clone https://github.com/<seu-usuario>/Metal-Gear-Ghost-Babel-Recomp.git
cd Metal-Gear-Ghost-Babel-Recomp
./bootstrap.sh
./build.sh ~/minha-rom.gbc
./run.sh
```

Só isso. Três comandos.

---

## O que este projeto faz (e o que não faz)

A Game Boy original roda a 4,19 MHz num processador de 8 bits com 8 KiB de
memória. Emuladores reimplementam esse processador e o *interpretam*. Aqui
fazemos o contrário: a ROM é **traduzida uma vez para código C nativo**, e o
programa resultante roda direto na sua máquina, sem interpretador no caminho
quente.

| | emulador comum | aqui |
|---|---|---|
| como roda | interpreta a ROM a cada frame | a ROM vira C, o C roda nativo |
| velocidade | depende de quanto a emulação é boa | folgado acima de 60 fps (medido ~2700 fps sem janela) |
| precisa de ROM na execução | precisa | **não** — vira executável |

O que **não** vem aqui: a ROM, o código do jogo, os gráficos, a trilha. Nada
disso é nosso para distribuir. O que vem são as **ferramentas** — o tradutor e
o runtime — que qualquer pessoa usa com a própria cópia do jogo.

> Você precisa ter uma cópia legítima do jogo. Este projeto não baixa, não
> vende e não inclui nada do jogo — nem a ROM, nem o resultado de compilar a
> ROM. É a mesma situação de qualquer emulador.

---

## O que você precisa ter antes

Quatro coisas, todas de installs de uma linha:

| precisa | já vem na maioria dos sistemas? |
|---|---|
| `git` | sim |
| compilador C/C++ | às vezes (`build-essential` no Debian, `base-devel` no Arch) |
| `make` | às vezes (vem com o compilador) |
| headers do SDL2 | não — é o único pacote a instalar |
| Python 3 | sim |

Se você usa Arch, Debian/Ubuntu, Fedora, Linux Mint ou openSUSE, o
`./bootstrap.sh` detecta e **imprime o comando exato** do que falta. Você não
precisa adivinhar nome de pacote.

---

## Passo a passo

### 1. Instalar o que falta (só se aparecer aviso)

O `bootstrap.sh` diz o que instalar. O comando também é:

```bash
# Arch / Omarchy
sudo pacman -S --needed base-devel pkgconf sdl2

# Debian / Ubuntu / Mint
sudo apt-get install -y build-essential pkg-config libsdl2-dev

# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```

### 2. `./bootstrap.sh`

Baixa o motor (versão fixada por hash, com um patch versionado que audita
linha a linha), compila as ferramentas e a biblioteca de execução. **Uns 30
segundos** na primeira vez. É só rodar de novo uma vez instalado.

### 3. `./build.sh caminho/da/rom.gbc`

- confere se o arquivo é mesmo uma ROM de Game Boy;
- traduz a ROM para ~283 arquivos de C;
- compila e linka;
- **roda o jogo por 120 frames** para garantir que nasceu vivo.

Uns 3 a 4 minutos. É o preço de traduzir 2 MB de ROM para código nativo — não
é um travamento, é compilação de verdade.

### 4. `./run.sh`

Abre o jogo.

Preferir uma janela em vez de digitar caminho? `./run.sh --gui` faz o mesmo
passo por uma interface gráfica, com o log sempre à vista.

---

## Como se joga

O teclado é o controle. Não há mouse, porque o jogo não tinha.

| tecla | ação |
|---|---|
| setas ou `WASD` | movimento e mira |
| `Z` ou `J` | botão A |
| `X` ou `K` | botão B |
| `Enter` | Start |
| `Backspace` ou `Shift` direito | Select |
| `Tab` | turbo (pula as esperas) |
| `F1` / `F2` | overlay de diagnóstico / UI |
| `F5` / `F8` | salvar / carregar posição |
| `M` | mudo |
| `Esc` | sair |

Controle de videogame USB funciona normalmente.

---

## Se algo der errado

Cada mensagem de erro deste projeto diz **o que fazer**, não só o que houve.
Se aparecer algo que não resolve sozinho:

```bash
cat .build.log        # log da última compilação
cat .bootstrap.log    # log da preparação
```

E, se for bug nosso, reporte no GitHub com o log. Log vale mais que descrição.

---

## Perguntas honestas

**Funciona igual ao original?**
Nos testes de hardware do GB (Blargg e Mooneye) o runtime passa 75 de 75, e a
comparação frame a frame com um emulador de referência coincide. Onde ainda há
diferença de 3–4 pixels em cenas específicas, ela é documentada e conhecida.

**Preciso de uma ROM específica?**
Não. A ferramenta funciona com qualquer ROM de Game Boy. Para o Ghost Babel
existe um perfil de análise (`profiles/ghost-babel.json`) com três endereços
de entrada que o analisador não descobre sozinho — só endereços, nenhum dado
do jogo.

**Vai ter jogável no futuro?**
Android e Windows estão planejados. Linux primeiro, porque é o único caminho
testado de verdade.

**Posso redistribuir o executável que ele gera?**
Não — e nem tentamos. O executável gerado *é o jogo*. Este repositório
distribui só ferramentas.

---

## Como funciona por dentro

```
sua-rom.gbc
   │
   ├─ gbrecomp ............. ROM → 283 arquivos C (~270 MB de código)
   │
   ├─ make ................. compila o C gerado
   │
   └─ libgbrt.a ............ runtime: CPU, PPU, timers, APU, MBC5, SDL
                 │
                 ▼
          build/ghost-babel        ← executável nativo, ~75 MB
```

O `gbrecomp` é o [`gb-recompiled`](https://github.com/arcanite24/gb-recompiled)
(MIT), com um patch deste repositório
([`patches/`](patches/gb-recompiled-ghost-babel.patch)) que corrige o
comportamento de hardware em CGB e o DIV pós-boot por revisão de boot-ROM.
As versões ficam fixadas em [`pins.lock`](pins.lock) e a integridade é conferida
por hash — o motor não muda por baixo dos panos.

## Licença

Ferramentas: MIT. Veja [LICENSE](LICENSE).

O jogo não tem licença nossa. O que é do jogo é de quem o fez — aqui só
ferramentas.