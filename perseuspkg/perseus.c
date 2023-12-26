/*
 * 
 * Microtelecom perseus HF receiver 
 * 
 * access module: exposes Python functions needed in quisk_hardware.py
 * to control hardware
 * 
 */
  
#include <Python.h>
#include <stdio.h> 
#include <string.h> 
#include <fcntl.h> 
#include <sys/stat.h> 
#include <sys/types.h> 
#include <unistd.h> 
#include <complex.h>
#include <perseus-sdr.h>

#define IMPORT_QUISK_API
#include "quisk.h"
#include "filter.h"

// This module was written by Andrea Montefusco IW0HDV.

typedef union {
	struct {
		int32_t	i;
		int32_t	q;
		} __attribute__((__packed__)) iq;
	struct {
		uint8_t		i1;
		uint8_t		i2;
		uint8_t		i3;
		uint8_t		i4;
		uint8_t		q1;
		uint8_t		q2;
		uint8_t		q3;
		uint8_t		q4;
		} __attribute__((__packed__)) ;
} iq_sample;


// buffer size for libperseus-sdr
const static	int nb = 6;
const static	int bs = 1024;


// This module uses the Python interface to import symbols from the parent _quisk
// extension module.  It must be linked with import_quisk_api.c.  See the documentation
// at the start of import_quisk_api.c.

#define DEBUG		1

static int num_perseus = 0;
static 	perseus_descr *descr = 0;
static int sr = 48000;
static float freq = 7050000.0;
static int adc_dither = 0;
static int adc_preamp = 0;

static void quisk_stop_samples(void);

static const char *fname = "/tmp/quiskperseus";
static int rfd = 0;
static int wfd = 0;
static int running = 0;
static int wb_filter = 0;

// Called in a loop to read samples; called from the sound thread.
static int quisk_read_samples(complex double * cSamples)
{
	//fprintf (stderr, "r"); fflush(stderr);

	int n = read(rfd, cSamples, sizeof(complex double)*SAMP_BUFFER_SIZE); 
	//fprintf(stderr, "%d ", n);
	if (n >= 0)
		return n/sizeof(complex double);	// return number of samples
	else
		return 0;
}

// Called in a loop to write samples; called from the sound thread.
static int quisk_write_samples(complex double * cSamples, int nSamples)
{
	return 0;
}


//
// callback that writes in the output pipe IQ values as
// complex floating point
//
static int user_data_callback_c_f(void *buf, int buf_size, void *extra)
{
	// The buffer received contains 24-bit IQ samples (6 bytes per sample)
	// Here we save the received IQ samples as 32 bit 
	// (msb aligned) integer IQ samples.

	uint8_t	*samplebuf	= (uint8_t*)buf;
	int nSamples 		= buf_size/6;
	int k;
	iq_sample s;

	// the 24 bit data is scaled to a 32bit value (so that the machine's
	// natural signed arithmetic will work)
	for (k=0; k < nSamples; k++) {
		s.i1 = s.q1 = 0;
		s.i2 = *samplebuf++;
		s.i3 = *samplebuf++;
		s.i4 = *samplebuf++;
		s.q2 = *samplebuf++;
		s.q3 = *samplebuf++;
		s.q4 = *samplebuf++;

		// move I/Q to complex number
		complex double x = (double)(s.iq.i)*10 + (double)(s.iq.q)*10 * _Complex_I;
		if (wfd > 0) {
			int n = write(wfd, &x, sizeof(complex double));
			if (n<0 && ! -EAGAIN ) 
				fprintf(stderr, "perseus c: Can't write output file: %s, descriptor: %d\n", strerror(errno), wfd);
		}
	}
	return 0;
}



// Start sample capture; called from the sound thread.
static void quisk_start_samples(void)
{
	if (DEBUG) { fprintf (stderr, "perseus c: quisk_start_samples\n"); fflush(stderr); }

	int rc = mkfifo(fname, 0666); 
	
	if ((rc == -1) && (errno != EEXIST)) {
		perror("perseus c: Error creating the named pipe");
	}

	rfd = open(fname, O_RDONLY|O_NONBLOCK); 
	if (rfd < 0) {
		fprintf(stderr, "perseus c: Can't open read FIFO (%s)\n", strerror(errno));
	} else {
		if (DEBUG) fprintf(stderr, "perseus c: read FIFO (%d)\n", rfd);
	}
	wfd = open(fname, O_WRONLY|O_NONBLOCK);
	if (wfd < 0) {
		fprintf(stderr, "perseus c: Can't open write FIFO (%s)\n", strerror(errno));
	} else {
		if (DEBUG) fprintf(stderr, "perseus c: write FIFO (%d)\n", wfd);
	}
	if (perseus_set_sampling_rate(descr, sr) < 0) {  // specify the sampling rate value in Samples/second
		fprintf(stderr, "perseus c: fpga configuration error: %s\n", perseus_errorstr());
	} else {
		if (DEBUG) fprintf(stderr, "perseus c: sampling rate set to: %d\n", sr);

		// Re-enable preselection filters (WB_MODE Off)
		perseus_set_ddc_center_freq(descr, freq, wb_filter);
		// start sampling ops
		if (perseus_start_async_input(descr, nb*bs, user_data_callback_c_f, 0)<0) {
			fprintf(stderr, "perseus c: start async input error: %s\n", perseus_errorstr());
		} else {
			if (DEBUG) fprintf(stderr, "perseus c: start async input\n");
		}
		running = 1;
	}
}

// Stop sample capture; called from the sound thread.
static void quisk_stop_samples(void)
{
	if (DEBUG) { fprintf (stderr, "perseus c: quisk_stop_samples\n"); fflush(stderr); }

	// We stop the acquisition...
	if (DEBUG) fprintf(stderr, "perseus c: stopping async data acquisition...\n");
	perseus_stop_async_input(descr);
	running = 0;
	// clearing FIFO...
	close(rfd);
	close(wfd);
	unlink(fname);
}


// Called to close the sample source; called from the GUI thread.
static PyObject * close_device(PyObject * self, PyObject * args)
{
	if (DEBUG) fprintf (stderr, "perseus c: close_device\n");
	int sample_device; // for now one only Perseus can be managed

	if (!PyArg_ParseTuple (args, "i", &sample_device))
		return NULL;

	if (descr) {
		// We stop the acquisition...
		if (running) {
			perseus_stop_async_input(descr);
			running = 0;
		}
		perseus_close(descr);
		descr = 0;
	}
	Py_INCREF (Py_None);
	return Py_None;
}

// Called to open the Perseus SDR device; called from the GUI thread.
static PyObject * open_device(PyObject * self, PyObject * args)
{
	char buf128[128] = "Capture Microtelecom Perseus HF receiver";
	eeprom_prodid prodid;

	if (DEBUG) { fprintf (stderr, "perseus c: open device (%d)\n", num_perseus); fflush(stderr); }

	// Check how many Perseus receivers are connected to the system
	if (num_perseus == 0) num_perseus = perseus_init();
	if (DEBUG) fprintf(stderr, "perseus c: %d Perseus receivers found\n",num_perseus);

	if (num_perseus == 0) {
		sprintf(buf128, "No Perseus receivers detected\n");
		perseus_exit();
		goto main_cleanup;
	}

	// Open the first one...
	if ((descr = perseus_open(0)) == NULL) {
		sprintf(buf128, "error: %s\n", perseus_errorstr());
		fprintf(stderr, "perseus c: open error: %s\n", perseus_errorstr());
		goto main_cleanup;
	}

	// Download the standard firmware to the unit
	if (DEBUG) fprintf(stderr, "perseus c: Downloading firmware...\n");
	if (perseus_firmware_download(descr,NULL)<0) {
		sprintf(buf128, "perseus c: firmware download error: %s", perseus_errorstr());
		goto main_cleanup;
	}
	// Dump some information about the receiver (S/N and HW rev)
	if (perseus_is_preserie(descr, 0) ==  PERSEUS_SNNOTAVAILABLE) {
		fprintf(stderr, "perseus c: The device is a preserie unit");
	} else {
		if (perseus_get_product_id(descr,&prodid)<0) {
			fprintf(stderr, "perseus c: get product id error: %s", perseus_errorstr());
		} else {
			if (DEBUG) {
				fprintf(stderr, "perseus c: Receiver S/N: %05d-%02hX%02hX-%02hX%02hX-%02hX%02hX - HW Release:%hd.%hd\n",
					(uint16_t) prodid.sn, 
					(uint16_t) prodid.signature[5],
					(uint16_t) prodid.signature[4],
					(uint16_t) prodid.signature[3],
					(uint16_t) prodid.signature[2],
					(uint16_t) prodid.signature[1],
					(uint16_t) prodid.signature[0],
					(uint16_t) prodid.hwrel,
					(uint16_t) prodid.hwver);
			}
		}
	}
    // Printing all sampling rates available .....
    {
        int buf[BUFSIZ];

        if (perseus_get_sampling_rates (descr, buf, sizeof(buf)/sizeof(buf[0])) < 0) {
			fprintf(stderr, "perseus c: get sampling rates error: %s\n", perseus_errorstr());
			goto main_cleanup;
        } else {
            int i = 0;
            while (buf[i]) {
                if (DEBUG) fprintf(stderr, "perseus c: #%d: sample rate: %d\n", i, buf[i]);
                i++;
            }
        }
    }

	// Configure the receiver for 2 MS/s operations
	if (DEBUG) fprintf(stderr, "perseus c: Configuring FPGA...\n");
	if (perseus_set_sampling_rate(descr, sr) < 0) {  // specify the sampling rate value in Samples/second
	//if (perseus_set_sampling_rate_n(descr, 0)<0)        // specify the sampling rate value as ordinal in the vector
		fprintf(stderr, "perseus c: fpga configuration error: %s\n", perseus_errorstr());
		goto main_cleanup;
	}

	// ADC settings
	perseus_set_adc (descr, adc_dither, adc_preamp);
	
	// Disable preselection filters (WB_MODE On)
	//perseus_set_ddc_center_freq(descr, freq, 0);
	//sleep(1);
	// Re-enable preselection filters (WB_MODE Off)
	perseus_set_ddc_center_freq(descr, freq, wb_filter);
	
	quisk_sample_source4(&quisk_start_samples, &quisk_stop_samples, &quisk_read_samples, &quisk_write_samples);
	
	if (DEBUG) { fprintf (stderr, "perseus c: quisk sample source callbacks established\n"); fflush(stderr); }
	goto exit_success;



	main_cleanup:
	return PyString_FromString("ERROR");

	exit_success:
	
	return PyString_FromString(buf128);
	
	
}

static PyObject * set_frequency(PyObject * self, PyObject * args)	// Called from GUI thread
{
	float param;
	
	if (!PyArg_ParseTuple (args, "f", &param))
		return NULL;
	if (DEBUG)
		fprintf (stderr, "perseus c: set DDC frequency %lf WB filter:%d\n", param, wb_filter);
	freq= param;
	if (descr) perseus_set_ddc_center_freq(descr, freq, wb_filter);

	Py_INCREF (Py_None);
	return Py_None;
}


static PyObject * set_input_filter(PyObject * self, PyObject * args)	// Called from GUI thread
{
	int param;
	
	if (!PyArg_ParseTuple (args, "i", &param))
		return NULL;
	if (DEBUG)
		fprintf (stderr, "perseus c: set input filter %d\n", param);
	wb_filter = param;
	if (descr) perseus_set_ddc_center_freq(descr, freq, wb_filter);

	Py_INCREF (Py_None);
	return Py_None;
}


static PyObject * set_sampling_rate(PyObject * self, PyObject * args)	// Called from GUI thread
{
	int param;

	if (!PyArg_ParseTuple (args, "i", &param))
		return NULL;
	
	if (DEBUG) fprintf (stderr, "perseus c: Set sampling rate %d\n", param);
	if (param < 48000) sr = param * 1000;
	else sr = param;

	if (descr) {
		if (running) {
			if (DEBUG) fprintf(stderr, "perseus c: stop async input\n");
			perseus_stop_async_input(descr);
		}

		// specify the sampling rate value in Samples/secon
		if (perseus_set_sampling_rate(descr, sr) < 0) {
			fprintf(stderr, "perseus c: fpga configuration error: %s\n", perseus_errorstr());
		}

		if (running) {
			if (perseus_start_async_input(descr, nb*bs, user_data_callback_c_f, 0)<0) {
				fprintf(stderr, "perseus c: start async input error: %s\n", perseus_errorstr());
			} else {
				if (DEBUG) fprintf(stderr, "perseus c: start async input @%d\n", sr);
			}
		}
	} else {
		fprintf(stderr, "perseus c: trying to start async input with no device open\n");
	}
	Py_INCREF (Py_None);
	return Py_None;
}


static PyObject * set_attenuator(PyObject * self, PyObject * args)	// Called from GUI thread
{	int param;

	if (!PyArg_ParseTuple (args, "i", &param))
		return NULL;
	if (DEBUG)
		fprintf (stderr, "perseus c: Set attenuator %d\n", param);

	if (descr) {
		// specify the sampling rate value in Samples/secon
		if (perseus_set_attenuator_n(descr, (int)(param / -10)) < 0) {
			fprintf(stderr, "perseus c: fpga configuration error: %s\n", perseus_errorstr());
		}
	}
	Py_INCREF (Py_None);
	return Py_None;
}


// Enable ADC Dither, Disable ADC Preamp
//	perseus_set_adc(descr, true, false);

static PyObject * set_adc_dither (PyObject * self, PyObject * args)	// Called from GUI thread
{	int dither_;

	if (!PyArg_ParseTuple (args, "i", &dither_))
		return NULL;
	if (DEBUG)
		fprintf (stderr, "perseus c: Set ADC: dither %d\n", dither_);

	adc_dither = dither_;
	if (descr) {
		// specify the ADC dithering
		if (perseus_set_adc(descr, adc_dither == 1, adc_preamp == 1) < 0) {
			fprintf(stderr, "perseus c: ADC configuration error: %s\n", perseus_errorstr());
		}
	}
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_adc_preamp (PyObject * self, PyObject * args)	// Called from GUI thread
{	int preamp_;

	if (!PyArg_ParseTuple (args, "i", &preamp_))
		return NULL;
	if (DEBUG)
		fprintf (stderr, "perseus c: Set ADC: preamp: %d\n", preamp_);

	adc_preamp = preamp_;
	if (descr) {
		// specify the sampling rate value in Samples/secon
		if (perseus_set_adc(descr,  adc_dither == 1, adc_preamp == 1) < 0) {
			fprintf(stderr, "perseus c: ADC configuration error: %s\n", perseus_errorstr());
		}
	}
	Py_INCREF (Py_None);
	return Py_None;
}


static PyObject * deinit(PyObject * self, PyObject * args)	// Called from dctor
{
	perseus_exit();

	Py_INCREF (Py_None);
	return Py_None;
}


// Functions callable from Python are listed here:
static PyMethodDef QuiskMethods[] = {
	{"open_device", open_device, METH_VARARGS, "Open the hardware."},
	{"close_device", close_device, METH_VARARGS, "Close the hardware"},
	{"set_frequency", set_frequency, METH_VARARGS, "set frequency"},
	{"set_input_filter", set_input_filter, METH_VARARGS, "set input filter"},
	{"set_sampling_rate", set_sampling_rate, METH_VARARGS, "set sampling rate"},
	{"set_attenuator", set_attenuator, METH_VARARGS, "set attenuator"},
	{"set_adc_dither", set_adc_dither, METH_VARARGS, "set ADC dither"},
	{"set_adc_preamp", set_adc_preamp, METH_VARARGS, "set ADC preamplifier"},
	{"deinit", deinit, METH_VARARGS, "deinit"},
//	{"get_device_list", get_device_list, METH_VARARGS, "Return a list of Perseus SDR devices"},
	{NULL, NULL, 0, NULL}		/* Sentinel */
};

#if PY_MAJOR_VERSION < 3
// Python 2.7:
// Initialization, and registration of public symbol "initperseus":
PyMODINIT_FUNC initperseus (void)
{
	if (Py_InitModule ("perseus", QuiskMethods) == NULL) {
		fprintf(stderr, "perseus c: Py_InitModule failed!\n");
		return;
	}
	// Import pointers to functions and variables from module _quisk
	if (import_quisk_api()) {
		fprintf(stderr, "perseus c: Failure to import pointers from _quisk\n");
		return;		//Error
	}
}

// Python 3:
#else
static struct PyModuleDef perseusmodule = {
	PyModuleDef_HEAD_INIT,
	"perseus",
	NULL,
	-1,
	QuiskMethods
} ;

PyMODINIT_FUNC PyInit_perseus(void)
{
	PyObject * m;

	m = PyModule_Create(&perseusmodule);
	if (m == NULL)
		return NULL;

	// Import pointers to functions and variables from module _quisk
	if (import_quisk_api()) {
		fprintf(stderr, "perseus c: Failure to import pointers from _quisk\n");
		return m;		//Error
	}
	return m;
}
#endif

