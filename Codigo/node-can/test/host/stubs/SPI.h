#ifndef HOST_SPI_H
#define HOST_SPI_H

class SPIClass {
 public:
  void begin(int, int, int, int) {}
};
extern SPIClass SPI;

#endif
