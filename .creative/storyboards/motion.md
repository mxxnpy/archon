# Storyboard — Archon · direção Halo (iOS, Android, popup Windows)

> Status: **RASCUNHO v3** (motion-director, 13/11/2026). Alinhado à direção escolhida pelo dono do projeto, **Halo** (`.creative/directions/direction-b.html`; a versão refinada b2 está em produção). Aguarda `APROVADO` do `creative-director`.
> Histórico: v1 "console motorizado" (descartada), v2 "simples" (base desta). Pedido do dono do projeto: poucos elementos, motion simples e moderno, nada vintage, nada de mesa de som.

## Cabeçalho

- Direção: **Halo**. "Volume é luz": o arco aceso é o valor; só o que toca brilha; tudo é redondo; o anel do pareamento vira o halo do mixer.
- Componentes que este storyboard anima (só estes):
  - **Radar** (Computadores): órbitas concêntricas, PCs como pontos; abaixo, um card por PC.
  - **Anel do código** (Pareamento): 6 orbes de dígito sobre um anel; o arco avança n/6.
  - **Halo master** (Mixer): arco grande em volta do valor em %.
  - **Card de app** (grade de 2 colunas): anel em volta do ícone (arco = volume), nome, linha de estado ("tocando · 45%" / "mudo · 45%"), botão de mudo redondo.
  - **Pílula** de estado (conexão, dispositivo de saída).
  - **Popup do Windows** com código e anel de contagem.
- Personalidade: **calma, luminosa e precisa**. Fades, escalas curtas e o arco que se preenche. Sem bounce visível e sem efeito decorativo em loop.
- Degrau: animação nativa (SwiftUI `withAnimation`, `.transition`, `.contentTransition(.numericText())`, `matchedGeometryEffect`, `Shape.trim`; Compose `animate*AsState`, `Animatable`, `AnimatedContent`, `SharedTransitionLayout`, `drawArc`). `Canvas` só para arcos e brilho. Win32: Direct2D + janela em camadas, timer apenas enquanto algo se move.
- Aparelho de referência: **iPhone 14 Plus, 60 Hz** (sem ProMotion) → 16,7 ms por quadro. 120 Hz suportado, nunca exigido.

### Convenção dos arcos

Todo arco é um `trim(from: 0, to: v)` de um círculo (SwiftUI `Circle().trim`, Compose `drawArc(sweepAngle = v·360°)`), com início e espessura definidos pela direção (trilho `track`, arco com gradiente `accent`). Volume `u16 0..10000` → `v = volume / 10000`. Animar **só** `v`, opacidade e escala; nunca o tamanho do layout.

## 1. Regras

1. **O dedo sempre ganha.** No arraste, o arco segue o dedo 1:1, sem animação, e nada vindo do PC move aquele controle.
2. **Mudança do PC é distinguível** (S4.6): arco desliza com spring crítico + legenda curta; dedo não tem deslize e tem haptic.
3. **Parado quando nada muda.** Mixer aberto, sem som e sem mudanças = zero animações e zero display links.
4. **Nada bloqueia o toque.** Entradas nunca seguram a interação.
5. **Curto.** ≤ 300 ms para quase tudo; morph anel → halo 500 ms; abertura do app ≤ 600 ms.
6. **Loops com teto de 5 s** (WCAG 2.2.2); nada acima de 3 Hz (WCAG 2.3.1). Onde a direção sugere loop contínuo (feixe do radar, arco girando, respiração de 2,4 s), este storyboard limita ou troca por estado estático: ver "Ajustes à direção".
7. **Reduced motion preserva a informação** por cor, texto, ícone e crossfade curto ou corte.

### Momentos-assinatura

| # | Momento | Cena |
| --- | --- | --- |
| H1 | feixe do radar dá uma volta e cada PC "acende" na sua órbita | S2 |
| H2 | cada dígito acende um orbe e o arco avança 1/6 | S3.3 |
| H3 | o anel do código fecha e vira o halo master | S3.5 |
| H4 | o arco do app se preenche no volume e só brilha quando toca, no ritmo do som | S4, S6 |

### Ajustes à direção (para o art-director confirmar)

| Na direção B | Neste storyboard | Motivo |
| --- | --- | --- |
| feixe do radar gira sem parar (4 s/volta) | uma volta de 4 s ao entrar ou ao "procurar de novo", depois o feixe some | WCAG 2.2.2 e bateria |
| anel de ping infinito nos pontos (1,6 s) | um ping só quando o PC aparece ou fica online | loop sem função |
| arco girando enquanto verifica | gira até 5 s, depois fica parado com "Verificando…" | WCAG 2.2.2 |
| cursor piscando no próximo orbe | orbe seguinte com contorno `accent` estático | piscar não informa nada que o contorno não informe |
| respiração de 2,4 s pelo flag "tocando" (sem dado de nível) | brilho estático quando toca; nível real via protocolo 1.1 (S6) | os medidores existem no protocolo; agente 1.0 fica estático |
| entrada dos cards 500 ms, 70 ms por card, overshoot nos orbes `(.34,1.56,.64,1)` | 220 ms, 35 ms por card (teto 240), `spring.pop` com 1,9% de overshoot | grade com muitos apps esperaria > 1 s; bounce forte foge do "simples" |
| popup do Windows com overshoot `(.34,1.3,.64,1)` | 220 ms `ease.enter`, sem overshoot | idem |

## 2. Tokens

### 2.1 Durações

| Token | ms | Uso | Saída |
| --- | --- | --- | --- |
| `motion.tap` | 90 | press, contorno | 70 |
| `motion.fast` | 150 | cor, ícone, texto, pílula | 110 |
| `motion.base` | 220 | card, ponto do radar, avanço do arco do código | 160 |
| `motion.slow` | 300 | card de estado, folha | 220 |
| `motion.screen` | 400 | transição entre telas | 300 |
| `motion.morph` | 500 | anel → halo (H3) | — |
| `motion.launch` | 600 | abertura (teto) | — |

Saída ≈ 75% da entrada, curva `ease.exit`.

### 2.2 Curvas

| Token | cubic-bezier | SwiftUI | Compose | Uso |
| --- | --- | --- | --- | --- |
| `ease.standard` | (0.2, 0, 0, 1) | `.timingCurve(0.2, 0, 0, 1, duration:)` | `CubicBezierEasing(0.2f, 0f, 0f, 1f)` | cor, arco do código, transições |
| `ease.enter` | (0, 0, 0, 1) | `.timingCurve(0, 0, 0, 1, duration:)` | `CubicBezierEasing(0f, 0f, 0f, 1f)` | entradas |
| `ease.exit` | (0.3, 0, 1, 1) | `.timingCurve(0.3, 0, 1, 1, duration:)` | `CubicBezierEasing(0.3f, 0f, 1f, 1f)` | saídas |
| `ease.inout` | (0.65, 0, 0.35, 1) | `.timingCurve(0.65, 0, 0.35, 1, duration:)` | `CubicBezierEasing(0.65f, 0f, 0.35f, 1f)` | morph anel → halo |
| `ease.linear` | (0, 0, 1, 1) | `.linear(duration:)` | `LinearEasing` | feixe do radar, giro do "verificando" |

Compose: `tween(durationMillis = d, easing = …)`.

### 2.3 Springs (1:1)

Conversão com massa 1: `stiffness = (2π / response)²`, `dampingRatio = dampingFraction`.

| Token | SwiftUI `.spring(response:dampingFraction:)` | Compose `spring(dampingRatio, stiffness)` | Overshoot | Assenta (~2%) | Uso |
| --- | --- | --- | --- | --- | --- |
| `spring.press` | 0.24 / 0.86 | 0.86f / 685f | 0,5% | ~180 ms | ponta do arco ao tocar/soltar, press |
| `spring.move` | 0.32 / 0.82 | 0.82f / 386f | 1,1% | ~250 ms | pílulas, seletor, ponto mudando de órbita |
| `spring.list` | 0.45 / 0.90 | 0.90f / 195f | 0,15% | ~320 ms | reflow da grade e da lista |
| `spring.pop` | 0.30 / 0.78 | 0.78f / 439f | 1,9% | ~260 ms | orbe do dígito, ponto do radar, check |
| `spring.remote` | 0.50 / 1.00 | 1.0f / 158f | 0% (crítico) | ~460 ms | valor alterado pelo PC |

`visibilityThreshold` no Compose: 0.001f para `v`, 0.5f para px. No iOS use sempre a forma `response/dampingFraction`.

## 3. Estados e cenas

```
S1 abertura ─▶ S2 Radar ─ponto/card não pareado─▶ S3 Anel do código ─sucesso (H3)─▶ S4 Mixer
                 ▲   └── pareado (connecting) ─────────────────────────────────────▲   │ queda
                 └──── pareamento removido (S4.10) ◀── failed ◀── reconnecting (S4.8) ◀┘
```

| Estado do cliente | Cena |
| --- | --- |
| idle | S2 |
| connecting | S3.1 (pareamento) / S4.1 (resume) |
| awaiting_code | S3.2, S3.3 |
| verifying | S3.4 |
| connected | S3.5 → S4 |
| reconnecting | S4.8 |
| failed: wrong_code / pairing_locked / pairing_rejected | S3.6 / S3.7 / S3.7 |
| failed: busy / timeout / connect_failed | S3.7 (pareamento) ou S4.9 (mixer) |
| failed: connection_lost | S4.9 |
| failed: revoked / unknown | S4.10 |

## 4. Tabela de cenas

| Cena | Gatilho | Inicial → final | Propriedades | ms | Curva | Stagger | Reduced motion | Custo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S1 Abertura | cold start | marca → radar | opacity, scale do anel da marca | ≤ 600 | `ease.enter` | — | corte | baixo |
| S2.1 Feixe | entrar / procurar de novo | parado → 1 volta → some | rotation do feixe, opacity | 4000 + 300 | `ease.linear` | — | pílula "Procurando…" | baixo |
| S2.2 PC aparece | mDNS add | — → ponto + card | opacity, scale, 1 ping | 220 + 600 | `spring.pop`, `ease.enter` | 40 ms | crossfade 150 | baixo |
| S2.3 PC some | mDNS remove + 3 s | ponto + card → — | opacity, scale | 160 | `ease.exit` | — | crossfade 150 | baixo |
| S2.4 Online/offline | status | cor + órbita | cor, posição radial | 150 / 320 | `ease.standard`, `spring.list` | — | corte | baixo |
| S2.6 Abrir PC | toque | radar → anel do código / mixer | push nativo + nome compartilhado | 400 | `ease.standard` | — | crossfade 200 | baixo |
| S3.3 Dígito | tecla | orbe vazio → aceso; arco +1/6 | scale, opacity, cor, trim | 260 / 220 | `spring.pop`, `ease.standard` | 30 ms (colar) | corte | baixo |
| S3.4 Verificando | 6º dígito | arco cheio → girando (≤ 5 s) | rotation do arco | 1200/volta | `ease.linear` | — | texto estático | baixo |
| S3.5 Anel → halo (H3) | PAIR_CONFIRM | anel do código → halo master | shared element, trim, opacity | 500 + 460 | `ease.inout`, `spring.remote` | — | check + crossfade 200 | médio |
| S3.6 Errado | ERROR 4 | orbes vermelhos → anel vazio | cor, rotation ±8°, trim | 600 | `ease.standard`, `ease.exit` | 30 ms | cor + texto | baixo |
| S3.7 Outros erros | ERROR 5/6/7/9, falha | anel → estado no centro + card | opacity, cor | 220 | `ease.enter` | — | crossfade 150 | baixo |
| S4.1 Grade entra | STATE_SYNCED | vazio → halo + cards | opacity, scale 0,96 → 1, trim | 220 / 460 | `ease.enter`, `spring.remote` | 35 ms, teto 240 | crossfade 150 | baixo |
| S4.2 Tocando | SOURCE bit1 | apagado ↔ brilho | opacity do brilho, texto | 220 | `ease.standard` | — | corte + texto | baixo |
| S4.3 App abre/fecha | SOURCE / SOURCE_REMOVED | — ↔ card | opacity, scale, reflow | 220 / 160 | `ease.enter`/`ease.exit`, `spring.list` | — | crossfade 150 | baixo |
| S4.4 Arraste | dedo | repouso → arraste → repouso | trim 1:1, ponta do arco | 0 / 180 | `spring.press` | — | sem rastro, sem scale | baixo |
| S4.5 Mudo | toque | ativo ↔ mudo | cor, opacity, ícone | 150 | `ease.standard` | — | corte | baixo |
| S4.6 Mudança do PC | SOURCE/ENDPOINT | v A → B | trim, número, legenda | 460 | `spring.remote` | — | salto + legenda | baixo |
| S4.7 Trocar saída | pílula / default muda | grade A → B | crossfade, trim | 220 | `ease.standard` | 35 ms | crossfade 150 | baixo |
| S4.8 Reconectando | queda > 700 ms | pílula verde → amarela | cor, texto, opacity | 150–220 | `ease.standard` | — | corte | baixo |
| S4.9 / S4.10 | failed / removido | → card de estado | opacity, translateY 8 pt | 300 | `ease.enter` | — | crossfade 150 | baixo |
| S5 Popup | HELLO(PAIR) | oculto → código → fechado | alpha, translateY 12 px | 220 / 150 | `ease.enter` / `ease.exit` | — | corte | baixo |
| S6 Brilho por nível | METERS ~20 Hz | — | opacity do brilho | contínuo com som | ataque/queda | — | estático | baixo |

## 5. Cenas

Notação: início e duração em ms a partir do gatilho; "1:1" = segue o dedo, sem animação.

### S1 — Abertura

- Só em cold start. O radar já está montado e tocável no primeiro quadro; a busca mDNS começa junto.
- Launch screen estática: fundo `bg`, anel da marca (círculo `accent`) centralizado.

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | anel da marca | scale, opacity | 1, 1 → órbita central do radar (matched geometry) | 0 | 400 | `ease.inout` |
| 2 | órbitas do radar | opacity | 0 → 1 | 150 | 300 | `ease.enter` |
| 3 | título, pílula, cards | opacity, translateY | 0, 6 pt → 1, 0 | 250 | 300 | `ease.enter` |
| 4 | feixe (S2.1) | começa | — | 400 | — | — |

Fim 550 ms. Reduced motion: corte para o radar. Haptics: nenhum.

### S2 — Computadores (radar)

Elementos: radar (centro = este celular, órbitas concêntricas), pontos de PC (ponto de cor + nome curto), feixe, pílula de estado, um card por PC abaixo (ícone, nome, estado, "Pareado"), "Adicionar por endereço".
Órbitas conforme a direção: pareados na interna, encontrados na do meio, offline na externa. Cada PC recebe um **ângulo fixo** derivado do `id` do agente (sem pular de lugar entre visitas); se dois colidirem, desloca-se o segundo em 24°.

#### S2.1 Feixe (procurando)

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | feixe (cone `accent` 22% → 0) | opacity | 0 → 1 | 0 | 150 | `ease.enter` |
| 2 | feixe | rotation | 0 → 360° (1 volta) | 0 | 4000 | `ease.linear` |
| 3 | feixe | opacity | 1 → 0 | 4000 | 300 | `ease.exit` |
| 4 | pílula | texto | "Procurando…" (`warn`) → "N computadores" ou continua "Procurando…" | 4000 | 150 | `ease.standard` |

- Reinicia ao puxar para atualizar ou tocar "Procurar de novo". A busca mDNS continua por baixo; o feixe é só a sinalização.
- Vazio após 6 s: dica "Celular e PC precisam estar na mesma rede" (opacity, 220 ms) e "Adicionar por endereço" ganha contorno `accent` (150 ms).
- Reduced motion: sem feixe; pílula "Procurando…" estática.

#### S2.2 PC aparece

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | ponto na órbita | scale, opacity | 0,4, 0 → 1, 1 | 0 | ~260 | `spring.pop` |
| 2 | anel de ping (uma vez) | scale, opacity | 0,6 → 2,2, 1 → 0 | 60 | 600 | `ease.enter` |
| 3 | nome junto ao ponto | opacity | 0 → 1 | 120 | 150 | `ease.enter` |
| 4 | cards abaixo (reflow) | posição | → | 0 | ~320 | `spring.list` |
| 5 | card novo | opacity, translateY | 0, 8 pt → 1, 0 | 60 | 220 | `ease.enter` |

Vários em < 300 ms: stagger 40 ms. Reduced motion: ponto e card por crossfade 150 ms, sem ping.

#### S2.3 PC some

Após 3 s de ausência confirmada (mDNS oscila). PCs pareados não somem, vão para offline (S2.4).

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | ponto + nome | scale, opacity | 1 → 0,6, 1 → 0 | 0 | 160 | `ease.exit` |
| 2 | card | opacity, scale | 1 → 0, 1 → 0,98 | 0 | 160 | `ease.exit` |
| 3 | cards vizinhos | reflow | → | 100 | ~320 | `spring.list` |

#### S2.4 Online ↔ offline

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | ponto | cor | `ok` ↔ `error` | 0 | 150 | `ease.standard` |
| 2 | ponto | posição radial (se a órbita mudar) | interna ↔ externa, mesmo ângulo | 0 | ~320 | `spring.list` |
| 3 | estado no card | crossfade de texto | "Disponível" ↔ "Offline" | 0 | 150 | `ease.standard` |
| 4 | card | opacity | 1 ↔ 0,6 | 0 | 220 | `ease.standard` |
| 5 | ping (só ao ficar online) | como S2.2 #2 | — | 100 | 600 | `ease.enter` |

No máximo 1 mudança por segundo por PC. Reduced motion: cor, texto e posição por corte, sem ping.

#### S2.6 Abrir um PC

- Toque no ponto ou no card. Press: scale 1 → 0,96 → 1 (90 ms + `spring.press`).
- Push nativo (iOS `NavigationStack`; Android Navigation Compose com predictive back). O **nome do PC** é compartilhado entre card/ponto e o título da próxima tela (`matchedGeometryEffect` / `sharedElement`), 400 ms `ease.standard`.
- Não pareado → S3. Pareado → mixer em `connecting` (S4.1).
- Reduced motion: crossfade 200 ms; foco no título. Haptics: impact `.light` / `VIRTUAL_KEY`.

### S3 — Pareamento (anel do código)

Elementos: título com o nome do PC; **anel** (trilho `track` + arco de progresso `accent`); **6 orbes** sobre o anel (vazio = `surface-2` com contorno `edge`; aceso = `accent` com brilho, dígito em `bg`); centro (estado e texto); teclado numérico do sistema (`.oneTimeCode` / `KeyboardType.NumberPassword`). **Sem haptic por dígito** (o teclado do sistema já dá o seu).

#### S3.1–S3.2 Pedindo código → aguardando código

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | centro | texto | "Pedindo código ao PC…" | 0 | — | — |
| 2 | orbes | opacity | 0,35 (desabilitados) | 0 | — | — |
| 3 | (HELLO_ACK em t₀) orbes | opacity, scale | 0,35, 0,9 → 1, 1 | t₀, +30 ms cada | 220 | `ease.enter` |
| 4 | centro | crossfade | → "Digite o código exibido no PC" | t₀ | 150 | `ease.standard` |
| 5 | orbe 1 | contorno | `edge` → `accent` (próximo a preencher) | t₀ + 150 | 150 | `ease.standard` |

#### S3.3 Dígito digitado (H2)

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | orbe n | preenchimento, scale | vazio, 0,8 → aceso, 1 | 0 | ~260 | `spring.pop` |
| 2 | dígito no orbe | opacity | 0 → 1 | 0 | 150 | `ease.enter` |
| 3 | brilho do orbe | opacity | 0 → 1 | 60 | 220 | `ease.enter` |
| 4 | arco de progresso | trim | (n−1)/6 → n/6 | 0 | 220 | `ease.standard` |
| 5 | contorno de próximo | orbe n → n+1 | — | 0 | 150 | `ease.standard` |

Apagar: orbe volta a vazio (opacity do dígito → 0, 110 ms `ease.exit`), arco recua 1/6 (160 ms `ease.exit`). Colar 6 dígitos: stagger 30 ms (150 ms) e arco 0 → 1 em 300 ms. Reduced motion: orbe e arco por corte.

#### S3.4 Verificando

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | teclado | recolhe | sistema | 0 | sistema | — |
| 2 | arco | trim | 1 → 0,25 (vira um segmento) | 0 | 220 | `ease.standard` |
| 3 | segmento | rotation | giro contínuo, 1200 ms/volta, **máx 5 s** | 220 | ≤ 5000 | `ease.linear` |
| 4 | centro | crossfade | → "Verificando…" | 0 | 150 | `ease.standard` |
| 5 | após 5 s | segmento para; texto fica | — | 5220 | 150 | `ease.standard` |

Reduced motion: arco cheio estático + "Verificando…".

#### S3.5 Sucesso: o anel vira o halo master (H3)

- Gatilho: PAIR_CONFIRM. O mixer é montado por baixo em `connecting`. O valor do master entra quando o dump chegar.
- Transição de tela por elemento compartilhado: SwiftUI `matchedGeometryEffect` do anel para o halo (ou `.navigationTransition(.zoom)` no iOS 18 com o anel como fonte); Compose `SharedTransitionLayout` + `sharedElement`. O pareamento é **substituído** pelo mixer (voltar não retorna ao código).

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | arco | trim | → 1 (anel fecha) | 0 | 220 | `ease.standard` |
| 2 | arco | cor | `accent` → `ok` → `accent` | 0 | 150 + 150 (aos 300) | `ease.standard` |
| 3 | check no centro | opacity, scale | 0, 0,9 → 1, 1 | 80 | ~260 | `spring.pop` |
| 4 | orbes | opacity, scale | 1 → 0, 1 → 0,6 | 250 | 160 | `ease.exit`, stagger 20 ms |
| 5 | anel → halo master | posição + tamanho (shared) | rect do anel → rect do halo | 400 | 500 | `ease.inout` |
| 6 | check → valor % | crossfade | ✓ → "—" | 600 | 150 | `ease.standard` |
| 7 | resto do mixer (pílulas, título) | opacity | 0 → 1 | 650 | 220 | `ease.enter` |
| 8 | arco do halo | trim | 1 → volume do master | ao chegar o estado (≥ 900) | ~460 | `spring.remote` |
| 9 | número % | `.numericText` / `AnimatedContent` | "—" → valor | junto com 8 | 220 | `ease.standard` |
| 10 | cards de app | S4.1 | — | junto com 8 | — | — |

Halo tocável a partir de 900 ms. Reduced motion: arco cheio + check + "Pareado com <PC>" por 600 ms, crossfade 200 ms para o mixer com valores no lugar. Haptics: `.success` / `CONFIRM`. Acessibilidade: anúncio "Pareado com <PC>", foco no título do mixer.

#### S3.6 Código errado (ERROR 4)

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | orbes acesos | cor | `accent` → `error` | 0 | 90 | `ease.standard` |
| 2 | anel + orbes | rotation ("nega") | 0 → −8° → 8° → 0 | 0 | 300 | `ease.standard` |
| 3 | arco | trim | 1 → 0 (esvazia) | 350 | 220 | `ease.exit` |
| 4 | orbes | volta a vazio, dígitos somem | — | 350, +30 ms cada | 110 | `ease.exit` |
| 5 | centro | crossfade | → "Código incorreto. O PC mostrou um código novo." | 0 | 150 | `ease.standard` |
| 6 | orbe 1 + teclado | contorno de próximo; teclado volta | — | 600 | 150 | `ease.standard` |

Reduced motion: sem rotação; cor `error` + texto, esvazia por corte. Haptics: `.error` / `REJECT`.

#### S3.7 Bloqueado (5), recusado (6), ocupado (7), tempo esgotado (9), falha de conexão

Molde único: orbes somem (opacity → 0, 160 ms `ease.exit`), arco fica no trilho com a cor do estado (150 ms), centro mostra ícone + texto, e um card com a ação entra abaixo do anel (opacity, translateY 8 → 0 pt, 220 ms `ease.enter`, início 100 ms).

| Caso | Cor do anel | Texto | Ação | Haptic iOS / Android |
| --- | --- | --- | --- | --- |
| Bloqueado | `warn` | "Muitas tentativas. Aguarde alguns minutos e tente de novo." (o protocolo não envia o tempo restante) | "Voltar" | `.warning` / `REJECT` |
| Recusado | `error` | "Recusado no PC" | "Voltar" | `.error` / `REJECT` |
| Ocupado | `warn` | "O PC está pareando com outro aparelho" | "Tentar de novo" | `.warning` / `REJECT` |
| Tempo esgotado | `warn` | "O tempo acabou" | "Pedir novo código" | `.warning` / `REJECT` |
| Falha de conexão | `error` | "Não foi possível conectar ao PC" | "Tentar de novo" / "Computadores" | `.error` / `REJECT` |

Reduced motion: crossfade 150 ms. Foco no texto; anúncio como alerta.

### S4 — Mixer

Elementos: pílula do dispositivo de saída (nome ▾), pílula de conexão, **halo master** (arco grande + valor % 68 pt + "MASTER"), botão de mudo do master, **grade de cards** (2 colunas): anel em volta do ícone, nome, linha de estado, botão de mudo redondo no canto.

#### S4.1 Halo e grade entram quando o estado chega

- O cliente acumula até `STATE_SYNCED` e anima uma vez. Os arcos partem do trilho vazio e se preenchem até o valor real com `spring.remote` (é o primeiro dado do PC, então usa a linguagem do PC).
- Placeholders só se o estado demorar > 250 ms: cards vazios estáticos, sem shimmer; após 5 s, "Carregando aplicativos…".
- Só o que está visível anima. Em resync após reconexão, nada entra em sequência: só as diferenças (S4.3, S4.6).

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | halo master (se não veio de H3) | opacity, scale | 0, 0,96 → 1, 1 | 0 | 220 | `ease.enter` |
| 2 | arco do halo | trim | 0 → volume | 60 | ~460 | `spring.remote` |
| 3 | cards 1–8 | opacity, scale | 0, 0,96 → 1, 1 | 60, +35 ms cada | 220 | `ease.enter` |
| 4 | cards 9+ | opacity | 0 → 1 | 300 | 150 | `ease.enter` |
| 5 | anel de cada card | trim | 0 → volume | início do card + 60 | ~460 | `spring.remote` |

Fim ≤ ~800 ms; tudo tocável assim que visível (tocar durante o preenchimento entrega o anel ao dedo, sem salto). Reduced motion: crossfade 150 ms, arcos já no valor. Haptics: nenhum.

#### S4.2 App começa / para de tocar

- "Só o que toca brilha." Liga na hora; desliga após **1 s** sem som (evita piscar entre faixas, < 3 Hz). A ordem dos cards **não** muda.
- Com medidores (S6): o brilho segue o nível. Sem medidores (agente 1.0, `meter_slot` 0) ou com o ajuste desligado: brilho **estático** enquanto toca.

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| liga 1 | brilho do anel | opacity | 0 → nível (S6) ou 0,6 fixo | 0 | 220 | `ease.standard` |
| liga 2 | linha de estado | texto | "45%" → "tocando · 45%" | 0 | 150 | `ease.standard` |
| desliga 1 | brilho | opacity | → 0 | 0 | 220 | `ease.exit` |
| desliga 2 | linha de estado | texto | → "45%" | 0 | 150 | `ease.standard` |

Reduced motion: brilho estático liga/desliga por corte; texto "tocando".

#### S4.3 App abre / fecha

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| abre 1 | cards vizinhos | reflow | → | 0 | ~320 | `spring.list` |
| abre 2 | card novo | opacity, scale | 0, 0,96 → 1, 1 | 40 | 220 | `ease.enter` |
| abre 3 | anel | trim | 0 → volume | 100 | ~460 | `spring.remote` |
| fecha 1 | card | opacity, scale | 1 → 0, 1 → 0,96 | 0 | 160 | `ease.exit` |
| fecha 2 | vizinhos | reflow | → | 100 | ~320 | `spring.list` |

- Nunca brigar com o dedo: se o app sob o dedo fechar, o card fica (opacidade 0,5, "Fechado no PC", sem enviar) até o dedo sair + 300 ms. Reflow que moveria o card em arraste espera o dedo sair.
- Mais de 3 mudanças em 1 s: um só reflow. Reduced motion: crossfade 150 ms. Anúncio resumido, ≤ 1 a cada 5 s.

#### S4.4 Arraste com o dedo

- **Gesto relativo** (decisão da hub): tocar não salta o volume.
  - começou **sobre o anel** (ou o halo): delta angular ao longo do anel;
  - começou **em outro ponto do card**: delta horizontal, 240 pt = 100% (no halo, 280 pt = 100%);
  - vertical rola a tela; limiar de 6 pt antes de decidir o eixo.
- Durante o arraste, sem animação (`Transaction(animation: nil)` / `Animatable.snapTo`); envio ao PC ≤ ~30 Hz + valor final. Sem inércia.

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| toque 1 | ponta do arco (ponto de luz) | scale, opacity | 0,6, 0 → 1, 1 | 0 | ~180 | `spring.press` |
| arraste | trim do arco | `v` | segue o dedo | — | 1:1 | — |
| arraste | número % (card e halo) | texto | atualiza na hora, sem transição | — | 1:1 | — |
| arraste | rastro de luz na ponta (da direção) | opacity de um segmento atrás da ponta | 0,5 → 0 | contínuo | 120 | `ease.exit` |
| solta | ponta do arco | scale, opacity | → 0,6, 0 | 0 | ~180 | `spring.press` |

Reduced motion: sem ponta animada e sem rastro; o arco e o número seguem o dedo. Acessibilidade: controle ajustável (VoiceOver ±5% por gesto; TalkBack `setProgress`), valor "Spotify, 45%".

#### S4.5 Mudo / desmudo

Direção: mudo = anel apagado + botão vermelho + texto "mudo".

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | botão | scale (press) | 1 → 0,92 → 1 | 0 | 90 + `spring.press` | `ease.standard` |
| 2 | botão | fundo | `surface-2` ↔ `error` (+ brilho vermelho suave) | 0 | 150 | `ease.standard` |
| 3 | ícone | troca alto-falante ↔ cortado | iOS `.contentTransition(.symbolEffect(.replace))`; Compose `AnimatedContent` crossfade | 0 | 150 | `ease.standard` |
| 4 | anel | opacity | 1 ↔ 0,55; brilho → 0 | 0 | 150 | `ease.standard` |
| 5 | nome, linha de estado | cor, texto | `paper` ↔ `muted`; "✕ mudo · 45%" | 0 | 150 | `ease.standard` |

O valor continua visível. **Master mudo:** o halo apaga como acima e os cards de app vão juntos para opacidade 0,5 (150 ms, sem cascata), com "Saída sem som" sob o halo; os mudos individuais não mudam. Mudo vindo do PC: igual, sem haptic, com a legenda de S4.6. Reduced motion: corte.

#### S4.6 Mudança feita no PC × pelo dedo

| | Dedo (S4.4) | PC |
| --- | --- | --- |
| arco | 1:1 | `spring.remote` (sem overshoot, ~460 ms) |
| número % | troca na hora | conta até o valor (`.contentTransition(.numericText(value:))` / `AnimatedContent` vertical curto) |
| ponta de luz / rastro | sim | não |
| legenda | — | "alterado no PC" na linha de estado por 1,5 s |
| haptic | sim | nunca |

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | arco | trim | A → B | 0 | ~460 | `spring.remote` |
| 2 | número % | numericText | A → B | 0 | 220 | `ease.standard` |
| 3 | linha de estado | crossfade | → "alterado no PC" | 0 | 150 | `ease.standard` |
| 4 | linha de estado | crossfade | → estado normal | 1500 após a última mudança | 150 | `ease.standard` |

Rajada (alguém arrastando no PC): cada mensagem redireciona o mesmo spring, preservando velocidade; uma legenda por rajada.

**Regras anti-briga (normativas):**
1. Com o dedo num controle, mensagens do PC para aquele id **não são desenhadas**; o último valor recebido é guardado.
2. Ecos dos próprios envios (valor igual a um enviado no último 1 s) são descartados sem animação.
3. Ao soltar, janela de **400 ms** para os ecos. Depois, se o último valor do PC diferir do local por > 0,5%, aplica-se S4.6.
4. Controles que não estão sob o dedo reagem normalmente.

Reduced motion: arco e número saltam; a legenda "alterado no PC" aparece. Acessibilidade: `accessibilityValue` atualizado; anúncio só se focado, ≤ 1 a cada 5 s.

#### S4.7 Trocar dispositivo de saída

Pílula: nome por crossfade 150 ms. Halo: arco → novo volume (`spring.remote`). Cards antigos: opacity → 0 (110 ms `ease.exit`); novos: S4.1 a partir de 110 ms. Default trocado no PC: o mesmo + "alterado no PC" sob o halo. Haptic no menu: selection / `CLOCK_TICK`. Reduced motion: crossfade 150 ms.

#### S4.8 Reconectando

- Só se a queda durar > 700 ms. Conteúdo visível, esmaecido e não interativo. **Comandos feitos desconectado são descartados**; após reconectar, o estado do PC prevalece (decisão da hub). Se o dedo estava num controle, o gesto termina e o valor do PC volta via S4.6.

| # | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | pílula de conexão | cor, texto | `ok` "ao vivo" → `warn` "Reconectando…" | 0 | 150 | `ease.standard` |
| 2 | conteúdo | opacity | 1 → 0,5; brilhos → 0 | 0 | 220 | `ease.standard` |
| 3 | ponto da pílula | opacity 1 → 0,4 → 1, ciclo 1200 ms | máx 4 ciclos (4,8 s), depois estático | 150 | 4800 | `ease.standard` |
| volta 1 | pílula | → `ok` "ao vivo" | — | 0 | 150 | `ease.standard` |
| volta 2 | conteúdo | opacity → 1 | — | 0 | 220 | `ease.standard` |
| volta 3 | diferenças do resync | S4.3 / S4.6 | — | 0 | — | — |

Reduced motion: corte, ponto estático. Haptics: nenhum. Anúncio polite "Reconectando" / "Conectado" (queda > 3 s).

#### S4.9 Conexão perdida · S4.10 Pareamento removido (ERROR 3, 8, 10)

Card de estado sobre o conteúdo (opacity 0 → 1, translateY 8 → 0 pt, 300 ms `ease.enter`); conteúdo → 0,2 (220 ms); halo apaga (arco → `track`, 150 ms).
- Perdida: pílula `error`, "Conexão perdida com <PC>", "Tentar de novo" / "Computadores". Haptic `.warning` / `REJECT`.
- Removido: "Pareamento removido no PC. Pareie de novo para controlar este computador." + "Voltar aos computadores". Sem navegação automática. No radar, o ponto do PC muda da órbita interna para a do meio (`spring.list`, ~320 ms) e o rótulo "Pareado" some do card (150 ms). Haptic `.error` / `REJECT`.

Reduced motion: crossfade 150–200 ms, mudança de órbita por corte. Foco no título do card; anúncio como alerta.

### S5 — Popup de pareamento no Windows

- Conteúdo da direção: "Um celular quer se conectar", nome do celular, 6 casas de código redondas (`accent`, brilho opcional), anel de contagem regressiva + "Aguardando · 1:24", "Recusar". Ícone da bandeja: anel amarelo; verde com celular conectado (troca de ícone estática).
- Implementação: `WS_EX_LAYERED`, topmost, `WS_EX_NOACTIVATE` (não rouba o foco de jogo em tela cheia; botão continua clicável); Direct2D; alpha por `UpdateLayeredWindow`; timer de 16 ms só durante animação, CPU 0% em repouso. Nome UIA com o código para o Narrador.
- Anel de contagem (120 s de espera do protocolo): avança **em passos de 1 s, sem tween** (informação essencial de tempo; um redesenho por segundo).
- Reduced motion: `SPI_GETCLIENTAREAANIMATION` = FALSE → tudo por corte.

| # | Momento | Elemento | Propriedade | De → Para | Início | Duração | Curva |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | aparece | janela | alpha, translateY | 0, 12 px → 1, 0 | 0 | 220 | `ease.enter` |
| 2 | código errado | código | crossfade | antigo → novo | 0 | 150 | `ease.standard` |
| 3 | código errado | rodapé `warn` | texto | "Código errado no celular. Use o novo código." | 0 | corte | — |
| 4 | sucesso | código → check `ok` + "<celular> conectado" | crossfade | — | 0 | 150 | `ease.standard` |
| 5 | sucesso | janela | alpha, translateY | 1, 0 → 0, 8 px | 1500 | 150 | `ease.exit` |
| 6 | recusar | janela | alpha | 1 → 0 | 0 | 110 | `ease.exit` |
| 7 | bloqueado | conteúdo | crossfade | → "Pareamento bloqueado. Tente em alguns minutos." + "Fechar" | 0 | 150 | `ease.standard` |
| 8 | tempo esgotado / celular desistiu | janela | alpha | 1 → 0 | 0 | 150 | `ease.exit` |

Sem `FlashWindowEx`, sem som, nada pisca.

### S6 — Brilho por nível ("só o que toca brilha", H4)

Protocolo 1.1: `SET_METERS(1)` ao abrir o mixer, `SET_METERS(0)` ao sair; reenviar após cada reconexão. `METERS` (~20 Hz) traz `meter_slot` + `peak` 0..255 em escala perceptual (raiz quadrada); `meter_slot` 0 = sem medidor. No silêncio o agente manda um quadro zerado e para; o cliente desenha a própria queda.

- O que anima: **somente a opacidade da camada de brilho** do anel de cada card e do halo master (endpoint). O arco (volume) não se mexe com o som.
- Normalização: `n = peak / 255` (já perceptual).
- Envelope suavizado (sem cintilar):
  - **subida**: τ = 60 ms (exponencial, por quadro);
  - **queda**: τ = 300 ms;
  - brilho = `0,15 + 0,55 · n_suavizado` enquanto toca; 0 quando não toca ou mudo.
  - Com τ de queda de 300 ms, a variação perceptível fica abaixo de ~3 Hz, em área pequena e com baixa variação de luminância (WCAG 2.3.1).
- Desenho: a camada de brilho é o mesmo arco com desfoque, **rasterizado só quando o volume muda**; por quadro muda apenas a opacidade (SwiftUI: arco borrado em camada própria com `.drawingGroup()` e `.opacity` lida de um modelo atualizado por `TimelineView(.animation(minimumInterval: 1/60))`; Compose: `graphicsLayer { alpha = … }` lendo `State` atualizado em `withFrameNanos`, sem recomposição por quadro).
- Orçamento: ≤ 1 ms por quadro no total; teto de 60 fps também em 120 Hz (`preferredFrameRateRange(30, 60, 60)` no iOS).
- **Parar:** quando todos os envelopes chegam a 0 após o quadro zerado do agente, o display link para. Desinscrever (`SET_METERS 0`) quando: mixer fora da tela, app em background, Low Power Mode (`ProcessInfo.isLowPowerModeEnabled`) / Economia de bateria (`PowerManager.isPowerSaveMode`), estado térmico sério (`thermalState ≥ .serious` / `THERMAL_STATUS_SEVERE`), reduced motion, ou ajuste "Brilho ao vivo" desligado. Cards fora da tela não desenham.
- Pausa (WCAG 2.2.2): ajuste "Brilho ao vivo" (padrão ligado; desligado sob reduced motion).
- Reduced motion / desligado / agente 1.0: brilho **estático** (0,6) enquanto toca, 0 quando não toca. A informação "tocando" fica no brilho e no texto.

## 6. Haptics

iOS: `UIImpactFeedbackGenerator`, `UISelectionFeedbackGenerator`, `UINotificationFeedbackGenerator` (ou `.sensoryFeedback`), `prepare()` no toque. Android: `View.performHapticFeedback` sem `FLAG_IGNORE_GLOBAL_SETTING`; API entre parênteses e fallback.

| Evento | iOS | Android |
| --- | --- | --- |
| tocar ponto/card de PC, botão | impact `.light` | `VIRTUAL_KEY` |
| puxar para atualizar (limiar) | selection | `GESTURE_THRESHOLD_ACTIVATE` (34) → `CONTEXT_CLICK` |
| dígito do código | — (teclado do sistema) | — |
| pareado | notification `.success` | `CONFIRM` (30) → `LONG_PRESS` |
| código errado, recusado, falha, conexão perdida, pareamento removido | notification `.error` | `REJECT` (30) → `LONG_PRESS` |
| bloqueado, ocupado, tempo esgotado | notification `.warning` | `REJECT` (30) → `CONTEXT_CLICK` |
| início do arraste | impact `.light` | `DRAG_START` (34) → `GESTURE_START` (30) → `VIRTUAL_KEY` |
| cada 10% | selection | `SEGMENT_FREQUENT_TICK` (34) → `CLOCK_TICK` |
| 0% / 100% | impact `.rigid` | `SEGMENT_TICK` (34) → `CONTEXT_CLICK` |
| mutar | impact `.medium` | `TOGGLE_ON` (34) → `CONTEXT_CLICK` |
| desmutar | impact `.light` | `TOGGLE_OFF` (34) → `CLOCK_TICK` |
| escolher saída | selection | `CLOCK_TICK` |
| qualquer coisa vinda do PC, reconexão, app abrindo/fechando | — | — |

Ticks coalescidos: ≤ 1 a cada 30 ms. Reduced motion não desliga haptics; o ajuste de vibração do sistema, sim.

## 7. Reduced motion

- iOS: `@Environment(\.accessibilityReduceMotion)` e `UIAccessibility.prefersCrossFadeTransitions` (troca o morph H3 por crossfade).
- Android: sinal = "Remover animações" (`Settings.Global.ANIMATOR_DURATION_SCALE == 0f`). Com escala 0 os tweens do Compose terminam na hora; por isso as variantes não dependem de fade: a informação está em cor, texto e ícone, e a legenda "alterado no PC" é um estado temporizado (1,5 s), não animação.
- Windows: `SPI_GETCLIENTAREAANIMATION`.

| Cena | Variante | Informação preservada |
| --- | --- | --- |
| S1 | corte | tela pronta |
| S2.1 | sem feixe; pílula "Procurando…" | busca ativa |
| S2.2–2.4 | crossfade/corte, sem ping | PC novo, removido, online/offline (cor + texto + órbita) |
| S2.6 | crossfade 200, foco no título | qual PC |
| S3.3 | corte | dígitos e progresso n/6 |
| S3.4 | arco estático + "Verificando…" | verificação |
| S3.5 | check + legenda 600 ms, crossfade 200 | sucesso e destino |
| S3.6 | sem rotação; cor + texto | erro e código novo |
| S3.7 | crossfade 150 | motivo e ação |
| S4.1, S4.3, S4.7 | crossfade 150, arcos no valor | estado / mudança |
| S4.2, S6 | brilho estático ligado/desligado + texto | tocando |
| S4.4 | sem ponta de luz nem rastro | valor |
| S4.5 | corte | mudo (cor, ícone, texto) |
| S4.6 | salto + legenda | valor novo e origem PC |
| S4.8–4.10 | corte / crossfade | conexão e motivo |
| S5 | corte | código e resultado |

Loops, todos com teto: feixe (4 s), giro do verificando (5 s), ponto do reconectando (4,8 s). Movimento contínuo sem teto: só o brilho por nível (S6), com ajuste para desligar. Nada acima de 3 Hz.

## 8. Performance e bateria

| Item | Limite |
| --- | --- |
| iPhone 14 Plus (60 Hz) | 16,7 ms por quadro; thread principal ≤ 8 ms durante arraste |
| 120 Hz (Pro, Android 90/120) | 8,3 ms; arraste e springs podem usar 120 Hz (`CADisableMinimumFrameDurationOnPhone = YES`) |
| Latência dedo → pixel | ≤ 1 quadro |
| Hitches (Instruments) | < 5 ms/s em S3.5, S4.1 e arraste |
| Android (Macrobenchmark, JankStats) | P90 `frameDurationCpuMs` < 8 ms |
| Elementos animando juntos | ≤ 20 (pior caso S4.1 com 8 cards: card + anel) |
| Desfoque do brilho | rasterizado só quando o volume muda; por quadro só opacidade |
| Brilho por nível | ≤ 1 ms/quadro no total, teto 60 fps, parado no silêncio |
| Rede no arraste | ≤ 30 SET/s + valor final |
| Ícones (ICON PNG) | decodificados fora do thread principal; entram com crossfade 150 ms |
| Popup Windows | timer só durante animação; anel de contagem 1 redesenho/s |

Teste de repouso: mixer aberto, sem som e sem mudanças por 30 s → nenhuma animação nem `TimelineView`/`CADisplayLink`/`withFrameNanos` ativos; Instruments/Perfetto sem quadros novos. Radar após o feixe: idem.

Propriedades: só `trim`, rotation, scale, translate, opacity e cor. Reflow da grade/lista é one-shot nativo.

## 9. Acessibilidade em transições

| Transição | Foco | Anúncio |
| --- | --- | --- |
| S2.6 abrir PC | título da nova tela | — |
| S3.2 código pronto | campo do código | "Digite o código exibido no PC" |
| S3.3 dígito | — | valor do campo "3 de 6 dígitos" |
| S3.5 sucesso | título do mixer | polite "Pareado com <PC>" |
| S3.6–3.7 | texto do estado | alerta |
| S4.3 | não move | polite resumido, ≤ 1/5 s |
| S4.6 | não move | só se focado, ≤ 1/5 s |
| S4.8 | não move; controles desabilitados | polite (queda > 3 s) |
| S4.9–4.10 | título do card | alerta |
| S5 | não rouba foco | nome UIA com código e celular |

Alvos ≥ 44 pt / 48 dp: o botão de mudo de 36 pt da direção precisa de área de toque expandida para 44 pt. Pontos do radar também (área de toque 44 pt em volta do ponto). Estado nunca só por cor: ponto/arco sempre com texto ou ícone.

## 10. Protótipo de risco

1. **S4.4 + S4.6 (arraste no anel × eco × mudança remota).** Aceite do brief: o eco não faz o controle pular. SwiftUI no iPhone 14 Plus, com agente simulado (ecos de 30–150 ms com jitter) e um "PC fantasma" em rajadas de 20 Hz. Aprovado com gravação a 60 fps mostrando: (a) nenhum salto durante o arraste nem até 400 ms depois; (b) rajada remota sem overshoot e uma legenda; (c) dedo assumindo um anel no meio de um deslize remoto sem salto; (d) arraste angular e horizontal sem conflito com a rolagem vertical; (e) hitches < 5 ms/s. Repetir em Compose num Android de 120 Hz.
2. **S3.5 (anel → halo)** e **S6 (brilho com desfoque)**: medir hitches na troca de tela e custo do brilho com 8 cards tocando.

Responsável: `creative-developer`.

## 11. Perguntas abertas (direção de arte)

1. Confirmar os "Ajustes à direção" da seção 1 (feixe com uma volta, ping único, cursor estático, giro do verificando com teto, brilho estático sem dado de nível, entrada mais curta e sem overshoot).
2. Órbitas: a b2 mantém três (pareados / encontrados / offline) ou duas (pareados / encontrados, offline só pela cor)? Com duas, S2.4 vira só troca de cor.
3. Rastro de luz de 120 ms na ponta do arco durante o arraste: manter (está aqui, desligado em reduced motion) ou cortar para reduzir elementos?
4. A seção "Motion" de `docs/design.md` deve ser substituída por estes tokens (`design-system-architect`).

Decisões da hub incorporadas: medidores no protocolo 1.1 (S6); comandos desconectado descartados e estado do PC prevalece (S4.8); arraste relativo (S4.4); bloqueio sem tempo restante, texto genérico (S3.7).
