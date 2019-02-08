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
#include <complex.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <iio.h>
#include <ad9361.h>
#include <fftw3.h>
#include "sampling.h"

#define MODES_DEFAULT_RATE         70000
#define MODES_DEFAULT_FREQ         908000000
#define FFT_SIZE 		   		   256
#define MODES_DATA_LEN             (16*16384)   /* 256k */
#define MODES_AUTO_GAIN            -100         /* Use automatic gain. */
#define MODES_MAX_GAIN             70       /* Use max available gain. */

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

double win_hanning(int j, int n) {
    double a = 2.0 * M_PI / (n - 1), w;
    w = 0.5 * (1.0 - cos(a * j));
    return (w);
}

void modesInitConfig(void) {
	Modes.gain = MODES_AUTO_GAIN;
	Modes.dev_index = 0;
	Modes.enable_agc = 1;
	Modes.freq = MODES_DEFAULT_FREQ;
}

void modesInit(void) {

	pthread_mutex_init(&Modes.data_mutex, NULL);
	pthread_cond_init(&Modes.data_cond, NULL);
	/* We add a full message minus a final bit to the length, so that we
	 * can carry the remaining part of the buffer that we can't process
	 * in the message detection loop, back at the start of the next data
	 * to process. This way we are able to also detect messages crossing
	 * two reads. */

	Modes.data_len = MODES_DATA_LEN + sizeof(fftw_complex)*FFT_SIZE;
	Modes.data_ready = 0;
	if ((Modes.data = malloc(Modes.data_len)) == NULL) {
		fprintf(stderr, "Out of memory allocating data buffer.\n");
		exit(1);
	}
	memset(Modes.data, 127, Modes.data_len);

	Modes.exit = 0;

	Modes.win = fftw_malloc(sizeof(double) * FFT_SIZE);
    Modes.in_c = fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    Modes.out = fftw_malloc(sizeof(fftw_complex) * (FFT_SIZE + 1));
    Modes.plan_forward = fftw_plan_dft_1d(FFT_SIZE, Modes.in_c, Modes.out, FFTW_FORWARD,
                                    FFTW_ESTIMATE);

	int i;
	for (i = 0; i < FFT_SIZE; i ++)
        Modes.win[i] = win_hanning(i, MODES_DATA_LEN);
}

/* =============================== PlutoSDR handling ========================== */

void modesInitPLUTOSDR(void) {
	int device_count;

	printf("* Acquiring IIO context\n");
	Modes.ctx = iio_create_default_context();
	if(Modes.ctx == NULL){
		Modes.ctx = iio_create_network_context("192.168.3.1");
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
 * reads data asynchronously, and
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
	memcpy(Modes.data, Modes.data+MODES_DATA_LEN, (FFT_SIZE-1)*32);
	/* Read the new data. */
	memcpy(Modes.data+(FFT_SIZE-1)*32, buf, len);
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
			// https://wiki.analog.com/resources/eval/user-guides/ad-fmcomms2-ebz/software/basic_iq_datafiles#binary_format
			cb_buf[j*2]=i;
			cb_buf[j*2+1]=q;
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
	double mag[5], peak[5];
	unsigned int k, cnt, bin[5];
	/* Set sane defaults. */
	modesInitConfig();

	/* Parse the command line options */
	int j;
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
		cnt = 0;
		uint32_t j;
		for (j = 0; j < FFT_SIZE; j += 1) {
			const int16_t real = ((int16_t*)p)[j]; // Real (I)
            const int16_t imag = ((int16_t*)p)[j+1]; // Imag (Q)
			printf("[%d] I = %d, Q = %d\n",j, real, imag);
			//Modes.in_c[cnt] = (real + I * imag) / 2048;
			Modes.in_c[cnt] = (real * Modes.win[cnt] + I * imag * Modes.win[cnt]) / 2048;
			cnt++;
		}

		fftw_execute(Modes.plan_forward);

		unsigned long long buffer_size_squared = (unsigned long long)FFT_SIZE *
                    (unsigned long long)FFT_SIZE;

		int i;
		memset(mag, 0, sizeof(mag));
		memset(bin, 0, sizeof(bin));
		memset(peak, 0, sizeof(peak));

		double average_power = 0;

		for (i = 1; i < FFT_SIZE; ++i) {
			mag[2] = mag[1];
			mag[1] = mag[0];
			mag[0] = 10 * log10((creal(Modes.out[i]) * creal(Modes.out[i]) + cimag(Modes.out[i]) * cimag(
										Modes.out[i])) / buffer_size_squared);
			
			printf("[%d] %f\n",i, mag[0]);

			if (i < 5 || i > 122 ){
				average_power = (mag[0]+average_power)/2;
			}

			if (i < 2)
				continue;
			for (j = 0; j <= 2; j++) {
				if  ((mag[1] > peak[j]) &&
						((!((mag[2] > mag[1]) && (mag[1] > mag[0]))) &&
						(!((mag[2] < mag[1]) && (mag[1] < mag[0]))))) {
					for (k = 2; k > j; k--) {
						peak[k] = peak[k - 1];
						bin[k] = bin[k - 1];
					}
					peak[j] = mag[1];
					bin[j] = i - 1;
					break;
				}
			}
        }
		printf("Average = %lf\n\n", average_power);
		printf("peak Average = %lf\n\n", (peak[0]+peak[1]+peak[2]+peak[3]+peak[4])/5);

		printf("bin %d, %d, %d, %d, %d\n\n", bin[0], bin[1], bin[2], bin[3], bin[4]);
		printf("peak %lf, %lf, %lf, %lf, %lf\n\n", peak[0], peak[1], peak[2], peak[3], peak[4]);

		printf("end of sample\n\n");

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