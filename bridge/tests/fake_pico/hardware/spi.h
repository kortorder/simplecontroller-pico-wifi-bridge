#pragma once
#include <cstdint>
struct spi_inst_t {};
struct spi_hw_t {std::uint32_t dr=0;};
inline spi_inst_t fake_spi;
inline spi_inst_t* spi0=&fake_spi;
inline spi_hw_t fake_spi_hw;
constexpr unsigned SPI_CPOL_0=0,SPI_CPHA_0=0,SPI_MSB_FIRST=0;
inline void spi_init(spi_inst_t*,unsigned) {}
inline void spi_set_format(spi_inst_t*,unsigned,unsigned,unsigned,unsigned) {}
inline bool spi_is_writable(spi_inst_t*) {return true;}
inline bool spi_is_readable(spi_inst_t*) {return true;}
inline bool spi_is_busy(spi_inst_t*) {return false;}
inline spi_hw_t* spi_get_hw(spi_inst_t*) {return &fake_spi_hw;}
