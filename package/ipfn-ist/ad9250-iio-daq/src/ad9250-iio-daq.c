/*
 * iio - AD9250 IIO FMC ADC streaming
 * https://github.com/analogdevicesinc/libiio/blob/master/examples/ad9361-iiostream.c
 * Copyright (C) 2014 IABG mbH
 * Author: Michael Feilen <feilen_at_iabg.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * Max aquisition continuous time is ~ 20 ms, 5 blocks
 *
 **/

#include <stdbool.h>
//#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
//#include <sys/stat.h>
#include <unistd.h>

#include <gpiod.h>
#include <iio.h>

/* helper macros */
#define MHZ(x) ((long long)(x * 1000000.0 + .5))
/*#define GHZ(x) ((long long)(x*1000000000.0 + .5))*/

//#define GPIO_NUM_O_LINES 14
//#define GPIO_LINE_OFFSET 18
#define N_CHAN 2
#define N_BLOCKS                                                               \
  2 //  Number of RX buffers to save, 32ms, Max 16.5 good acquisition
#define GPIO_CHIP_NAME "/dev/gpiochip0"
#define GPIO_CONSUMER "gpiod-consumer"
#define GPIO_MAX_LINES 64
#define TRIG_EN_OFF 36
#define TRIG_REG_ADD_OFF 11
#define TRIG_REG_WRT_OFF 13
#define TRIG_REG_VAL_OFF 40

#define GPIO_BASE_ADDRESS 0x40000000
#define GPIO_0_DATA_OFFSET 0
#define GPIO_DIRECTION_OFFSET 4
#define GPIO_1_DATA_OFFSET 8

#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)
/*
line   9:      unnamed       unused  output  active-high Trigger active High
line  10:      unnamed       unused   input  active-high
line  11:      unnamed       unused  output  active-high Address Line
line  12:      unnamed       unused  output  active-high Address Line
line  13:      unnamed       unused  output  active-high
line  32:      unnamed "sysref-enable" output active-high [used]

line  36:      unnamed  Trigger enable bit
Lines 40-55 Trigger Value
*/
#define ASSERT(expr)                                                           \
  {                                                                            \
    if (!(expr)) {                                                             \
      (void)fprintf(stderr, "assertion failed (%s:%d)\n", __FILE__, __LINE__); \
      (void)abort();                                                           \
    }                                                                          \
  }

/* RX is input, TX is output */
enum iodev { RX, TX };

/* IIO structs required for streaming */
static struct iio_context *ctx = NULL;
static struct iio_device *dev0 = NULL;
static struct iio_device *dev1 = NULL;
static struct iio_channel *rx0_a = NULL;
static struct iio_channel *rx0_b = NULL;
static struct iio_channel *rx1_a = NULL;
static struct iio_channel *rx1_b = NULL;
static struct iio_buffer *rxbuf0 = NULL;
static struct iio_buffer *rxbuf1 = NULL;
/*static struct iio_buffer  *txbuf = NULL;*/

int memfd;
void *mapped_base, *mapped_dev_base;

static bool stop;
/* cleanup and exit */
static void shutdown_iio() {
  printf("* Destroying IIO buffers\n");
  if (rxbuf0) {
    iio_buffer_destroy(rxbuf0);
  }
  if (rxbuf1) {
    iio_buffer_destroy(rxbuf1);
  }
  /*if (txbuf) { iio_buffer_destroy(txbuf); }*/
  printf("* Disabling streaming channels\n");
  if (rx0_a) {
    iio_channel_disable(rx0_a);
  }
  if (rx0_b) {
    iio_channel_disable(rx0_b);
  }
  if (rx1_a) {
    iio_channel_disable(rx1_a);
  }
  if (rx1_b) {
    iio_channel_disable(rx1_b);
  }

  printf("* Destroying context\n");
  if (ctx) {
    iio_context_destroy(ctx);
  }
  // exit(0);
}

static void config_iio_acq() {
  int ctx_cnt;
  printf("* Acquiring IIO context\n");

  ASSERT((ctx = iio_create_local_context()) && "No context");
  ASSERT(ctx_cnt = iio_context_get_devices_count(ctx) > 0 && "No devices");
  dev0 = iio_context_find_device(ctx, "axi-ad9250-hpc-0");
  ASSERT(dev0 && "No axi-ad9250-hpc-0 device found");
  /* finds AD9250 streaming IIO channels */
  rx0_a = iio_device_find_channel(dev0, "voltage0", 0); // RX
  ASSERT(rx0_a && "No axi-ad9250-hpc-0 channel 0 found");
  rx0_b = iio_device_find_channel(dev0, "voltage1", 0);
  ASSERT(rx0_b && "No axi-ad9250-hpc-0 channel 1 found");

  iio_channel_enable(rx0_a);
  iio_channel_enable(rx0_b);

  /*printf("ctx_cnt=%d\n", ctx_cnt);*/
  dev1 = iio_context_find_device(ctx, "axi-ad9250-hpc-1");
  ASSERT(dev1 && "No axi-ad9250-hpc-1 device found");
  rx1_a = iio_device_find_channel(dev1, "voltage0", 0); // RX
  ASSERT(rx1_a && "No axi-ad9250-hpc-1 channel 0 found");
  // iio_channel_enable(rx1_a);
  rx1_b = iio_device_find_channel(dev1, "voltage1", 0); // RX
  ASSERT(rx1_b && "No axi-ad9250-hpc-1 channel 1 found");
  // iio_channel_enable(rx1_b);
}

static void handle_sig(int sig) {
  printf("Waiting for process to finish...\n");
  stop = true;
}

/* Since linux 4.8 the GPIO sysfs interface is deprecated. User space should use
   the character device instead. This library encapsulates the ioctl calls and
   data structures behind a straightforward API.
   */

int set_multiple_gpio(unsigned int offset, unsigned int width, int value) {
  int rv;
  unsigned int gpio_offsets[32];
  int gpio_values[32];
  if ((offset + width) > GPIO_MAX_LINES)
    return -1;
  for (int i = 0; i < width; i++) {
    gpio_offsets[i] = offset + i;
    gpio_values[i] = ((value >> i) & 0x1);
  }
  rv = gpiod_ctxless_set_value_multiple(GPIO_CHIP_NAME, gpio_offsets,
                                        gpio_values, width, false,
                                        GPIO_CONSUMER, NULL, NULL);
  return rv;
}
int get_multiple_gpio(unsigned int offset, unsigned int width, int *value) {
  int rv;
  int data = 0;
  unsigned int gpio_offsets[32];
  int gpio_values[32];
  if ((offset + width) > GPIO_MAX_LINES)
    return -1;
  for (int i = 0; i < width; i++)
    gpio_offsets[i] = offset + i;

  rv = gpiod_ctxless_get_value_multiple(
      GPIO_CHIP_NAME, gpio_offsets, gpio_values, width, false, GPIO_CONSUMER);
  for (int i = 0; i < width; i++)
    data |= ((gpio_values[i]) & 0x1) << i;
  *value = data;
  return rv;
}
int write_trigger_reg(unsigned int reg, int value) {
  int rv;
  rv = set_multiple_gpio(TRIG_REG_ADD_OFF, 2, reg);
  rv = set_multiple_gpio(TRIG_REG_VAL_OFF, 16, value); // Lines 40-55
  rv = gpiod_ctxless_set_value(GPIO_CHIP_NAME, TRIG_REG_WRT_OFF, 1, false,
                               GPIO_CONSUMER, NULL, NULL);
  rv = gpiod_ctxless_set_value(GPIO_CHIP_NAME, TRIG_REG_WRT_OFF, 0, false,
                               GPIO_CONSUMER, NULL, NULL);
  rv = gpiod_ctxless_set_value(GPIO_CHIP_NAME, 12, 1, false, GPIO_CONSUMER,
                               NULL, NULL);
  return rv;
}

int mmap_gpio_mem() {
  off_t dev_base = GPIO_BASE_ADDRESS;
  memfd = open("/dev/mem", O_RDWR | O_SYNC);
  if (memfd == -1) {
    printf("Can't open /dev/mem.\n");
    return -1;
  }
  printf("/dev/mem opened.\n");
  // Map one page of memory into user space such that the device is in that
  // page, but it may not be at the start of the page

  mapped_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd,
                     dev_base & ~MAP_MASK);
  if (mapped_base == (void *)-1) {
    printf("Can't map the memory to user space.\n");
    return -1;
  }
  printf("GPIO Memory mapped at address %p.\n", mapped_base);
  // get the address of the device in user space which will be an offset from
  // the base that was mapped as memory is mapped at the start of a page

  mapped_dev_base = mapped_base + (dev_base & MAP_MASK);
  return 0;
}
void mmap_gpio_write32(unsigned int uval, unsigned int reg_offset) {
  *((unsigned int *)(mapped_dev_base + reg_offset)) = uval;
}

/* Main Board configuration and streaming */
int main(int argc, char **argv) {

  char *p_dat_a, *p_end; //, *p_dat_b;
  char *pAdcData = NULL;
  char *pAdcData1 = NULL;
  ptrdiff_t p_inc;
  /*int16_t *pval16;*/
  unsigned int n_samples, bufSamples, savBytes;
  unsigned int bufSize; // =128*4096;

  FILE *fd_data;
  FILE *fd_data1 = NULL;
  int rv;
  int trigger_value = 4000; // 0x0025;
  int delay_val = 0;
  unsigned long ulval;

  // Listen to ctrl+c and ASSERT
  signal(SIGINT, handle_sig);

  mmap_gpio_mem();

  // Clear Write reg enable
  rv = gpiod_ctxless_set_value(GPIO_CHIP_NAME, TRIG_REG_WRT_OFF, 0, false,
                               GPIO_CONSUMER, NULL, NULL);
  rv = write_trigger_reg(1, trigger_value);
  rv = write_trigger_reg(3, trigger_value);
  trigger_value = -6000; // 0x0025;
  rv = write_trigger_reg(2, trigger_value);
  /*sprintf(fd_name,"intData.bin");*/
  fd_data = fopen("intData.bin", "wb");
  /*fd_data1 = fopen("intData34.bin", "wb");*/

  /* ~4 ms buffers*/
  bufSamples = 1024 * 1024; // number of samples per buff RX IIO
  bufSize = N_CHAN * bufSamples * sizeof(int16_t);
  savBytes = N_BLOCKS * bufSize; // sizeof(int16_t) * savBlock ;
  pAdcData = (char *)malloc(savBytes);
  if (!pAdcData) {
    perror("Could not create pAdcData buffer");
    shutdown_iio();
  }
  if (fd_data1) {
    pAdcData1 = (char *)malloc(savBytes);
    if (!pAdcData1) {
      perror("Could not create pAdcData1 buffer");
      shutdown_iio();
    }
  }
  config_iio_acq();
  ulval = 0x11;
  mmap_gpio_write32(0x11, GPIO_1_DATA_OFFSET);

  /**((unsigned long *)(mapped_dev_base + GPIO_1_DATA_OFFSET)) = ulval;*/
  /* Starts acquition channels 0,1*/
  /*
   rxbuf1 = iio_device_create_buffer(dev1, bufSamples, false);
  if (!rxbuf1) {
    perror("Could not create RX buffer 1");
    shutdown_iio();
  }
  */
  rxbuf0 = iio_device_create_buffer(dev0, bufSamples, false);
  if (!rxbuf0) {
    perror("Could not create RX buffer");
    shutdown_iio();
  }
  /*~ 26 ms interval*/
  /*mmap_gpio_write32(0x01, GPIO_1_DATA_OFFSET);*/
  /* arm Trigger (also first blink LED. delay ~8 ms */
  /* Wavetek 395 Model: Pulse mode, 1ms period, 800 ns /800 ns Leading/Trail,
   * 10 count */
  // for faster IO see
  // https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842018/Linux+User+Mode+Pseudo+Driver
  /*rv = gpiod_ctxless_get_value(GPIO_CHIP_NAME, TRIG_EN_OFF, false,*/
  /*GPIO_CONSUMER);*/
  /*rv = gpiod_ctxless_set_value(GPIO_CHIP_NAME, TRIG_EN_OFF, 1, false,*/
  /*GPIO_CONSUMER, NULL, NULL);*/
  /*
   *for (int i = 0; i < 1; i++) { // max 16 ?
   *  iio_buffer_refill(rxbuf0);
   *  p_inc = iio_buffer_step(rxbuf0);
   *  p_end = iio_buffer_end(rxbuf0);
   *  p_dat_a = (char *)iio_buffer_first(rxbuf0, rx0_a);
   *  p_dat_b = (char *)iio_buffer_first(rxbuf0, rx0_b);
   *  pval16 = (int16_t *)(p_end - p_inc);
   *  if (*pval16 > 40) {
   *    printf("got trigger!\n");
   *    printf("p_dat, %p, %p, End %p,  %d\n", p_dat_a, p_dat_b, p_end,
   **pval16);
   *    break;
   *  }
   *  //    usleep(10);
   *}
   *  while(*pval16 < trigLevel);
   * memcpy(pAdcData, p_dat_a, (p_end - p_dat_a));
   *memcpy(pAdcData, p_dat_a, bufSize);
   */
  /*for (int i = 0; i < 2; i++) {*/
  /*memcpy(pAdcData + i * savBlock, p_dat_a + i * savBlock, savBlock);*/
  /*}*/
  for (int i = 0; i < N_BLOCKS; i++) {
    rv = iio_buffer_refill(rxbuf0);
    if (rv != bufSize)
      printf("Refill size: %d\n", rv);
    /*iio_buffer_refill(rxbuf1);*/
    p_inc = iio_buffer_step(rxbuf0);
    p_end = iio_buffer_end(rxbuf0);
    p_dat_a = (char *)iio_buffer_first(rxbuf0, rx0_a);
    /*p_dat_b = (char *)iio_buffer_first(rxbuf0, rx0_b);*/
    /*p_dat_b = (char *)iio_buffer_first(rxbuf1, rx1_a);*/
    /*pval16 = (int16_t *)(p_end - p_inc);*/
    memcpy(pAdcData + bufSize * i, p_dat_a, bufSize);
    /*memcpy(pAdcData1 + bufSize * i, p_dat_b, bufSize);*/
  }
  /*usleep(10);*/
  n_samples = (p_end - p_dat_a) / p_inc;
  printf("Inc, %d, End %p, NS:%d,  BS, %d, rv:%d\n", p_inc, p_end, n_samples,
         bufSamples, rv);
  // printf("p_dat, %p, %p, End %p, N:%d, LS, %d\n", p_dat_a, p_dat_b, p_end,
  //       n_samples, *pval16);
  //       Inc, 4, End 0x3345a000, NS:1048576,  BS, 1048576
  // p_dat, 0x3305a000, (nil), End 0x3345a000, N:1048576, LS, -16
  // Inc, 4, End 0x3345a000, N:-1048576,  SS, 4

  printf("Inc, %d, End %p, N:%d,  SS, %d\n", p_inc, p_end,
         (p_dat_a - p_end) / p_inc, iio_device_get_sample_size(dev0));

  shutdown_iio();
  rv = get_multiple_gpio(TRIG_REG_VAL_OFF, 16, &delay_val); // Lines 40-55
  if (rv) {
    printf("Error get_multiple_gpio: %d\n", rv);
    return -1;
  }
  printf("* get_multiple_gpio delay_val: %d, %.3f us\n", delay_val,
         delay_val / 5.0 * 8e-3);
  // turns OFF LED, reset trigger machine
  rv = gpiod_ctxless_set_value(GPIO_CHIP_NAME, TRIG_EN_OFF, 0, false,
                               GPIO_CONSUMER, NULL, NULL);
  /*Fast reading of GPIO register*/
  ulval = *((unsigned long *)(mapped_dev_base + GPIO_0_DATA_OFFSET));
  printf("gpio 0 val 0x%lX,", ulval);
  ulval = *((unsigned long *)(mapped_dev_base + GPIO_1_DATA_OFFSET));
  printf("gpio 1 val 0x%lX \n", ulval);
  printf("* Saving Data to Files\n");
  fwrite(pAdcData, N_BLOCKS, bufSize, fd_data);
  /*fwrite(pAdcData1, N_BLOCKS, bufSize, fd_data1);*/
  if (pAdcData)
    free(pAdcData);
  if (pAdcData1)
    free(pAdcData1);
  fclose(fd_data);
  if (fd_data1)
    fclose(fd_data1);
  if (munmap(mapped_base, MAP_SIZE) == -1) {
    printf("Can't unmap memory from user space.\n");
    exit(0);
  }

  close(memfd);
  printf("Program Ended\n");
  return 0;
}
