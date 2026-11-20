/**
 * @file ioc_version.h
 * @brief Versão e commit do firmware, para o boot e para o comando VERSION.
 *
 * Os dois valores vêm do build: IOT_FW_VERSION é o conteúdo do arquivo VERSION
 * da raiz do repositório (a única fonte da versão do projeto) e IOT_FW_COMMIT é
 * o commit do git, com o sufixo "-dirty" se havia alterações não commitadas.
 * Os scripts de gravação, o PlatformIO (Codigo/scripts/pio_versao.py), o CMake
 * dos sensores (Codigo/common/ioc_version.cmake) e a bancada virtual os definem.
 *
 * Compilado por fora desses caminhos (a IDE do Arduino, por exemplo), o
 * firmware se identifica como "0.0.0-dev" e commit "unknown": a diferença
 * aparece no boot e o script de fumaça (smoke_boot.py) a recusa.
 */
#ifndef IOC_VERSION_H
#define IOC_VERSION_H

#ifndef IOT_FW_VERSION
#define IOT_FW_VERSION "0.0.0-dev"
#endif

#ifndef IOT_FW_COMMIT
#define IOT_FW_COMMIT "unknown"
#endif

#endif /* IOC_VERSION_H */
