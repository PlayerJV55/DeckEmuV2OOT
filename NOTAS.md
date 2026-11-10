# NOTAS DE DESEMPENHO E DESENVOLVIMENTO — DeckEmuV2OOT

Este documento registra o histórico de diagnósticos, configurações testadas, versões geradas e as medições de FPS no Steam Deck para o jogo *The Legend of Zelda: Ocarina of Time*.

---

## 1. Linha de Base (Baseline)

### Hardware e Ambiente de Teste
- **Dispositivo:** Steam Deck (AMD Zen 2 4c/8t, GPU RDNA2 8 CUs, 16 GB de RAM LPDDR5 compartilhada)
- **Sistema Operacional:** SteamOS / Linux (driver Vulkan RADV)
- **Local do Jogo:** `/home/deck/Games/OcarinaOfTime` (a ROM original nunca é alterada)
- **Diretório de Configuração / Dados:** `/home/deck/.local/share/eden` e `$XDG_CONFIG_HOME/nemyu`
- **Opções de Inicialização (padrão de partida):**
  ```bash
  QT_QPA_PLATFORM=xcb VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json APPIMAGE_EXTRACT_AND_RUN=1 MESA_SHADER_CACHE_MAX_SIZE=10G RADV_DEBUG=async_compute %command%
  ```
  *(Variável temporária de contorno para mesh shaders: `RX_VK_NO_MESH=1`)*

### Sintomas Medidos Iniciais (Build de Linha de Base)
- **Tela de início (antes de apertar Play):** ~6 FPS
- **Gameplay (o jogo em si após carregar):** ~13 FPS
- **Menu dentro do jogo (após apertar Play e carregar o save):** ~60 FPS
- **Utilização de Hardware:** CPU, GPU e memória com folga (recursos sobrando). Gargalo concentrado em única thread na CPU (thread da GPU do emulador a ~100%).

---

## 2. Histórico de Builds e Testes

| Build / Tag | Alterações Principais | Tela de Início | In-Game (OoT) | Menu | Observações |
|---|---|---|---|---|---|
| **v0.1.0-baseline** | Importação da base sem alterações de código. Workflow GitHub Actions com x86-64-v3/AVX2. | 6 FPS | 13 FPS | 60 FPS | Linha de base oficial para comparação de ganhos futuros. |

---

## 3. Principais Hipóteses e Próximas Investigações

1. **Tela inicial a 6 FPS (Aquecimento de Shaders):**
   - O aquecimento padrão dispara 4 threads em paralelo (`EDEN_NX2_WARM_THREADS=4`), saturando todos os 4 núcleos físicos do Zen 2 na inicialização e competindo com a renderização.
   - Teste inicial imediato via `nx2_switches.txt`: reduzir para `EDEN_NX2_WARM_THREADS=1` ou `2`.

2. **Gargalo In-Game a 13 FPS (Thread da GPU):**
   - Investigar a divisão de trabalho da GPU thread entre Front-thread (parse de GPFIFO/macros) e Back-thread (métodos/rasterizador) documentada em `NX2_GPU_THREAD_SPLIT.md`.
   - Testar o comportamento das opções `EDEN_NX2_SPLIT` e `EDEN_NX2_SPLIT_METHODS`.

3. **Pipeline Binaries e RADV:**
   - Verificar a compatibilidade de `VK_KHR_pipeline_binary` no driver Mesa/RADV do SteamOS.

4. **Frame Pacer do Host:**
   - Avaliar a estabilidade com e sem o frame pacer (`VK_KHR_present_wait` e `EDEN_NX2_PRESENT_QUEUE=1`).

5. **PGO (Profile-Guided Optimization) no Linux:**
   - Configurar pipeline com Clang e dados de perfil treinados diretamente no hardware do Deck rodando OoT.
