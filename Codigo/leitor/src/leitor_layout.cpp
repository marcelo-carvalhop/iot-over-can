#include "leitor_layout.h"

namespace leitor {

namespace {

struct Forma {
  uint8_t primeiro;   // recuo da primeira linha
  uint8_t seguinte;   // recuo das linhas seguintes
  uint8_t altura;
  uint8_t antes;
};

Forma forma(char bloco) {
  switch (bloco) {
    case '1': return {0, 0, 34, 0};
    case '2': return {0, 0, 32, 16};
    case '3': return {0, 0, 24, 12};
    case '4': return {0, 0, 22, 10};
    case '-': return {2, 2, 22, 4};
    case '=': return {4, 4, 22, 2};
    case '#': return {0, 3, 22, 4};
    case 'C': return {1, 3, 20, 8};
    case 'Q': return {2, 2, 22, 8};
    case 'T': return {0, 2, 22, 10};
    case 't': return {2, 4, 22, 0};
    case 'D': return {0, 2, 22, 4};
    case 'I': return {0, 0, 22, 8};
    default: return {0, 0, 22, 8};  // 'P' e desconhecidos
  }
}

bool controle(uint8_t b) { return b >= 1 && b <= 3; }
bool inicioDeCaractere(uint8_t b) { return (b & 0xC0) != 0x80; }

/* Byte em que o trecho [s, fim) atinge `limite` colunas, sem partir um caractere. */
uint32_t corte(const char* s, uint32_t inicio, uint32_t fim, uint16_t limite) {
  uint16_t col = 0;
  for (uint32_t i = inicio; i < fim; ++i) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    if (controle(b) || !inicioDeCaractere(b)) continue;
    if (col == limite) return i;
    ++col;
  }
  return fim;
}

uint8_t recuoNumerado(const char* s, uint32_t inicio, uint32_t fim) {
  // "12. texto": as linhas seguintes alinham depois do número.
  uint32_t i = inicio;
  while (i < fim && s[i] >= '0' && s[i] <= '9') ++i;
  if (i < fim && (s[i] == '.' || s[i] == ')')) ++i;
  if (i < fim && s[i] == ' ') ++i;
  const uint32_t n = i - inicio;
  return n > 0 && n < 8 ? static_cast<uint8_t>(n) : 3;
}

class Diagramador {
 public:
  Diagramador(const char* texto, const Medidas& m, Diagramacao& out) : t_(texto), m_(m), out_(out) {}

  void bloco(char codigo, uint32_t inicio, uint32_t fim, char anterior) {
    const Forma f = forma(codigo);
    const uint8_t cel = blocoGrande(codigo) ? m_.celGrande : m_.celCorpo;
    const uint16_t total = static_cast<uint16_t>(m_.largura / cel);
    uint8_t antes = f.antes;
    if ((codigo == 'C' && anterior == 'C') || (codigo == 'D' && anterior == 'D') ||
        ((codigo == '-' || codigo == '=') && (anterior == '-' || anterior == '='))) {
      antes = codigo == 'C' ? 0 : 2;
    }
    seguinte_ = codigo == '#' ? recuoNumerado(t_, inicio, fim) : f.seguinte;
    primeiro_ = f.primeiro;
    codigo_ = codigo;
    altura_ = f.altura;
    antes_ = antes;
    estilo_ = 0;
    primeiraLinha_ = true;
    if (codigo == '2' || codigo == '3') {
      out_.secoes.push_back({static_cast<uint32_t>(out_.linhas.size()), static_cast<uint8_t>(codigo - '0')});
    }
    if (codigo == 'C') {
      codigoPreformatado(inicio, fim, total);
    } else {
      prosa(inicio, fim, total);
    }
  }

 private:
  uint16_t disponivel(uint16_t total) const {
    const uint8_t r = primeiraLinha_ ? primeiro_ : seguinte_;
    return total > r + 4 ? static_cast<uint16_t>(total - r) : 4;
  }

  void emite(uint32_t inicio, uint32_t fim) {
    Linha l;
    l.inicio = inicio;
    l.tamanho = static_cast<uint16_t>(fim - inicio);
    l.bloco = codigo_;
    l.estilo = estilo_;
    l.recuo = primeiraLinha_ ? primeiro_ : seguinte_;
    l.marca = primeiraLinha_ ? MARCA_INICIO : 0;
    l.altura = altura_;
    l.antes = primeiraLinha_ ? antes_ : 0;
    out_.linhas.push_back(l);
    estilo_ = estiloApos(estilo_, t_ + inicio, fim - inicio);
    primeiraLinha_ = false;
  }

  void prosa(uint32_t inicio, uint32_t fim, uint16_t total) {
    uint32_t linha = inicio;  // início da linha em montagem
    uint32_t ultimo = inicio; // fim da última palavra que coube
    uint16_t col = 0;
    uint32_t i = inicio;
    bool vazia = true;
    while (i < fim) {
      while (i < fim && t_[i] == ' ') ++i;
      if (i >= fim) break;
      uint32_t j = i;
      while (j < fim && t_[j] != ' ') ++j;
      if (vazia) linha = i;
      uint32_t palavra = i;
      for (;;) {
        const uint16_t w = colunas(t_ + palavra, j - palavra);
        const uint16_t limite = disponivel(total);
        const uint16_t preciso = static_cast<uint16_t>((vazia ? 0 : 1) + w);
        if (col + preciso <= limite) {
          col = static_cast<uint16_t>(col + preciso);
          ultimo = j;
          vazia = false;
          break;
        }
        if (!vazia) {  // a palavra vai para a próxima linha
          emite(linha, ultimo);
          linha = palavra;
          col = 0;
          vazia = true;
          continue;
        }
        // Palavra maior que a linha: corta no limite.
        const uint32_t c = corte(t_, palavra, j, limite);
        emite(linha, c);
        palavra = c;
        linha = c;
        col = 0;
        vazia = true;
        if (palavra >= j) break;
      }
      i = j;
    }
    if (!vazia || primeiraLinha_) emite(vazia ? inicio : linha, vazia ? inicio : ultimo);
  }

  void codigoPreformatado(uint32_t inicio, uint32_t fim, uint16_t total) {
    if (inicio >= fim) {
      emite(inicio, inicio);
      return;
    }
    uint32_t i = inicio;
    while (i < fim) {
      const uint32_t c = corte(t_, i, fim, disponivel(total));
      emite(i, c);
      i = c;
    }
  }

  const char* t_;
  const Medidas& m_;
  Diagramacao& out_;
  char codigo_ = 'P';
  uint8_t primeiro_ = 0, seguinte_ = 0, altura_ = 22, antes_ = 0, estilo_ = 0;
  bool primeiraLinha_ = true;
};

bool titulo(char bloco) { return bloco >= '1' && bloco <= '4'; }

void paginar(const Medidas& m, Diagramacao& d) {
  d.paginas.clear();
  d.paginas.push_back(0);
  uint32_t y = 0;
  const uint32_t n = static_cast<uint32_t>(d.linhas.size());
  for (uint32_t i = 0; i < n; ++i) {
    const Linha& l = d.linhas[i];
    uint32_t h = l.altura + (y > 0 ? l.antes : 0);
    uint32_t precisa = h;
    if (titulo(l.bloco) && (l.marca & MARCA_INICIO)) {
      // O título não fica sozinho no pé da página: vai junto do resto dele, dos
      // títulos logo abaixo (seção e subseção) e da primeira linha do texto.
      uint32_t k = i + 1;
      while (k < n && titulo(d.linhas[k].bloco)) {
        precisa += d.linhas[k].altura + d.linhas[k].antes;
        ++k;
      }
      if (k < n) precisa += d.linhas[k].altura + d.linhas[k].antes;
    }
    if (y > 0 && y + precisa > m.altura) {
      d.paginas.push_back(i);
      y = 0;
      h = l.altura;
    }
    y += h;
  }
}

}  // namespace

bool blocoGrande(char bloco) { return bloco == '1' || bloco == '2'; }

uint16_t colunas(const char* s, uint32_t n) {
  uint16_t col = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    if (!controle(b) && inicioDeCaractere(b)) ++col;
  }
  return col;
}

uint8_t estiloApos(uint8_t estilo, const char* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    if (s[i] == NEGRITO) estilo ^= EST_NEGRITO;
    else if (s[i] == CODIGO) estilo ^= EST_CODIGO;
    else if (s[i] == ITALICO) estilo ^= EST_ITALICO;
  }
  return estilo;
}

uint32_t Diagramacao::paginaDaLinha(uint32_t linha) const {
  uint32_t lo = 0, hi = static_cast<uint32_t>(paginas.size());
  while (hi - lo > 1) {
    const uint32_t mid = (lo + hi) / 2;
    if (paginas[mid] <= linha) lo = mid;
    else hi = mid;
  }
  return lo;
}

uint32_t Diagramacao::primeiraLinha(uint32_t pagina) const {
  return pagina < paginas.size() ? paginas[pagina] : static_cast<uint32_t>(linhas.size());
}

uint32_t Diagramacao::fimDaPagina(uint32_t pagina) const {
  return pagina + 1 < paginas.size() ? paginas[pagina + 1] : static_cast<uint32_t>(linhas.size());
}

void diagramar(const char* texto, uint32_t tamanho, const Medidas& m, Diagramacao& out) {
  out.linhas.clear();
  out.secoes.clear();
  Diagramador d(texto, m, out);
  char anterior = 0;
  uint32_t i = 0;
  while (i < tamanho) {
    uint32_t fim = i;
    while (fim < tamanho && texto[fim] != '\n') ++fim;
    if (fim > i) {
      const char codigo = texto[i];
      d.bloco(codigo, i + 1, fim, anterior);
      anterior = codigo;
    }
    i = fim + 1;
  }
  paginar(m, out);
}

}  // namespace leitor
