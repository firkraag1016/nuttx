/****************************************************************************
 * include/nuttx/mtd/song_spi_flash.h
 *
 *   Copyright (C) 2019 FishSemi Inc. All rights reserved.
 *   Author: zhuyanlin<zhuyanlin@fishsemi.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_MTD_SONG_SPI_FLASH_H
#define __INCLUDE_NUTTX_MTD_SONG_SPI_FLASH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/mtd/mtd.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* This structure describes the configuration of song platform spi flash
 * controller and flash IP parameters.
 */

struct song_spi_flash_config_s
{
  uint32_t base;            /* base address of the flash controller */
  uint32_t cpu_base;        /* cpu interface flash read base address */
  FAR const char *mclk;     /* flash controller clk name for operate on it */
  uint32_t rate;            /* if not zero: flash controller clk rate */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: song_spi_flash_initialize
 *
 * Description:
 *  Create and initialize a song spi flash controller MTD device instance
 *  that can be used to access the spi program memory.
 *
 ****************************************************************************/

FAR struct mtd_dev_s *song_spi_flash_initialize(
                          FAR const struct song_spi_flash_config_s *cfg);

#endif /* __INCLUDE_NUTTX_MTD_SONG_SPI_FLASH_H */
