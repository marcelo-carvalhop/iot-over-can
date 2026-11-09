/* O firmware do leitor (src/main.cpp) rodando no computador sobre os stubs de
 * tests/stubs: todas as telas cabem no papel; a navegação; o papel só é
 * redesenhado quando a tela muda; a NVS; o teclado, o sono e o desligamento. */
#include "../src/main.cpp"

#include <chrono>

static int falhas = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      ++falhas;                                                          \
    }                                                                    \
  } while (0)

/* Espera a tarefa de desenho (tempo real; o relógio do firmware fica parado). */
static void esperaPapel() {
  for (int i = 0; i < 3000 && desenhando; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(!desenhando);
}

/* Roda o laço por `ms` do relógio do firmware. */
static void roda(uint32_t ms) {
  const uint32_t fim = millis() + ms;
  while (millis() < fim) {
    loop();
    esperaPapel();
  }
}

static void toca(BotaoStub& b) {
  b.proximoClique = true;
  roda(50);
}

static void segura(BotaoStub& b) {
  b.proximoSegura = true;
  roda(50);
}

/* Desenha cada página, cada índice e cada item da biblioteca. */
static void todasAsTelas() {
  Pedido p{};
  p.estado = estado;
  p.bateria = 100;
  uint32_t telas = 0;
  for (uint8_t d = 0; d < DOCUMENTOS_QTD; ++d) {
    p.estado.doc = d;
    p.estado.modo = Modo::Leitura;
    for (uint32_t pg = 0; pg < diag[d].totalPaginas(); ++pg, ++telas) {
      p.estado.pagina = static_cast<uint16_t>(pg);
      telaLeitura(p);
    }
    p.estado.modo = Modo::Secoes;
    for (uint32_t s = 0; s < entradas(d); ++s, ++telas) {
      p.estado.selSecao = static_cast<uint16_t>(s);
      telaSecoes(p);
    }
    p.estado.paginaDoc[d] = static_cast<uint16_t>(diag[d].totalPaginas() - 1);
  }
  p.estado.modo = Modo::Biblioteca;
  p.usb = true;
  for (uint8_t d = 0; d < DOCUMENTOS_QTD; ++d, ++telas) {
    p.estado.selDoc = d;
    telaBiblioteca(p);
  }
  std::printf("  %u telas desenhadas\n", static_cast<unsigned>(telas));
}

int main() {
  setup();
  esperaPapel();
  CHECK(!stub::autoDisplay);
  CHECK(stub::noPapel == 1);  // primeiro boot: a biblioteca
  CHECK(estado.modo == Modo::Biblioteca);
  todasAsTelas();

  // Biblioteca: C desce, B abre o documento.
  const int base = stub::noPapel;
  toca(M5.BtnC);
  roda(1000);
  CHECK(estado.selDoc == 1);
  CHECK(stub::noPapel == base + 1);
  toca(M5.BtnB);
  roda(1000);
  CHECK(estado.modo == Modo::Leitura && estado.doc == 1 && estado.pagina == 0);
  CHECK(stub::noPapel == base + 2);

  // A na primeira página não muda nada: o papel não é redesenhado.
  toca(M5.BtnA);
  roda(1000);
  CHECK(stub::noPapel == base + 2);
  // C e A antes de o papel desenhar: a página volta a ser a que já está nele.
  toca(M5.BtnC);
  toca(M5.BtnA);
  roda(1000);
  CHECK(stub::noPapel == base + 2);
  // Três toques seguidos viram um desenho só.
  toca(M5.BtnC);
  toca(M5.BtnC);
  toca(M5.BtnC);
  roda(1000);
  CHECK(estado.pagina == 3);
  CHECK(stub::noPapel == base + 3);

  // Segurar C: a próxima seção; segurar A: o começo da seção (ou a anterior).
  const uint8_t doc = estado.doc;
  uint32_t pg = estado.pagina;
  segura(M5.BtnC);
  roda(1000);
  CHECK(estado.pagina > pg);
  CHECK(paginaDaEntrada(doc, secaoDaPagina(doc, estado.pagina)) == estado.pagina);
  pg = estado.pagina;
  segura(M5.BtnA);
  roda(1000);
  CHECK(estado.pagina < pg);
  CHECK(paginaDaEntrada(doc, secaoDaPagina(doc, estado.pagina)) == estado.pagina);

  // B: índice na seção atual; C e B abrem a seção seguinte.
  toca(M5.BtnB);
  roda(1000);
  CHECK(estado.modo == Modo::Secoes);
  const uint16_t sel = estado.selSecao;
  CHECK(sel == secaoDaPagina(doc, estado.pagina));
  toca(M5.BtnC);
  toca(M5.BtnB);
  roda(1000);
  CHECK(estado.modo == Modo::Leitura);
  CHECK(estado.pagina == paginaDaEntrada(doc, sel + 1u));
  const uint16_t parou = estado.pagina;

  // Segurar B: biblioteca; o documento reabre onde parou.
  segura(M5.BtnB);
  roda(1000);
  CHECK(estado.modo == Modo::Biblioteca && estado.selDoc == doc && estado.paginaDoc[doc] == parou);
  toca(M5.BtnB);
  roda(1000);
  CHECK(estado.modo == Modo::Leitura && estado.pagina == parou);

  // Serial: c avança, s mostra o estado.
  Serial.entrada = "cs";
  roda(1000);
  CHECK(estado.pagina == parou + 1u);
  CHECK(Serial.saida.find("#LEITOR modo=leitura") != std::string::npos);

  // Novo boot com a mesma página no papel: nada é redesenhado.
  const Estado desenhado = estado;
  int antes = stub::noPapel;
  setup();
  esperaPapel();
  roda(1000);
  CHECK(stub::noPapel == antes);
  CHECK(memcmp(&estado, &desenhado, sizeof(estado)) == 0);

  // Faltou energia no meio do desenho: a posição fica, a página é redesenhada.
  prefs.putUInt("tela", 0);
  CHECK(!carregaEstado());
  CHECK(memcmp(&estado, &desenhado, sizeof(estado)) == 0);
  // Documentação nova: começa da biblioteca.
  impressao ^= 1;
  CHECK(!carregaEstado());
  CHECK(estado.modo == Modo::Biblioteca && estado.paginaDoc[doc] == 0);
  pedeDesenho();
  esperaPapel();

  // CardKB: aparece em até 2 s; na biblioteca, 3 abre o terceiro documento.
  stub::teclado = true;
  roda(2500);
  CHECK(teclado);
  stub::tecla = '3';
  roda(1000);
  CHECK(estado.modo == Modo::Leitura && estado.doc == 2 && estado.pagina == 0);

  // Na bateria, 20 s sem uso: sono leve. Com o teclado, uma tecla acorda e vira a página.
  stub::vbus = 0;
  stub::botaoAcorda = false;
  stub::teclaAoAcordar = K_DIR;
  int sonos = stub::sonos;
  roda(22000);
  CHECK(stub::sonos > sonos);
  CHECK(estado.pagina == 1);
  stub::teclado = false;
  roda(5000);
  CHECK(!teclado);

  // Um botão acorda.
  stub::botaoAcorda = true;
  sonos = stub::sonos;
  roda(21000);
  CHECK(stub::sonos > sonos);
  CHECK(!stub::desligado);

  // Na USB não dorme (a serial continua).
  stub::vbus = 5000;
  sonos = stub::sonos;
  roda(60000);
  CHECK(stub::sonos == sonos);

  // 30 min dormindo sem toque: desliga, e a página fica no papel.
  stub::vbus = 0;
  stub::botaoAcorda = false;
  antes = stub::noPapel;
  bool desligou = false;
  try {
    roda(25000);
  } catch (const stub::Desligou&) {
    desligou = true;
  }
  CHECK(desligou && stub::desligado);
  CHECK(stub::noPapel == antes);

  if (stub::foraDaTela || stub::controleNaTela) {
    std::printf("FALHA %d desenho(s) fora da tela, %d com byte de controle; último: %s\n", stub::foraDaTela.load(),
                stub::controleNaTela.load(), stub::ultimoErro.c_str());
    ++falhas;
  }
  if (falhas) {
    std::printf("%d falha(s)\n", falhas);
    std::fflush(stdout);
    std::_Exit(1);
  }
  std::printf("leitor: firmware OK\n");
  std::fflush(stdout);
  std::_Exit(0);  // a tarefa de desenho fica bloqueada para sempre, como no ESP32
}
