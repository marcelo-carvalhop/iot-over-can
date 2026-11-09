/*
 * Diagramação do leitor: quebra de linha, paginação e índice de seções.
 *
 * Não depende de placa: roda igual no firmware e no teste do computador. O
 * texto vem de documentos.h (formato descrito em tools/documentos.py): uma
 * linha por bloco, com o código do bloco no primeiro byte e bytes de controle
 * para negrito, código e itálico. A fonte da tela (efont) tem largura fixa
 * para o alfabeto latino, então a largura de um trecho é o número de
 * caracteres vezes a largura de um caractere.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace leitor {

constexpr char NEGRITO = '\x01';
constexpr char CODIGO = '\x02';
constexpr char ITALICO = '\x03';

enum : uint8_t { EST_NEGRITO = 1, EST_CODIGO = 2, EST_ITALICO = 4 };

/* Área de texto e largura de um caractere nas duas fontes. */
struct Medidas {
  uint16_t largura = 376;
  uint16_t altura = 520;
  uint8_t celCorpo = 8;    // fonte de 16 px
  uint8_t celGrande = 12;  // fonte de 24 px (título e seções)
};

enum : uint8_t { MARCA_INICIO = 1 };  // primeira linha de um bloco

struct Linha {
  uint32_t inicio;   // primeiro byte no texto do documento
  uint16_t tamanho;  // bytes da linha
  char bloco;        // código do bloco ('P', '2', 'C', ...)
  uint8_t estilo;    // estilos em linha ativos no início da linha
  uint8_t recuo;     // caracteres antes do texto
  uint8_t marca;     // MARCA_INICIO na primeira linha do bloco
  uint8_t altura;    // altura da linha, em pixels
  uint8_t antes;     // espaço antes da linha, se ela não abrir a página
};

struct Secao {
  uint32_t linha;  // primeira linha do título
  uint8_t nivel;   // 2 seção, 3 subseção
};

struct Diagramacao {
  std::vector<Linha> linhas;
  std::vector<uint32_t> paginas;  // primeira linha de cada página
  std::vector<Secao> secoes;

  uint32_t paginaDaLinha(uint32_t linha) const;
  uint32_t totalPaginas() const { return paginas.empty() ? 1 : static_cast<uint32_t>(paginas.size()); }
  /* Linhas da página: [primeira, fim). */
  uint32_t primeiraLinha(uint32_t pagina) const;
  uint32_t fimDaPagina(uint32_t pagina) const;
};

bool blocoGrande(char bloco);  // título e seções usam a fonte de 24 px
uint16_t colunas(const char* s, uint32_t n);
uint8_t estiloApos(uint8_t estilo, const char* s, uint32_t n);

void diagramar(const char* texto, uint32_t tamanho, const Medidas& m, Diagramacao& out);

}  // namespace leitor
