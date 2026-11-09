/*
 * Leitor da documentação do iot-over-can no M5Stack PaperColor.
 *
 * A documentação vem gravada no firmware (documentos.h, gerado na compilação a
 * partir dos arquivos .md do repositório). Três telas: Biblioteca (a lista dos
 * documentos), Seções (o índice de um documento) e Leitura.
 *
 * Botões: A volta, C avança, B escolhe. Segurar A ou C pula de seção (na
 * Leitura) ou de dez em dez itens (nas listas); segurar B volta à tela
 * anterior. O papel colorido leva de 15 a 30 s para desenhar: toques seguidos
 * se somam e a tela só é desenhada quando eles param. Enquanto o papel
 * desenha, o LED fica azul.
 *
 * O papel guarda a imagem sem energia. Na bateria, sem uso por 20 s, o ESP32
 * entra em sono leve com a página na tela, e um botão o acorda. Depois de
 * 30 min dormindo, o leitor se desliga. O botão de energia segue o padrão de
 * fábrica do M5PM1: um toque liga (ou reinicia), dois toques desligam. Ao
 * ligar, o leitor volta onde parou sem redesenhar a página que já está no
 * papel. A posição em cada documento fica na NVS.
 *
 * Teclado CardKB (opcional, Port A): setas, Enter, Esc e, na Biblioteca, 1 a 0
 * abrem os documentos. Pela serial USB, a/b/c tocam e A/B/C seguram os botões;
 * s mostra o estado.
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "documentos.h"
#include "leitor_layout.h"

namespace {

using namespace leitor;

constexpr int W = 400;
constexpr int H = 600;
constexpr int MARGEM = 12;
constexpr int TOPO = 34;              // faixa do cabeçalho
constexpr int CORPO_Y = TOPO + 8;     // início do texto
constexpr int RODAPE_Y = H - 42;      // linha do rodapé
constexpr uint32_t ESPERA_MS = 900;   // toques seguidos se somam antes de desenhar
constexpr uint32_t SONO_MS = 20000;   // sem uso, sono leve
constexpr uint64_t DESLIGA_US = 30ULL * 60ULL * 1000000ULL;  // dormindo sem toque, desliga
constexpr uint64_t TECLADO_US = 120000;  // dormindo com o CardKB, lê o teclado a cada 120 ms
constexpr uint8_t CARDKB = 0x5F;
constexpr uint32_t I2C_FREQ = 100000;
constexpr uint8_t M5PM1_ADDR = 0x6E;
constexpr uint8_t PM1_BTN_CFG_1 = 0x49;  // bit 0: desliga o reinício por um toque
constexpr uint8_t PM1_BTN_CFG_2 = 0x4A;  // bit 0: desliga o desligamento por dois toques
constexpr uint32_t MAGICO = 0x4C454931;  // "LEI1"
const char COMPILACAO[] = __DATE__ " " __TIME__;

enum : uint8_t { K_ESQ = 0xB4, K_CIMA = 0xB5, K_BAIXO = 0xB6, K_DIR = 0xB7, K_ENTER = 0x0D, K_ESC = 0x1B, K_BS = 0x08 };

enum class Modo : uint8_t { Biblioteca = 0, Secoes = 1, Leitura = 2 };
enum class Acao : uint8_t { Nenhuma, Volta, Escolhe, Avanca, VoltaMuito, Sai, AvancaMuito };

struct Estado {
  uint32_t magico;
  Modo modo;
  uint8_t doc;
  uint8_t selDoc;
  uint8_t reservado;
  uint16_t pagina;
  uint16_t selSecao;
  uint16_t paginaDoc[DOCUMENTOS_QTD];  // onde parou em cada documento
};

struct Pedido {
  Estado estado;
  int bateria;   // %, ou -1
  bool usb;
};

Estado estado{};
Diagramacao diag[DOCUMENTOS_QTD];
Medidas medidas;
Preferences prefs;
TaskHandle_t tarefa = nullptr;
portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;
Pedido pedido{};
Estado noPapel{};         // o último estado mandado ao papel
bool papelValido = false;
volatile bool desenhando = false;
bool sujo = false;
bool teclado = false;
bool exI2c = false;
uint32_t ultimoToque = 0;
uint32_t piscaAte = 0;
uint32_t procuraTecladoMs = 0;
uint32_t desenhos = 0;
uint32_t impressao = 0;  // muda quando o texto ou a diagramação mudam

/* ------------------------------------------------------------ cores e fontes */

uint32_t cor(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color888(r, g, b); }
uint32_t preto() { return cor(0, 0, 0); }
uint32_t branco() { return cor(255, 255, 255); }
uint32_t azul() { return cor(100, 64, 255); }    // cores exatas do papel (sem pontilhado)
uint32_t verde() { return cor(67, 138, 28); }

const lgfx::IFont* fonte(char bloco, uint8_t estilo) {
  if (blocoGrande(bloco)) return &fonts::efontJA_24_b;
  if (bloco == '3' || bloco == '4' || bloco == 'T') return &fonts::efontJA_16_b;
  if (estilo & EST_CODIGO) return &fonts::efontJA_16;
  if (estilo & EST_NEGRITO) return &fonts::efontJA_16_b;
  if ((estilo & EST_ITALICO) || bloco == 'I') return &fonts::efontJA_16_i;
  return &fonts::efontJA_16;
}

uint32_t corDoTexto(char bloco, uint8_t estilo) {
  if (bloco == '2' || bloco == '3') return azul();
  if ((estilo & EST_CODIGO) && bloco != 'C') return verde();
  return preto();
}

/* Texto de até `max` colunas; o que passa vira "...". */
void escreve(const char* s, int x, int y, uint16_t max) {
  char buf[160];
  const uint32_t n = static_cast<uint32_t>(strlen(s));
  if (colunas(s, n) <= max) {
    M5.Display.drawString(s, x, y);
    return;
  }
  uint32_t i = 0;
  uint16_t col = 0;
  while (i < n && i < sizeof(buf) - 4) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    if ((b & 0xC0) != 0x80) {
      if (col + 3 >= max) break;
      ++col;
    }
    buf[i] = s[i];
    ++i;
  }
  while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;  // não parte um caractere
  memcpy(buf + i, "...", 4);
  M5.Display.drawString(buf, x, y);
}

/* ------------------------------------------------------------------ páginas */

void cabecalho(const char* esquerda, const char* direita) {
  M5.Display.setFont(&fonts::efontJA_16_b);
  M5.Display.setTextColor(preto());
  int wd = 0;
  if (direita && *direita) {
    wd = M5.Display.textWidth(direita);
    M5.Display.drawString(direita, W - MARGEM - wd, 9);
  }
  escreve(esquerda, MARGEM, 9, static_cast<uint16_t>((W - 2 * MARGEM - wd - 8) / 8));
  M5.Display.drawFastHLine(0, TOPO - 2, W, preto());
}

void rodape(const char* linha1, const char* linha2, const Pedido& p) {
  M5.Display.drawFastHLine(0, RODAPE_Y, W, preto());
  M5.Display.setFont(&fonts::efontJA_12);
  M5.Display.setTextColor(preto());
  M5.Display.drawString(linha1, MARGEM, RODAPE_Y + 6);
  M5.Display.drawString(linha2, MARGEM, RODAPE_Y + 23);
  char energia[24];
  if (p.usb) snprintf(energia, sizeof(energia), "USB");
  else if (p.bateria >= 0) snprintf(energia, sizeof(energia), "bateria %d%%", p.bateria);
  else energia[0] = 0;
  M5.Display.drawString(energia, W - MARGEM - M5.Display.textWidth(energia), RODAPE_Y + 6);
}

void desenhaLinha(const Documento& doc, const Linha& l, int y) {
  const int cel = blocoGrande(l.bloco) ? medidas.celGrande : medidas.celCorpo;
  const int altFonte = blocoGrande(l.bloco) ? 24 : 16;
  const int ty = y + (l.altura - altFonte) / 2;
  if (l.bloco == 'C') M5.Display.fillRect(MARGEM, y, 3, l.altura, verde());
  if (l.bloco == 'Q') M5.Display.fillRect(MARGEM, y, 3, l.altura, azul());
  if (l.marca & MARCA_INICIO) {
    if (l.bloco == '-') M5.Display.fillCircle(MARGEM + 5, y + l.altura / 2, 3, preto());
    if (l.bloco == '=') M5.Display.drawCircle(MARGEM + 2 * cel + 5, y + l.altura / 2, 3, preto());
  }
  int x = MARGEM + l.recuo * cel + (l.bloco == 'C' ? 4 : 0);
  uint8_t estilo = l.estilo;
  char buf[200];
  size_t n = 0;
  auto despeja = [&]() {
    if (!n) return;
    buf[n] = 0;
    M5.Display.setFont(fonte(l.bloco, estilo));
    M5.Display.setTextColor(corDoTexto(l.bloco, estilo));
    M5.Display.drawString(buf, x, ty);
    x += M5.Display.textWidth(buf);
    n = 0;
  };
  for (uint32_t i = l.inicio; i < l.inicio + l.tamanho; ++i) {
    const char c = doc.texto[i];
    if (c == NEGRITO || c == CODIGO || c == ITALICO) {
      despeja();
      estilo = estiloApos(estilo, &c, 1);
      continue;
    }
    if (n + 1 >= sizeof(buf)) despeja();
    buf[n++] = c;
  }
  despeja();
}

void telaLeitura(const Pedido& p) {
  const Estado& e = p.estado;
  const Documento& doc = DOCUMENTOS[e.doc];
  const Diagramacao& d = diag[e.doc];
  char pag[24];
  snprintf(pag, sizeof(pag), "%u/%u", static_cast<unsigned>(e.pagina + 1), static_cast<unsigned>(d.totalPaginas()));
  cabecalho(doc.titulo, pag);
  int y = CORPO_Y;
  const uint32_t primeira = d.primeiraLinha(e.pagina);
  for (uint32_t i = primeira; i < d.fimDaPagina(e.pagina); ++i) {
    if (i != primeira) y += d.linhas[i].antes;
    desenhaLinha(doc, d.linhas[i], y);
    y += d.linhas[i].altura;
  }
  rodape(e.pagina + 1u < d.totalPaginas() ? "A < página   B seções   C página >" : "A < página   B seções   (fim do documento)",
         "segurar A/C: seção anterior/seguinte; segurar B: biblioteca", p);
}

void telaBiblioteca(const Pedido& p) {
  const Estado& e = p.estado;
  M5.Display.setFont(&fonts::efontJA_24_b);
  M5.Display.setTextColor(preto());
  M5.Display.drawString("Documentação do iot-over-can", MARGEM, 10);
  M5.Display.setFont(&fonts::efontJA_12);
  char sub[64];
  snprintf(sub, sizeof(sub), "versão %s", DOCS_VERSAO);
  M5.Display.drawString(sub, MARGEM, 42);
  M5.Display.drawFastHLine(0, 60, W, preto());
  for (uint8_t n = 0; n < DOCUMENTOS_QTD; ++n) {
    const int y = 68 + n * 49;
    const bool sel = n == e.selDoc;
    if (sel) M5.Display.fillTriangle(MARGEM, y + 3, MARGEM, y + 17, MARGEM + 9, y + 10, azul());
    char titulo[48];
    snprintf(titulo, sizeof(titulo), "%u  %s", static_cast<unsigned>((n + 1) % 10), DOCUMENTOS[n].titulo);
    M5.Display.setFont(&fonts::efontJA_16_b);
    M5.Display.setTextColor(sel ? azul() : preto());
    M5.Display.drawString(titulo, MARGEM + 16, y);
    if (e.paginaDoc[n] > 0) {
      char onde[24];
      snprintf(onde, sizeof(onde), "p. %u/%u", static_cast<unsigned>(e.paginaDoc[n] + 1),
               static_cast<unsigned>(diag[n].totalPaginas()));
      M5.Display.setFont(&fonts::efontJA_12);
      M5.Display.setTextColor(preto());
      M5.Display.drawString(onde, W - MARGEM - M5.Display.textWidth(onde), y + 3);
    }
    M5.Display.setFont(&fonts::efontJA_16);
    M5.Display.setTextColor(preto());
    escreve(DOCUMENTOS[n].resumo, MARGEM + 16, y + 22, 44);
  }
  rodape("A sobe   B abre   C desce", "cada documento abre onde você parou", p);
}

/* Entradas do índice: 0 é o início do documento; as outras, as seções. */
uint32_t entradas(uint8_t doc) { return 1 + static_cast<uint32_t>(diag[doc].secoes.size()); }

uint32_t paginaDaEntrada(uint8_t doc, uint32_t i) {
  return i == 0 ? 0 : diag[doc].paginaDaLinha(diag[doc].secoes[i - 1].linha);
}

void textoDaEntrada(uint8_t doc, uint32_t i, char* out, size_t tam) {
  if (i == 0) {
    snprintf(out, tam, "Início");
    return;
  }
  // O título inteiro, mesmo quando ocupa mais de uma linha na página.
  const Documento& d = DOCUMENTOS[doc];
  size_t n = 0;
  for (uint32_t k = diag[doc].linhas[diag[doc].secoes[i - 1].linha].inicio; k < d.tamanho && d.texto[k] != '\n';) {
    const uint8_t b = static_cast<uint8_t>(d.texto[k]);
    if (b == NEGRITO || b == CODIGO || b == ITALICO) {
      ++k;
      continue;
    }
    const uint32_t bytes = b < 0x80 ? 1 : (b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2));  // caractere inteiro
    if (n + bytes >= tam) break;
    for (uint32_t j = 0; j < bytes && k < d.tamanho; ++j) out[n++] = d.texto[k++];
  }
  out[n] = 0;
}

void telaSecoes(const Pedido& p) {
  const Estado& e = p.estado;
  char direita[16];
  snprintf(direita, sizeof(direita), "%u seções", static_cast<unsigned>(diag[e.doc].secoes.size()));
  cabecalho(DOCUMENTOS[e.doc].titulo, direita);
  const uint32_t total = entradas(e.doc);
  const uint32_t visiveis = (RODAPE_Y - CORPO_Y - 4) / 22;
  uint32_t inicio = e.selSecao > visiveis / 2 ? e.selSecao - visiveis / 2 : 0;
  if (total > visiveis && inicio > total - visiveis) inicio = total - visiveis;
  if (total <= visiveis) inicio = 0;
  for (uint32_t i = inicio; i < total && i < inicio + visiveis; ++i) {
    const int y = CORPO_Y + static_cast<int>(i - inicio) * 22;
    const bool sel = i == e.selSecao;
    const bool sub = i > 0 && diag[e.doc].secoes[i - 1].nivel == 3;
    if (sel) M5.Display.fillTriangle(MARGEM, y + 5, MARGEM, y + 17, MARGEM + 8, y + 11, azul());
    char texto[120];
    textoDaEntrada(e.doc, i, texto, sizeof(texto));
    char pag[12];
    snprintf(pag, sizeof(pag), "%u", static_cast<unsigned>(paginaDaEntrada(e.doc, i) + 1));
    M5.Display.setFont(sub ? &fonts::efontJA_16 : &fonts::efontJA_16_b);
    M5.Display.setTextColor(sel ? azul() : preto());
    const int x = MARGEM + 14 + (sub ? 16 : 0);
    const int wp = M5.Display.textWidth(pag);
    M5.Display.drawString(pag, W - MARGEM - wp, y + 3);
    escreve(texto, x, y + 3, static_cast<uint16_t>((W - MARGEM - wp - 10 - x) / 8));
  }
  rodape("A sobe   B abre a seção   C desce", "segurar A ou C: dez de cada vez; segurar B: volta ao texto", p);
}

/* ------------------------------------------------------------------ estado */

uint32_t fnv(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 16777619u;
  }
  return h;
}

/* Impressão digital do texto e da diagramação: se mudar, as páginas guardadas
 * não valem mais. */
uint32_t calculaImpressao() {
  uint32_t h = 2166136261u;
  for (uint8_t n = 0; n < DOCUMENTOS_QTD; ++n) {
    for (uint32_t i = 0; i < DOCUMENTOS[n].tamanho; ++i) h = (h ^ static_cast<uint8_t>(DOCUMENTOS[n].texto[i])) * 16777619u;
    h = fnv(h, diag[n].totalPaginas());
    h = fnv(h, static_cast<uint32_t>(diag[n].linhas.size()));
  }
  return h;
}

/* O que está no papel. Inclui a compilação: depois de gravar um firmware novo,
 * a tela é redesenhada. */
uint32_t assinatura(const Estado& e) {
  uint32_t h = fnv(2166136261u, impressao);
  for (const char* c = COMPILACAO; *c; ++c) h = fnv(h, static_cast<uint8_t>(*c));
  h = fnv(h, static_cast<uint8_t>(e.modo));
  if (e.modo == Modo::Biblioteca) {
    h = fnv(h, e.selDoc);
    for (uint8_t n = 0; n < DOCUMENTOS_QTD; ++n) h = fnv(h, e.paginaDoc[n]);
  } else {
    h = fnv(h, e.doc);
    h = fnv(h, e.modo == Modo::Leitura ? e.pagina : e.selSecao);
  }
  return h;
}

void estadoInicial() {
  memset(&estado, 0, sizeof(estado));
  estado.magico = MAGICO;
  estado.modo = Modo::Biblioteca;
}

void validaEstado() {
  if (estado.doc >= DOCUMENTOS_QTD) estado.doc = 0;
  if (estado.selDoc >= DOCUMENTOS_QTD) estado.selDoc = 0;
  if (static_cast<uint8_t>(estado.modo) > 2) estado.modo = Modo::Biblioteca;
  for (uint8_t n = 0; n < DOCUMENTOS_QTD; ++n)
    if (estado.paginaDoc[n] >= diag[n].totalPaginas()) estado.paginaDoc[n] = 0;
  if (estado.pagina >= diag[estado.doc].totalPaginas()) estado.pagina = 0;
  if (estado.selSecao >= entradas(estado.doc)) estado.selSecao = 0;
}

/* true se a página que está no papel é a deste estado. */
bool carregaEstado() {
  estadoInicial();
  prefs.begin("leitor", false);
  if (prefs.getUInt("diag", 0) != impressao) {  // documentação ou diagramação nova: começa da biblioteca
    prefs.remove("estado");
    prefs.putUInt("tela", 0);
    prefs.putUInt("diag", impressao);
    return false;
  }
  Estado lido{};
  if (prefs.getBytes("estado", &lido, sizeof(lido)) == sizeof(lido) && lido.magico == MAGICO) estado = lido;
  validaEstado();
  return prefs.getUInt("tela", 0) == assinatura(estado);
}

/* -------------------------------------------------------------- navegação */

uint32_t secaoDaPagina(uint8_t doc, uint32_t pagina) {
  uint32_t atual = 0;
  for (uint32_t i = 1; i < entradas(doc); ++i) {
    if (paginaDaEntrada(doc, i) <= pagina) atual = i;
    else break;
  }
  return atual;
}

void abre(uint8_t doc) {
  estado.doc = doc;
  estado.modo = Modo::Leitura;
  estado.pagina = estado.paginaDoc[doc];
}

void aplica(Acao a) {
  if (a == Acao::Nenhuma) return;
  Estado& e = estado;
  const Estado antes = estado;
  switch (e.modo) {
    case Modo::Leitura: {
      const uint32_t total = diag[e.doc].totalPaginas();
      const uint32_t secao = secaoDaPagina(e.doc, e.pagina);
      if (a == Acao::Volta && e.pagina > 0) --e.pagina;
      if (a == Acao::Avanca && e.pagina + 1u < total) ++e.pagina;
      if (a == Acao::Escolhe) {
        e.modo = Modo::Secoes;
        e.selSecao = static_cast<uint16_t>(secao);
      }
      if (a == Acao::VoltaMuito) {  // início da seção atual; se ela começa nesta página, a anterior
        uint32_t destino = 0;
        for (uint32_t i = 0; i < entradas(e.doc); ++i) {
          const uint32_t pg = paginaDaEntrada(e.doc, i);
          if (pg >= e.pagina) break;
          destino = pg;
        }
        e.pagina = static_cast<uint16_t>(destino);
      }
      if (a == Acao::AvancaMuito) {
        for (uint32_t i = secao + 1; i < entradas(e.doc); ++i) {
          const uint32_t pg = paginaDaEntrada(e.doc, i);
          if (pg > e.pagina) {
            e.pagina = static_cast<uint16_t>(pg);
            break;
          }
        }
      }
      if (a == Acao::Sai) {
        e.modo = Modo::Biblioteca;
        e.selDoc = e.doc;
      }
      e.paginaDoc[e.doc] = e.pagina;
      break;
    }
    case Modo::Secoes: {
      const int32_t total = static_cast<int32_t>(entradas(e.doc));
      int32_t s = e.selSecao;
      if (a == Acao::Volta) s -= 1;
      if (a == Acao::Avanca) s += 1;
      if (a == Acao::VoltaMuito) s -= 10;
      if (a == Acao::AvancaMuito) s += 10;
      e.selSecao = static_cast<uint16_t>(s < 0 ? 0 : (s >= total ? total - 1 : s));
      if (a == Acao::Escolhe) {
        e.pagina = static_cast<uint16_t>(paginaDaEntrada(e.doc, e.selSecao));
        e.paginaDoc[e.doc] = e.pagina;
        e.modo = Modo::Leitura;
      }
      if (a == Acao::Sai) e.modo = Modo::Leitura;
      break;
    }
    case Modo::Biblioteca:
      if (a == Acao::Volta || a == Acao::VoltaMuito) e.selDoc = static_cast<uint8_t>((e.selDoc + DOCUMENTOS_QTD - 1) % DOCUMENTOS_QTD);
      if (a == Acao::Avanca || a == Acao::AvancaMuito) e.selDoc = static_cast<uint8_t>((e.selDoc + 1) % DOCUMENTOS_QTD);
      if (a == Acao::Escolhe) abre(e.selDoc);
      break;
  }
  if (memcmp(&antes, &estado, sizeof(estado)) != 0) sujo = true;  // nada mudou (ex.: C na última página): não redesenha
  ultimoToque = millis();
  piscaAte = millis() + 120;
}

Acao daTecla(uint8_t k) {
  if (estado.modo == Modo::Biblioteca && k >= '0' && k <= '9') {
    const uint8_t n = static_cast<uint8_t>(k == '0' ? 9 : k - '1');
    if (n < DOCUMENTOS_QTD) {
      estado.selDoc = n;
      return Acao::Escolhe;
    }
    return Acao::Nenhuma;
  }
  const bool leitura = estado.modo == Modo::Leitura;
  switch (k) {
    case K_ESQ: return Acao::Volta;
    case K_DIR: return Acao::Avanca;
    case K_CIMA: return leitura ? Acao::VoltaMuito : Acao::Volta;
    case K_BAIXO: return leitura ? Acao::AvancaMuito : Acao::Avanca;
    case K_ENTER: return Acao::Escolhe;
    case K_ESC:
    case K_BS: return Acao::Sai;
    case ' ': return Acao::Avanca;
    default: return Acao::Nenhuma;
  }
}

Acao daSerial(int c) {
  switch (c) {
    case 'a': return Acao::Volta;
    case 'b': return Acao::Escolhe;
    case 'c': return Acao::Avanca;
    case 'A': return Acao::VoltaMuito;
    case 'B': return Acao::Sai;
    case 'C': return Acao::AvancaMuito;
    default: return Acao::Nenhuma;
  }
}

/* --------------------------------------------------------------- teclado */

void procuraTeclado() {
  if (!exI2c || (procuraTecladoMs && millis() - procuraTecladoMs < (teclado ? 5000u : 2000u))) return;
  procuraTecladoMs = millis();
  const bool achou = M5.Ex_I2C.scanID(CARDKB, I2C_FREQ);
  if (achou != teclado) Serial.printf("#LEITOR teclado CardKB %s\n", achou ? "conectado" : "ausente");
  teclado = achou;
}

uint8_t leTecla() {
  if (!teclado) return 0;
  uint8_t c = 0;
  if (!M5.Ex_I2C.start(CARDKB, true, I2C_FREQ)) return 0;
  const bool ok = M5.Ex_I2C.read(&c, 1, true);
  M5.Ex_I2C.stop();
  if (!ok) {
    teclado = false;
    return 0;
  }
  return c;
}

/* ------------------------------------------------------------------ desenho */

// O papel leva de 15 a 30 s: o desenho roda numa tarefa própria, e o laço
// continua lendo os botões. Só esta tarefa toca a tela e grava na NVS.
void tarefaDesenho(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Pedido p;
    portENTER_CRITICAL(&trava);
    p = pedido;
    portEXIT_CRITICAL(&trava);
    prefs.putUInt("tela", 0);  // se faltar energia no meio do desenho, a página é redesenhada ao ligar
    const uint32_t t0 = millis();
    M5.Display.startWrite();
    M5.Display.fillScreen(branco());
    if (p.estado.modo == Modo::Leitura) telaLeitura(p);
    else if (p.estado.modo == Modo::Secoes) telaSecoes(p);
    else telaBiblioteca(p);
    M5.Display.endWrite();
    M5.Display.display();
    M5.Display.waitDisplay();
    prefs.putBytes("estado", &p.estado, sizeof(p.estado));
    prefs.putUInt("tela", assinatura(p.estado));
    ++desenhos;
    Serial.printf("#LEITOR desenho %lu pronto em %lu ms\n", static_cast<unsigned long>(desenhos),
                  static_cast<unsigned long>(millis() - t0));
    desenhando = false;
  }
}

void pedeDesenho() {
  const int32_t bat = M5.Power.getBatteryLevel();  // I2C: só no laço, nunca na tarefa
  portENTER_CRITICAL(&trava);
  pedido.estado = estado;
  pedido.bateria = bat >= 0 && bat <= 100 ? static_cast<int>(bat) : -1;
  pedido.usb = M5.Power.getVBUSVoltage() > 4300;
  portEXIT_CRITICAL(&trava);
  noPapel = estado;
  papelValido = true;
  sujo = false;
  desenhando = true;
  xTaskNotifyGive(tarefa);
}

void mostraEstado() {
  static const char* const modos[] = {"biblioteca", "secoes", "leitura"};
  Serial.printf("#LEITOR modo=%s doc=%u pagina=%u/%u secao=%u teclado=%s desenhando=%s desenhos=%lu\n",
                modos[static_cast<uint8_t>(estado.modo)], static_cast<unsigned>(estado.doc + 1),
                static_cast<unsigned>(estado.pagina + 1), static_cast<unsigned>(diag[estado.doc].totalPaginas()),
                static_cast<unsigned>(estado.selSecao), teclado ? "sim" : "nao", desenhando ? "sim" : "nao",
                static_cast<unsigned long>(desenhos));
}

/* -------------------------------------------------------------------- LED */

void led() {
  static uint8_t ultimo = 0xFF;
  const uint8_t agora = millis() < piscaAte ? 2 : (desenhando ? 1 : 0);
  if (agora == ultimo) return;
  ultimo = agora;
  if (agora == 2) M5.Led.setAllColor(40, 40, 40);  // toque recebido
  else if (agora == 1) M5.Led.setAllColor(0, 0, 24);  // o papel está desenhando
  else M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
}

/* ------------------------------------------------------------------- sono */

/* Botão de energia no padrão de fábrica do M5PM1 (um toque liga ou reinicia,
 * dois toques desligam), mesmo que outro firmware o tenha deixado diferente. */
void botaoDeFabrica() {
  M5.In_I2C.bitOff(M5PM1_ADDR, PM1_BTN_CFG_1, 0x01, I2C_FREQ);
  M5.In_I2C.bitOff(M5PM1_ADDR, PM1_BTN_CFG_2, 0x01, I2C_FREQ);
}

void desliga() {
  Serial.println("#LEITOR desligando; o botão de energia religa onde parou");
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  M5.Power.setExtOutput(false);  // 5 V do CardKB
  M5.Power.M5pm1.setLDOOutput(false);
  M5.Display.sleep();
  M5.Display.waitDisplay();
  Serial.flush();
  M5.Power.M5pm1.powerOff();
  delay(1500);
  // Ainda alimentado (USB): espera sem fonte de despertar; o botão de energia reinicia pelo M5PM1.
  esp_deep_sleep_start();
}

/* Sono leve com a página no papel. Um botão acorda; com o CardKB, o ESP32
 * acorda a cada 120 ms só para ler o teclado. */
void dorme() {
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  Serial.println("#LEITOR dormindo; um botão acorda");
  Serial.flush();
  const gpio_num_t botoes[] = {GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_1};  // A, B, C (ativos em nível baixo)
  for (gpio_num_t b : botoes) gpio_wakeup_enable(b, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  uint64_t dormiu = 0;
  uint8_t tecla = 0;
  while (dormiu < DESLIGA_US && !tecla) {
    const uint64_t passo = teclado ? TECLADO_US : DESLIGA_US - dormiu;
    esp_sleep_enable_timer_wakeup(passo);
    esp_light_sleep_start();
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) break;  // um botão
    dormiu += passo;
    if (teclado) tecla = leTecla();
  }
  for (gpio_num_t b : botoes) gpio_wakeup_disable(b);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  if (dormiu >= DESLIGA_US) desliga();
  ultimoToque = millis();
  if (tecla) aplica(daTecla(tecla));
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.clear_display = false;  // a página da última leitura continua no papel
  cfg.output_power = true;    // 5 V na Port A para o CardKB
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5.begin(cfg);
  botaoDeFabrica();
  M5.Power.setExtOutput(true);
  M5.Led.setAutoDisplay(false);
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  M5.Display.setRotation(0);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.setAutoDisplay(false);  // o papel só desenha no display() da tarefa
  M5.Display.setTextWrap(false);
  M5.BtnA.setHoldThresh(700);
  M5.BtnB.setHoldThresh(700);
  M5.BtnC.setHoldThresh(700);
  exI2c = M5.Ex_I2C.begin();  // a Port A não é iniciada pelo M5Unified no PaperColor

  medidas.altura = static_cast<uint16_t>(RODAPE_Y - CORPO_Y - 4);
  for (uint8_t n = 0; n < DOCUMENTOS_QTD; ++n) diagramar(DOCUMENTOS[n].texto, DOCUMENTOS[n].tamanho, medidas, diag[n]);
  impressao = calculaImpressao();
  const bool naTela = carregaEstado();
  if (naTela) {
    noPapel = estado;
    papelValido = true;
  }
  Serial.printf("#LEITOR documentação %s, %u documentos; tela %s\n", DOCS_VERSAO, static_cast<unsigned>(DOCUMENTOS_QTD),
                naTela ? "já mostra a página" : "precisa ser desenhada");

  xTaskCreatePinnedToCore(tarefaDesenho, "papel", 8192, nullptr, 1, &tarefa, 0);
  ultimoToque = millis();
  if (!naTela) pedeDesenho();
}

void loop() {
  M5.update();
  // Um toque decide no soltar; segurar decide ao passar do limite, sem esperar o soltar.
  if (M5.BtnA.wasHold()) aplica(Acao::VoltaMuito);
  else if (M5.BtnA.wasClicked()) aplica(Acao::Volta);
  if (M5.BtnB.wasHold()) aplica(Acao::Sai);
  else if (M5.BtnB.wasClicked()) aplica(Acao::Escolhe);
  if (M5.BtnC.wasHold()) aplica(Acao::AvancaMuito);
  else if (M5.BtnC.wasClicked()) aplica(Acao::Avanca);

  procuraTeclado();
  for (uint8_t k = leTecla(), n = 0; k && n < 8; k = leTecla(), ++n) aplica(daTecla(k));
  while (Serial.available()) {
    const int c = Serial.read();
    if (c == 's') mostraEstado();
    else aplica(daSerial(c));
  }

  if (sujo && !desenhando && millis() - ultimoToque >= ESPERA_MS) {
    if (papelValido && memcmp(&estado, &noPapel, sizeof(estado)) == 0) sujo = false;  // voltou à página que está no papel
    else pedeDesenho();
  }
  led();
  if (!sujo && !desenhando && millis() - ultimoToque >= SONO_MS) {
    if (M5.Power.getVBUSVoltage() > 4300) ultimoToque = millis();  // na USB não dorme: a serial continua
    else dorme();
  }
  delay(10);
}
