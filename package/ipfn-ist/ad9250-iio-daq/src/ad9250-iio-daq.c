/*
 * iio - AD9250 IIO streaming example
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
 *
 **/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iio.h>

/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
/*#define GHZ(x) ((long long)(x*1000000000.0 + .5))*/

#define ASSERT(expr) { \
	if (!(expr)) { \
		(void) fprintf(stderr, "assertion failed (%s:%d)\n", __FILE__, __LINE__); \
		(void) abort(); \
	} \
}

/* RX is input, TX is output */
enum iodev { RX, TX };


/* IIO structs required for streaming */
static struct iio_context *ctx   = NULL;
static struct iio_device  *dev   = NULL;
static struct iio_channel *rx0_a = NULL;
static struct iio_channel *rx0_b = NULL;
static struct iio_channel *rx1_a = NULL;
static struct iio_channel *rx1_b = NULL;
static struct iio_buffer  *rxbuf0_a = NULL;
//static struct iio_buffer  *rxbuf0_b = NULL;
/*static struct iio_buffer  *txbuf = NULL;*/
short * pAdcData = NULL;

static bool stop;

/* cleanup and exit */
static void shutdown()
{
	printf("* Destroying buffers\n");
	if (rxbuf0_a) { iio_buffer_destroy(rxbuf0_a); }
//	if (rxbuf0_b) { iio_buffer_destroy(rxbuf0_b); }
	/*if (txbuf) { iio_buffer_destroy(txbuf); }*/
    if(pAdcData) free(pAdcData);
	printf("* Disabling streaming channels\n");
	if (rx0_a) { iio_channel_disable(rx0_a); }
	if (rx0_b) { iio_channel_disable(rx0_b); }
	if (rx1_a) { iio_channel_disable(rx1_a); }
	if (rx1_b) { iio_channel_disable(rx1_b); }

	printf("* Destroying context\n");
	if (ctx) { iio_context_destroy(ctx); }
	exit(0);
}

static void handle_sig(int sig)
{
	printf("Waiting for process to finish...\n");
	stop = true;
}
//http://www.wiki.xilinx.com/GPIO+User+Space+App
static void export_gpio(void ){
    int exportfd;

    printf("GPIO test running...\n");
    // The GPIO has to be exported to be able to see it
    // in sysfs
    exportfd = open("/sys/class/gpio/export", O_WRONLY);
    if (exportfd < 0)  {
        printf("Cannot open GPIO to export it\n");
        exit(1);
    }

    write(exportfd, "458", 4);
    close(exportfd);
}

/* simple configuration and streaming */
int main (int argc, char **argv)
{
	// Streaming devices
	//        struct iio_device *rx;

	// RX and TX sample counters
	//size_t nrx = 0;
	//	size_t ntx = 0;
	//	ssize_t nbytes_rx;//, nbytes_tx;
	int16_t *p_dat_a, *p_end_a, *p_dat_b, *p_end_b;
	ptrdiff_t p_inc_a;
    int16_t * pval16;
    int16_t trigLevel = 2000;
    unsigned int n_samples, saveSize;

    /*char fd_name[64];*/
	FILE * fd_data;

	export_gpio();
	/*sprintf(fd_name,"intData.bin");*/
	fd_data =  fopen("intData.bin","wb");

	// Listen to ctrl+c and ASSERT
	signal(SIGINT, handle_sig);

    printf("* Acquiring IIO context\n");
	ASSERT((ctx = iio_create_local_context()) && "No context");
	ASSERT(iio_context_get_devices_count(ctx) > 0 && "No devices");
	dev =  iio_context_find_device(ctx, "axi-ad9250-hpc-0");
	ASSERT(dev && "No axi-ad9250-hpc-0 device found");
	/* finds AD9361 streaming IIO channels */
	rx0_a = iio_device_find_channel(dev, "voltage0", 0); // RX
	ASSERT(rx0_a && "No axi-ad9250-hpc-0 channel found");
	iio_channel_enable(rx0_a);
	rx0_b = iio_device_find_channel(dev, "voltage1", 0); // RX
	ASSERT(rx0_b && "No axi-ad9250-hpc-1 channel found");
	iio_channel_enable(rx0_b);
    /*~0.5 ms buffers*/
    saveSize = 256*1024;
    rxbuf0_a = iio_device_create_buffer(dev, saveSize, false);
//    rxbuf0_b = iio_device_create_buffer(dev, 256*1024, false);
    pAdcData = (int16_t *) malloc(2*saveSize);
    do{
		iio_buffer_refill(rxbuf0_a);
//		iio_buffer_refill(rxbuf0_b);
		p_inc_a = iio_buffer_step(rxbuf0_a);
		p_end_a = iio_buffer_end(rxbuf0_a);
		p_dat_a = (int16_t *)iio_buffer_first(rxbuf0_a, rx0_a);
        pval16 = (int16_t *) (p_end_a - p_inc_a);
    }
    while(*pval16 < trigLevel);
    n_samples = (p_end_a -p_dat_a)/ p_inc_a;
    printf("Inc, %d, End %p, N:%d,  SS, %d\n", p_inc_a, p_end_a, n_samples, *pval16);
    for (int i=0; i<1; i++){
		iio_buffer_refill(rxbuf0_a);
		p_inc_a = iio_buffer_step(rxbuf0_a);
		p_end_a = iio_buffer_end(rxbuf0_a);
		p_dat_a = (int16_t *)iio_buffer_first(rxbuf0_a, rx0_a);
		printf("Inc, %d, End %p, N:%d,  SS, %d\n", p_inc_a, p_end_a,(p_dat_a -p_end_a)/p_inc_a, iio_device_get_sample_size(dev));
		fwrite(p_dat_a, 1, (p_end_a-p_dat_a), fd_data);
	}

    shutdown();
    fclose(fd_data);
	printf("Program Ended\n");

    return 0;
}


