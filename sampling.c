#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <iio.h>
#include <ad9361.h>
#include <fftw3.h>
#include "sampling.h"

#define MODES_DEFAULT_RATE         2000000
#define MODES_DEFAULT_FREQ         1090000000
#define MODES_DATA_LEN             (16*16384)   /* 256k */
#define MODES_AUTO_GAIN            -100         /* Use automatic gain. */
#define MODES_MAX_GAIN             70       /* Use max available gain. */


// TODO: fix so that this is just the sample size, nothing to do with the length of a message?
// unlesss we want to decode?
#define MODES_PREAMBLE_US 8       /* microseconds */
#define MODES_LONG_MSG_BITS 112
#define MODES_SHORT_MSG_BITS 56
#define MODES_FULL_LEN (MODES_PREAMBLE_US+MODES_LONG_MSG_BITS)
#define MODES_LONG_MSG_BYTES (112/8)
#define MODES_SHORT_MSG_BYTES (56/8)

#define MODES_NOTUSED(V) ((void) V)

/* Program global state. */
struct {
	/* Internal state */
	pthread_t reader_thread;
	pthread_mutex_t data_mutex;     /* Mutex to synchronize buffer access. */
	pthread_cond_t data_cond;       /* Conditional variable associated. */
	unsigned char *data;            /* Raw IQ samples buffer */
	uint32_t data_len;              /* Buffer length. */
	int fd;                         /* --ifile option file descriptor. */
	int data_ready;                 /* Data ready to be processed. */
	int exit;                       /* Exit from the main loop when true. */

	/* PLUTOSDR */
	int dev_index;
	int gain;
	int enable_agc;
	struct iio_context *ctx;
	struct iio_device *dev;
	long long freq;
	struct iio_channel *rx0_i;
	struct iio_channel *rx0_q;
	struct iio_buffer  *rxbuf;
	int stop;

	/*fftw */
	fftw_complex *in_c, *out;
    fftw_plan plan_forward;
    double *win;

} Modes;

/* =============================== Initialization =========================== */

void modesInitConfig(void) {
	Modes.gain = MODES_AUTO_GAIN;
	Modes.dev_index = 0;
	Modes.enable_agc = 1;
	Modes.freq = MODES_DEFAULT_FREQ;
}

void modesInit(void) {
	int i, q;

	pthread_mutex_init(&Modes.data_mutex, NULL);
	pthread_cond_init(&Modes.data_cond, NULL);
	/* We add a full message minus a final bit to the length, so that we
	 * can carry the remaining part of the buffer that we can't process
	 * in the message detection loop, back at the start of the next data
	 * to process. This way we are able to also detect messages crossing
	 * two reads. */
	Modes.data_len = MODES_DATA_LEN + (MODES_FULL_LEN - 1) * 4;
	Modes.data_ready = 0;
	if ((Modes.data = malloc(Modes.data_len)) == NULL) {
		fprintf(stderr, "Out of memory allocating data buffer.\n");
		exit(1);
	}
	memset(Modes.data, 127, Modes.data_len);

	Modes.exit = 0;
}

/* =============================== PlutoSDR handling ========================== */

void modesInitPLUTOSDR(void) {
	int device_count;

	printf("* Acquiring IIO context\n");
	Modes.ctx = iio_create_default_context();
	if(Modes.ctx == NULL){
		Modes.ctx = iio_create_network_context("pluto.local");
	}
	device_count = iio_context_get_devices_count(Modes.ctx);
	if (!device_count) {
		fprintf(stderr, "No supported PLUTOSDR devices found.\n");
		exit(1);
	}

	fprintf(stderr, "Found %d device(s):\n", device_count);

	printf("* Acquiring AD9361 streaming devices\n");

	Modes.dev = iio_context_find_device(Modes.ctx, "cf-ad9361-lpc");

	if (Modes.dev == NULL) {
		fprintf(stderr, "Error opening the PLUTOSDR device: %s\n",
				strerror(errno));
		exit(1);
	}

	printf("* Acquiring AD9361 phy channel 0\n");

	struct iio_channel* phy_chn = iio_device_find_channel(iio_context_find_device(Modes.ctx, "ad9361-phy"), "voltage0", false);

	iio_channel_attr_write(phy_chn, "rf_port_select", "A_BALANCED");
	iio_channel_attr_write_longlong(phy_chn, "rf_bandwidth", MODES_DEFAULT_RATE);
	iio_channel_attr_write_longlong(phy_chn, "sampling_frequency", MODES_DEFAULT_RATE);

	struct iio_channel* lo_chn = iio_device_find_channel(iio_context_find_device(Modes.ctx, "ad9361-phy"), "altvoltage0", true);
	iio_channel_attr_write_longlong(lo_chn, "frequency", Modes.freq);

	printf("* Initializing AD9361 IIO streaming channels\n");

	Modes.rx0_i = iio_device_find_channel(Modes.dev, "voltage0", false);
	if (!Modes.rx0_i)
		Modes.rx0_i= iio_device_find_channel(Modes.dev, "altvoltage0", false);

	Modes.rx0_q = iio_device_find_channel(Modes.dev, "voltage1", false);
	if (!Modes.rx0_q)
		Modes.rx0_q = iio_device_find_channel(Modes.dev, "altvoltage1", false);

	ad9361_set_bb_rate(iio_context_find_device(Modes.ctx, "ad9361-phy"), MODES_DEFAULT_RATE);

	printf("* Enabling IIO streaming channels\n");

	iio_channel_enable(Modes.rx0_i);
	iio_channel_enable(Modes.rx0_q);

	printf("* Creating non-cyclic IIO buffers \n");

	Modes.rxbuf=iio_device_create_buffer(Modes.dev, MODES_DATA_LEN/2, false);

	if (!Modes.rxbuf) {
		perror("Could not create RX buffer");
	}

	if(Modes.gain==MODES_AUTO_GAIN){
		iio_channel_attr_write(phy_chn,"gain_control_mode","slow_attack");
	}else{
		if(Modes.gain>MODES_MAX_GAIN)
			Modes.gain=MODES_MAX_GAIN;
		iio_channel_attr_write(phy_chn,"gain_control_mode","manual");
		iio_channel_attr_write_longlong(phy_chn, "hardwaregain", Modes.gain);
	}


}

/* Use a thread reading data in background, while the main thread
 * handles decoding and visualization of data to the user.
 *
 * The reading thread calls the RTLSDR API to read data asynchronously, and
 * uses a callback to populate the data buffer.
 * A Mutex is used to avoid races with the decoding thread. */
void plutosdrCallback(unsigned char *buf, uint32_t len){
	pthread_mutex_lock(&Modes.data_mutex);
	uint32_t i;
	for (i = 0; i < len; i++){
		buf[i]^= (uint8_t)0x80;
	}
	if (len > MODES_DATA_LEN) len = MODES_DATA_LEN;
	/* Move the last part of the previous buffer, that was not processed,
	 * on the start of the new buffer. */
	memcpy(Modes.data, Modes.data+MODES_DATA_LEN, (MODES_FULL_LEN-1)*4);
	/* Read the new data. */
	memcpy(Modes.data+(MODES_FULL_LEN-1)*4, buf, len);
	Modes.data_ready = 1;
	/* Signal to the other thread that new data is ready */
	pthread_cond_signal(&Modes.data_cond);
	pthread_mutex_unlock(&Modes.data_mutex);

}

/* We read data using a thread, so the main thread only handles decoding
 * without caring about data acquisition. */
void *readerThreadEntryPoint(void *arg) {

	MODES_NOTUSED(arg);

	unsigned char cb_buf[MODES_DATA_LEN];

	while(!Modes.stop){
		void *p_dat, *p_end;
		ptrdiff_t p_inc;
		int j=0;
		iio_buffer_refill(Modes.rxbuf);
		p_inc = iio_buffer_step(Modes.rxbuf);
		p_end = iio_buffer_end(Modes.rxbuf);
		p_dat = iio_buffer_first(Modes.rxbuf, Modes.rx0_i);
		for(p_dat = iio_buffer_first(Modes.rxbuf, Modes.rx0_i);p_dat < p_end; p_dat += p_inc){
			const int16_t i = ((int16_t*)p_dat)[0]; // Real (I)
			const int16_t q = ((int16_t*)p_dat)[1]; // Imag (Q)
			https://wiki.analog.com/resources/eval/user-guides/ad-fmcomms2-ebz/software/basic_iq_datafiles#binary_format
			cb_buf[j*2]=i>>4;
			cb_buf[j*2+1]=q>>4;
			j++;

		}
		plutosdrCallback(cb_buf,MODES_DATA_LEN);	
	}
	return NULL;
}


/* ================================ Main ==================================== */

void showHelp(void) {
	printf(
            // TODO: eventuall this will be helpfull inforamtion, but for now it is just this
			"welcome to an unhelpful help page\n"
			);
}

int main(int argc, char **argv) {
	int j;
	/* Set sane defaults. */
	modesInitConfig();

	/* Parse the command line options */
	for (j = 1; j < argc; j++) {

		if (!strcmp(argv[j], "--help")) {
			showHelp();
			exit(0);
		}
		else {
			fprintf(stderr,
					"Unknown or not enough arguments for option '%s'.\n\n",
					argv[j]);
			showHelp();
			exit(1);
		}
	}

	/* Initialization */
	modesInit();
	modesInitPLUTOSDR();

	/* Create the thread that will read the data from the device. */
	pthread_create(&Modes.reader_thread, NULL, readerThreadEntryPoint, NULL);

	pthread_mutex_lock(&Modes.data_mutex);
	while (1) {
		if (!Modes.data_ready) {
			pthread_cond_wait(&Modes.data_cond, &Modes.data_mutex);
			continue;
		}
		// TODO: get the samples from Modes.data_len;
		// and just SQRT(I^2 + q^2)
		//computeMagnitudeVector();
		printf("we have samples! i think");
		unsigned char *p = Modes.data;
		int sample = 0;
		uint32_t j;
		for (j = 0; j < Modes.data_len; j += 2) {
			int i = p[j] - 127;
			int q = p[j + 1] - 127;
			printf("sample %d:: i = %d, q = %d\n",sample, i, q);
			sample++;
		}
		printf("end of sample");

		/* Signal to the other thread that we processed the available data
		 * and we want more (useful for --ifile). */
		Modes.data_ready = 0;
		pthread_cond_signal(&Modes.data_cond);

		/* Process data after releasing the lock, so that the capturing
		 * thread can read data while we perform computationally expensive
		 * stuff * at the same time. (This should only be useful with very
		 * slow processors). */
		pthread_mutex_unlock(&Modes.data_mutex);
		// TODO: get rid of this bullshit and put my cool stuff in it 8)
		// detectModeS(Modes.magnitude, Modes.data_len / 2);
		// backgroundTasks();
		pthread_mutex_lock(&Modes.data_mutex);
		if (Modes.exit) break;
	}
	printf("* Destroying buffers\n");
	if (Modes.rxbuf) { iio_buffer_destroy(Modes.rxbuf); }

	printf("* Disabling streaming channels\n");
	if (Modes.rx0_i) { iio_channel_disable(Modes.rx0_i); }
	if (Modes.rx0_q) { iio_channel_disable(Modes.rx0_q); }

	printf("* Destroying context\n");
	if (Modes.ctx) { iio_context_destroy(Modes.ctx); }

	return 0;
}