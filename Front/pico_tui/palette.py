"""Paleta "Aço industrial" da TUI iot-over-can.

Princípios (inspirados na ISA-101 / High Performance HMI):

* Fundo e superfícies em cinza-aço neutro: a tela "em repouso" não chama atenção.
* Cor saturada é reservada para condições anormais (atenção e crítico).
* Azul de segurança é usado apenas para foco, seleção e informação, nunca para alarme.
* Estado nunca é comunicado só por cor: sempre há símbolo + palavra.

Todas as combinações texto/fundo usadas pela interface têm contraste igual ou
superior a 4,5:1 (WCAG AA), exceto ``TEXT_DIM``, reservado a informação
secundária como horários (3,0:1).
"""

# Superfícies
BG_ROOT = "#1F2429"          # fundo da aplicação
BG_PANEL = "#272D33"         # painéis e seções
BG_RAISED = "#313840"        # diálogos, faixa de estado neutra
BG_WIDGET = "#1B2025"        # campos de entrada, áreas de log
BG_SELECTED = "#2E4A66"      # linha selecionada/realçada
BORDER_SOFT = "#46505A"
BORDER_STRONG = "#5F6B77"

# Texto
TEXT_PRIMARY = "#E3E7EB"
TEXT_SECONDARY = "#B7C0C8"
TEXT_MUTED = "#8E99A3"
TEXT_DIM = "#6B7680"
TEXT_DISABLED = "#56606A"
TEXT_INVERSE = "#15191D"

# Interação
ACCENT_FOCUS = "#5B9BE0"     # azul de segurança (foco, seleção, links)
ACCENT_ACTION = "#3D7CC9"    # botões primários

# Estados operacionais
STATE_OK = "#79B38A"         # normal (usado com parcimônia, só no símbolo)
STATE_INFO = "#5B9BE0"
STATE_WARNING = "#E8A317"    # atenção (âmbar de segurança)
STATE_CRITICAL = "#F4706A"   # crítico (texto sobre fundo escuro)
STATE_OFFLINE = "#8A949E"    # sem dados / desconectado
STATE_UNKNOWN = "#8A949E"

# Faixa de estado do sistema (fundos sólidos)
BAND_NORMAL_BG = BG_RAISED
BAND_NORMAL_FG = TEXT_PRIMARY
BAND_WARNING_BG = "#E8A317"
BAND_WARNING_FG = TEXT_INVERSE
BAND_CRITICAL_BG = "#B3261E"
BAND_CRITICAL_FG = "#FFFFFF"
BAND_OFFLINE_BG = "#3A424B"
BAND_OFFLINE_FG = TEXT_PRIMARY

# Aliases preservados para compatibilidade com módulos e versões anteriores.
BG_BASE = BG_ROOT
CRITICAL = STATE_CRITICAL
BORDER_DIM = BORDER_SOFT
WARNING = STATE_WARNING
SUCCESS = STATE_OK
ACCENT = ACCENT_FOCUS
ACCENT2 = ACCENT_ACTION
TEXT_MAIN = TEXT_PRIMARY
ACCENT_COPPER = ACCENT_ACTION
ACCENT_RUST = STATE_CRITICAL
ACCENT_LICHEN = STATE_OK
ACCENT_PINE = BG_SELECTED
STATE_DEGRADED = STATE_WARNING
STATE_CHANGED = ACCENT_FOCUS

# Marcadores textuais legados (logs e exportações antigas ainda os utilizam).
STATUS_MARKERS = {
    "ONLINE": "[OK]",
    "AGING": "[INFO]",
    "STALE": "[STALE]",
    "LOST": "[LOST]",
    "OFFLINE": "[OFFLINE]",
    "UNKNOWN": "[N/A]",
}

QUALITY_MARKERS = {
    "REAL": "[REAL]",
    "SIMULATED": "[SIM]",
    "PLACEHOLDER": "[N/A]",
    "DEGRADED": "[DEG]",
    "INVALID": "[CRIT]",
    "STALE": "[STALE]",
    "LOST": "[LOST]",
    "UNKNOWN": "[N/A]",
}
