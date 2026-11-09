/* Diagramação do leitor: quebra de linha, paginação e seções, com textos
 * sintéticos e com a documentação real (documentos.h gerado na hora). */
#include <cstdio>
#include <cstring>
#include <string>

#include "documentos.h"
#include "leitor_layout.h"

using namespace leitor;

static int falhas = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      ++falhas;                                                          \
    }                                                                    \
  } while (0)

static uint16_t totalColunas(const Medidas& m, char bloco) {
  return static_cast<uint16_t>(m.largura / (blocoGrande(bloco) ? m.celGrande : m.celCorpo));
}

static std::string semEspacos(const char* s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i)
    if (s[i] != ' ') out += s[i];
  return out;
}

/* Confere uma diagramação: largura, altura das páginas, títulos e cobertura do texto. */
static void confere(const char* texto, uint32_t tamanho, const Medidas& m, const Diagramacao& d, const char* nome) {
  for (const Linha& l : d.linhas) {
    const uint16_t total = totalColunas(m, l.bloco);
    if (colunas(texto + l.inicio, l.tamanho) + l.recuo > total) {
      std::printf("FALHA %s: linha larga demais em %u\n", nome, static_cast<unsigned>(l.inicio));
      ++falhas;
    }
  }
  for (uint32_t p = 0; p < d.totalPaginas(); ++p) {
    uint32_t y = 0;
    for (uint32_t i = d.primeiraLinha(p); i < d.fimDaPagina(p); ++i) {
      y += d.linhas[i].altura + (i == d.primeiraLinha(p) ? 0 : d.linhas[i].antes);
    }
    if (y > m.altura) {
      std::printf("FALHA %s: página %u com %u px\n", nome, static_cast<unsigned>(p + 1), static_cast<unsigned>(y));
      ++falhas;
    }
    const uint32_t ultima = d.fimDaPagina(p) - 1;
    if (p + 1 < d.totalPaginas() && d.linhas[ultima].bloco >= '1' && d.linhas[ultima].bloco <= '4') {
      std::printf("FALHA %s: título no pé da página %u\n", nome, static_cast<unsigned>(p + 1));
      ++falhas;
    }
  }
  for (const Secao& s : d.secoes) {
    CHECK(s.linha < d.linhas.size());
    CHECK(d.linhas[s.linha].bloco == static_cast<char>('0' + s.nivel));
    CHECK(d.linhas[s.linha].marca & MARCA_INICIO);
  }
  // Nada se perde: cada bloco, sem os espaços, é a soma das suas linhas.
  std::string esperado, obtido;
  for (uint32_t i = 0; i < tamanho;) {
    uint32_t fim = i;
    while (fim < tamanho && texto[fim] != '\n') ++fim;
    if (fim > i) esperado += semEspacos(texto + i + 1, fim - i - 1);
    i = fim + 1;
  }
  for (const Linha& l : d.linhas) obtido += semEspacos(texto + l.inicio, l.tamanho);
  if (esperado != obtido) {
    std::printf("FALHA %s: o texto diagramado difere do original\n", nome);
    ++falhas;
  }
}

static void testeBasico() {
  const char s[] = "a\001\xc3\xa7\xc3\xa3o\001";
  CHECK(colunas(s, sizeof(s) - 1) == 4);
  CHECK(estiloApos(0, s, 2) == EST_NEGRITO);
  CHECK(estiloApos(0, s, sizeof(s) - 1) == 0);

  Medidas m;
  m.largura = 80;  // 10 colunas
  m.altura = 66;   // 3 linhas de 22 px
  const std::string texto =
      "Puma palavra outra coisa \001negrito aqui\001 fim\n"
      "-item de lista que quebra\n"
      "Cabcdefghijklmnopqrstuvwxyz\n"
      "C    recuado\n"
      "Psupercalifragilisticexpialidocious\n"
      "2Titulo\n"
      "Ptexto depois do titulo\n";
  Diagramacao d;
  diagramar(texto.data(), static_cast<uint32_t>(texto.size()), m, d);
  confere(texto.data(), static_cast<uint32_t>(texto.size()), m, d, "sintetico");
  // O estilo continua na linha seguinte quando o negrito quebra.
  bool negritoContinua = false;
  for (const Linha& l : d.linhas)
    if (l.bloco == 'P' && l.estilo == EST_NEGRITO) negritoContinua = true;
  CHECK(negritoContinua);
  // Código mantém os espaços do início e quebra com recuo maior.
  bool recuado = false, continuacao = false;
  for (const Linha& l : d.linhas) {
    if (l.bloco == 'C' && std::strncmp(texto.data() + l.inicio, "    recuado", 11) == 0) recuado = true;
    if (l.bloco == 'C' && !(l.marca & MARCA_INICIO) && l.recuo == 3) continuacao = true;
  }
  CHECK(recuado);
  CHECK(continuacao);
  CHECK(d.secoes.size() == 1);
  CHECK(d.totalPaginas() > 1);
  CHECK(d.paginaDaLinha(0) == 0);
  CHECK(d.paginaDaLinha(static_cast<uint32_t>(d.linhas.size() - 1)) == d.totalPaginas() - 1);

  // Seção logo seguida de subseção: as duas passam juntas para a página seguinte.
  Medidas m2;
  m2.largura = 80;
  m2.altura = 140;
  const std::string t2 = "Pa\nPb\n2T\n3S\nPc\n";
  Diagramacao d2;
  diagramar(t2.data(), static_cast<uint32_t>(t2.size()), m2, d2);
  confere(t2.data(), static_cast<uint32_t>(t2.size()), m2, d2, "títulos seguidos");
  CHECK(d2.totalPaginas() == 2);
  CHECK(d2.primeiraLinha(1) == 2);
}

static void testeDocumentos() {
  Medidas m;
  uint32_t paginas = 0;
  for (uint8_t n = 0; n < DOCUMENTOS_QTD; ++n) {
    const Documento& doc = DOCUMENTOS[n];
    Diagramacao d;
    diagramar(doc.texto, doc.tamanho, m, d);
    confere(doc.texto, doc.tamanho, m, d, doc.titulo);
    CHECK(!d.linhas.empty());
    CHECK(d.linhas[0].bloco == '1');
    CHECK(!d.secoes.empty());
    paginas += d.totalPaginas();
    std::printf("  %-16s %3u páginas, %2u seções\n", doc.titulo, static_cast<unsigned>(d.totalPaginas()),
                static_cast<unsigned>(d.secoes.size()));
  }
  std::printf("  total: %u páginas\n", static_cast<unsigned>(paginas));
}

int main() {
  testeBasico();
  testeDocumentos();
  if (falhas) {
    std::printf("%d falha(s)\n", falhas);
    return 1;
  }
  std::printf("leitor: diagramação OK\n");
  return 0;
}
