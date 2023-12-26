#include <Python.h>
#include <complex.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Version.h>

#define IMPORT_QUISK_API
#include "quisk.h"
#include "filter.h"

// This module was written by James Ahlstrom, N2ADR.

// This module uses the Python interface to import symbols from the parent _quisk
// extension module.  It must be linked with import_quisk_api.c.  See the documentation
// at the start of import_quisk_api.c.

#define DEBUG		0

#define RX_BUF_SIZE	(SAMP_BUFFER_SIZE / 2)
static SoapySDRDevice * soapy_sample_device;		// device for the sample stream
static SoapySDRDevice * soapy_config_device;		// device for configuration, not samples
static SoapySDRStream * rxStream;
static SoapySDRStream * txStream;
static double rx_sample_rate = 48000;
static double tx_sample_rate = 48000;
static complex float rx_stream_buffer[RX_BUF_SIZE];
static void * rx_stream_buffs[] = {rx_stream_buffer};
static int shutdown_sample_device;
static int data_poll_usec = 10000;
static int soapy_FDX;					// Full duplex flag
static int soapy_KeyDown;				// Current key state
static int soapy_KeyWasDown;				// Previous key state
static size_t numTxChannels;				// number of Tx channels
static size_t txMTU;					// maximum transmission unit of Tx stream

// Start sample capture; called from the sound thread.
static void quisk_start_samples(void)
{
	//setup an Rx and Tx stream (complex floats)
#if SOAPY_SDR_API_VERSION < 0x00080000
	if (SoapySDRDevice_setupStream(soapy_sample_device, &rxStream, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, NULL) != 0) {
		printf("Soapy Rx setupStream fail: %s\n", SoapySDRDevice_lastError());
		return;
	}
	if (numTxChannels) {
		if (SoapySDRDevice_setupStream(soapy_sample_device, &txStream, SOAPY_SDR_TX, SOAPY_SDR_CF32, NULL, 0, NULL) != 0) {
			printf("Soapy Tx setupStream fail: %s\n", SoapySDRDevice_lastError());
			return;
		}
		txMTU = SoapySDRDevice_getStreamMTU(soapy_sample_device, txStream);
	}
#else
	if ((rxStream = SoapySDRDevice_setupStream(soapy_sample_device, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, NULL)) == NULL) {
		printf("Soapy Rx setupStream fail: %s\n", SoapySDRDevice_lastError());
		return;
	}
	if (numTxChannels) {
		if ((txStream = SoapySDRDevice_setupStream(soapy_sample_device, SOAPY_SDR_TX, SOAPY_SDR_CF32, NULL, 0, NULL)) == NULL) {
			printf("Soapy Tx setupStream fail: %s\n", SoapySDRDevice_lastError());
			return;
		}
		txMTU = SoapySDRDevice_getStreamMTU(soapy_sample_device, txStream);
	}
#endif
	SoapySDRDevice_activateStream(soapy_sample_device, rxStream, 0, 0, 0); //start streaming
}

// Stop sample capture; called from the sound thread.
static void quisk_stop_samples(void)
{
	shutdown_sample_device = 1;
	if (rxStream) {
		SoapySDRDevice_deactivateStream(soapy_sample_device, rxStream, 0, 0); //stop streaming
		SoapySDRDevice_closeStream(soapy_sample_device, rxStream);
		rxStream = NULL;
	}
	if (txStream) {
		SoapySDRDevice_deactivateStream(soapy_sample_device, txStream, 0, 0); //stop streaming
		SoapySDRDevice_closeStream(soapy_sample_device, txStream);
		txStream = NULL;
	}
}

// Called in a loop to read samples; called from the sound thread.
static int quisk_read_samples(complex double * cSamples)
{
	int i, flags; //flags set by receive operation
	long long timeNs; //timestamp for receive buffer
	int nSamples;
	int num_samp;

	soapy_KeyDown = quisk_is_key_down();
	num_samp = (int)(rx_sample_rate * (data_poll_usec * 1E-6));
	num_samp = ((num_samp + 255) / 256) * 256;
	if (num_samp > RX_BUF_SIZE)
		num_samp = RX_BUF_SIZE;
	if (shutdown_sample_device) {
		if (rxStream) {
			quisk_stop_samples();
		}
		if (soapy_sample_device) {
			SoapySDRDevice_unmake(soapy_sample_device);
			soapy_sample_device = NULL;
		}
		nSamples = num_samp;
		for (i = 0; i < nSamples; i++)
			cSamples[i] = 0;
	}
	else if (rxStream) {
		nSamples = SoapySDRDevice_readStream(soapy_sample_device, rxStream, rx_stream_buffs, num_samp, &flags, &timeNs, data_poll_usec * 2);
		if (nSamples == -1) {	// Timeout
			nSamples = 0;
		}
		else if (nSamples < 0) {	// Some other error
			pt_quisk_sound_state->read_error++;
			nSamples = 0;
		}
		pt_quisk_sound_state->latencyCapt = 0;
		for (i = 0; i < nSamples; i++)
			cSamples[i] = rx_stream_buffer[i] * CLIP32;
	}
	else {
		nSamples = num_samp;
		for (i = 0; i < nSamples; i++)
			cSamples[i] = 0;
	}
	return nSamples;	// return number of samples
}

// Called in a loop to write samples; called from the sound thread.
static int quisk_write_samples(complex double * cSamples, int nSamples)
{
	static complex float * tx_stream_buffer = NULL;
	static int tx_buf_size = 0;
	static long long timeNs = 0;
	int i, flags, ret, count;

	if ( ! txStream)
		return 0;
	if (soapy_KeyDown != soapy_KeyWasDown) {	// key changed state
		soapy_KeyWasDown = soapy_KeyDown;
		if (DEBUG)
			printf("**** Key Change %i rate %.0f\n", soapy_KeyDown, tx_sample_rate);
		if (soapy_KeyDown)	// Key went down
			SoapySDRDevice_activateStream(soapy_sample_device, txStream, 0, 0, 0);		// start Tx streaming
		else			// Key went up
			SoapySDRDevice_deactivateStream(soapy_sample_device, txStream, 0, 0);		// stop Tx streaming
	}
	if ( ! soapy_KeyDown || nSamples <= 0)
		return 0;

	if (tx_buf_size < nSamples) {
		if (tx_stream_buffer)
			free(tx_stream_buffer);
		tx_buf_size = nSamples * 2;
		tx_stream_buffer = (complex float *)malloc(tx_buf_size * sizeof(complex float));
	}
	for (i = 0; i < nSamples; i++)
		tx_stream_buffer[i] = cSamples[i] / CLIP16;
	while (nSamples > 0) {
		if (nSamples > (int)txMTU)
			count = txMTU;
		else
			count = nSamples;
		nSamples -= count;
		timeNs  = 0; //+= count / tx_sample_rate * 1E9;
		ret = SoapySDRDevice_writeStream(soapy_sample_device, txStream, (void *)&tx_stream_buffer, count, &flags, timeNs, data_poll_usec * 2);
		if (ret < 0)
			printf("Soapy writeStream fail: %s\n", SoapySDRDevice_lastError());
		if (ret != count)
			printf ("Soapy writeStream short write; %d < %d\n", ret, count);
		//printf ("Soapy writeStream write; %i  %i\n", ret, count);
	}
	return 0;
}

// Called to close the sample source; called from the GUI thread.
static PyObject * close_device(PyObject * self, PyObject * args)
{
	int sample_device;

	if (!PyArg_ParseTuple (args, "i", &sample_device))
		return NULL;
	if (sample_device) {
		shutdown_sample_device = 1;
	}
	else {
		if (soapy_config_device) {
			SoapySDRDevice_unmake(soapy_config_device);
			soapy_config_device = NULL;
		}
	}
	Py_INCREF (Py_None);
	return Py_None;
}

// Called to open the SoapySDR device; called from the GUI thread.
static PyObject * open_device(PyObject * self, PyObject * args)
{
	int sample_device, poll;
	const char * name;
	char buf128[128];
	SoapySDRDevice * sdev;

	if (!PyArg_ParseTuple (args, "sii", &name, &sample_device, &poll))
		return NULL;
	sdev = SoapySDRDevice_makeStrArgs(name);
	if(sdev) {
		snprintf(buf128, 128, "Capture from %s", name);
		if (sample_device) {
			shutdown_sample_device = 0;
			soapy_sample_device = sdev;
			data_poll_usec = poll;
			quisk_sample_source4(&quisk_start_samples, &quisk_stop_samples, &quisk_read_samples, &quisk_write_samples);
			numTxChannels = SoapySDRDevice_getNumChannels(sdev, SOAPY_SDR_TX);
			if (sample_device == 3)		// disable transmit
				numTxChannels = 0;
		}
		else {
			soapy_config_device = sdev;
		}
	}
	else {
		snprintf(buf128, 128, "SoapySDRDevice_make fail: %s", SoapySDRDevice_lastError());
	}
	return PyString_FromString(buf128);
}

static void get_direc_len(const char * name, int * direction, int * length)
{  // return the direction (Rx or Tx) and length of name to compare
	*length = strlen(name);
	*direction = SOAPY_SDR_RX;
	if (*length < 4)
		return;
	if (name[*length - 1] == 'x' && name[*length - 3] == '_') {	// ends in "_rx" or "_tx"
		if (name[*length - 2] == 't')
			*direction = SOAPY_SDR_TX;
		*length -= 3;
	}
}

// Get a list of SoapySDR devices
static PyObject * get_device_list(PyObject * self, PyObject * args)	// Called from GUI thread
{
	PyObject * devices;
	PyObject * dict;
	size_t length, i, j;
	SoapySDRKwargs * results;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	devices = PyList_New(0);
	results = SoapySDRDevice_enumerate(NULL, &length);
	for (i = 0; i < length; i++) {
		dict = PyDict_New();
		for (j = 0; j < results[i].size; j++)
			PyDict_SetItemString(dict, results[i].keys[j], PyString_FromString(results[i].vals[j]));
		PyList_Append(devices, dict);
		Py_DECREF(dict);
	}
	SoapySDRKwargsList_clear(results, length);
	return devices;
}

static PyObject * set_parameter(PyObject * self, PyObject * args)	// Called from GUI thread
{  // Parameter name can end in "_rx" or "_tx" to specify direction.
	int direction, length;
	const char * param;	// name of the parameter
	const char * name2;	// string data or sub-parameter name if any
	double datum;		// floating point value if any
	bool is_true;
	char msg200[200];

	if (!PyArg_ParseTuple (args, "ssd", &param, &name2, &datum))
		return NULL;
	if (DEBUG)
		printf ("Set %s - %s - %lf\n", param, name2, datum);
	get_direc_len(param, &direction, &length);
	msg200[0] = 0;
	if (soapy_sample_device) {
		if (numTxChannels <= 0 && direction == SOAPY_SDR_TX) {
		}
		//else if (direction == SOAPY_SDR_TX) {
		//}
		else if ( ! strcmp(param, "soapy_FDX")) {
			if (datum)
				soapy_FDX = 1;
			else
				soapy_FDX = 0;
		}
		else if ( ! strncmp(param, "soapy_setAntenna", length)) {	// do not set empty string
			if (name2[0] && SoapySDRDevice_setAntenna(soapy_sample_device, direction, 0, name2) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
		else if ( ! strncmp(param, "soapy_setBandwidth", length)) {
			if (soapy_sample_device && SoapySDRDevice_setBandwidth(soapy_sample_device, direction, 0, datum) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
		else if ( ! strncmp(param, "soapy_setFrequency", length)) {
			if (SoapySDRDevice_setFrequency(soapy_sample_device, direction, 0, datum, NULL) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
		else if ( ! strncmp(param, "soapy_setGain", length)) {
			if (soapy_sample_device && SoapySDRDevice_setGain(soapy_sample_device, direction, 0, datum) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
        	else if ( ! strncmp(param, "soapy_setGainElement", length)) {
			if (SoapySDRDevice_setGainElement(soapy_sample_device, direction, 0, name2, datum) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
      		else if ( ! strncmp(param, "soapy_setGainMode", length)) {
			if ( ! strcmp(name2, "true"))
				is_true = 1;
			else
				is_true = 0;
			if (SoapySDRDevice_setGainMode(soapy_sample_device, direction, 0, is_true) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
		else if ( ! strncmp(param, "soapy_setSampleRate", length)) {
			if (direction == SOAPY_SDR_RX)
				rx_sample_rate = datum;
			else
				tx_sample_rate = datum;
			if (SoapySDRDevice_setSampleRate(soapy_sample_device, direction, 0, datum) != 0)
				snprintf(msg200, 200, "%s fail: %s\n", param, SoapySDRDevice_lastError());
		}
		else {
			snprintf(msg200, 200, "Soapy set_parameter() for unknown name %s\n", param);
		}
	}
	if (msg200[0])
		return PyString_FromString(msg200);
	Py_INCREF (Py_None);
	return Py_None;
}

static void Range2List(SoapySDRRange range, PyObject * pylist)
{
	PyObject * pyobj;

	pyobj = PyFloat_FromDouble(range.minimum);
	PyList_Append(pylist, pyobj);
	Py_DECREF(pyobj);
	pyobj = PyFloat_FromDouble(range.maximum);
	PyList_Append(pylist, pyobj);
	Py_DECREF(pyobj);
	pyobj = PyFloat_FromDouble(range.step);
	PyList_Append(pylist, pyobj);
	Py_DECREF(pyobj);
}

static PyObject * get_parameter(PyObject * self, PyObject * args)	// Called from GUI thread
{ // Return a SoapySDR parameter.
  // Parameter name can end in "_rx" or "_tx" to specify direction.
	int sample_device, direction, length;
	char * name;
	char ** names;
	size_t i, len_list;
	bool is_true;
	double value;
	PyObject * pylist, * pyobj, * pylst2;
	SoapySDRDevice * sdev;
	SoapySDRRange range;
	SoapySDRRange * ranges;

	if (!PyArg_ParseTuple (args, "si", &name, &sample_device))
		return NULL;
	if (sample_device)
		sdev = soapy_sample_device;
	else
		sdev = soapy_config_device;
	get_direc_len(name, &direction, &length);
	if ( ! sdev) {
		;
	}
	else if ( ! strncmp(name, "soapy_listAntennas", length)) {
		pylist = PyList_New(0);
		names = SoapySDRDevice_listAntennas(sdev, direction, 0, &len_list);
		for (i = 0; i < len_list; i++) {
			pyobj = PyString_FromString(names[i]);
			PyList_Append(pylist, pyobj);
			Py_DECREF(pyobj);
		}
		SoapySDRStrings_clear(&names, len_list);
		return pylist;
	}
	else if ( ! strncmp(name, "soapy_getBandwidth", length)) {
		value = SoapySDRDevice_getBandwidth(sdev, direction, 0);
		return PyFloat_FromDouble(value);
	}
	else if ( ! strncmp(name, "soapy_getBandwidthRange", length)) {
		pylist = PyList_New(0);
		ranges = SoapySDRDevice_getBandwidthRange(sdev, direction, 0, &len_list);
		for (i = 0; i < len_list; i++) {
			pylst2 = PyList_New(0);
			range = ranges[i];
			Range2List(range, pylst2);
			PyList_Append(pylist, pylst2);
			Py_DECREF(pylst2);
		}
		return pylist;
	}
	else if ( ! strncmp(name, "soapy_getFullDuplex", length)) {
		is_true = SoapySDRDevice_getFullDuplex(sdev, direction, 0);
		if (is_true)
			return PyInt_FromLong(1);
		else
			return PyInt_FromLong(0);
	}
	else if ( ! strncmp(name, "soapy_getGainRange", length)) {
		pylist = PyList_New(0);
		range = SoapySDRDevice_getGainRange(sdev, direction, 0);
		Range2List(range, pylist);
		return pylist;
	}
	else if ( ! strncmp(name, "soapy_getSampleRate", length)) {
		value = SoapySDRDevice_getSampleRate(sdev, direction, 0);
		return PyFloat_FromDouble(value);
	}
	else if ( ! strncmp(name, "soapy_getSampleRateRange", length)) {
		pylist = PyList_New(0);
		ranges = SoapySDRDevice_getSampleRateRange(sdev, direction, 0, &len_list);
		for (i = 0; i < len_list; i++) {
			pylst2 = PyList_New(0);
			range = ranges[i];
			Range2List(range, pylst2);
			PyList_Append(pylist, pylst2);
			Py_DECREF(pylst2);
		}
		return pylist;
	}
	else if ( ! strncmp(name, "soapy_hasGainMode", length)) {
		is_true = SoapySDRDevice_hasGainMode(sdev, direction, 0);
		if (is_true)
			return PyInt_FromLong(1);
		else
			return PyInt_FromLong(0);
	}
	else if ( ! strncmp(name, "soapy_listGains", length)) {
		pylist = PyList_New(0);
		names = SoapySDRDevice_listGains(sdev, direction, 0, &len_list);
		for (i = 0; i < len_list; i++) {
			pyobj = PyString_FromString(names[i]);
			PyList_Append(pylist, pyobj);
			Py_DECREF(pyobj);
		}
		SoapySDRStrings_clear(&names, len_list);
		return pylist;
	}
	else if ( ! strncmp(name, "soapy_listGainsValues", length)) {
		pylist = PyList_New(0);
		pylst2 = PyList_New(0);		// First element is the total gain
		pyobj = PyString_FromString("total");
		PyList_Append(pylst2, pyobj);
		Py_DECREF(pyobj);
		range = SoapySDRDevice_getGainRange(sdev, direction, 0);
		Range2List(range, pylst2);
		PyList_Append(pylist, pylst2);
		Py_DECREF(pylst2);
		names = SoapySDRDevice_listGains(sdev, direction, 0, &len_list);
		for (i = 0; i < len_list; i++) {
			pylst2 = PyList_New(0);
			pyobj = PyString_FromString(names[i]);
			PyList_Append(pylst2, pyobj);
			Py_DECREF(pyobj);
			range = SoapySDRDevice_getGainElementRange(sdev, direction, 0, names[i]);
			Range2List(range, pylst2);
			PyList_Append(pylist, pylst2);
			Py_DECREF(pylst2);
		}
		SoapySDRStrings_clear(&names, len_list);
		return pylist;
	}
	else {
		printf("Soapy get_parameter() for unknown name %s\n", name);
	}
	Py_INCREF (Py_None);
	return Py_None;
}

// Functions callable from Python are listed here:
static PyMethodDef QuiskMethods[] = {
	{"open_device", open_device, METH_VARARGS, "Open the hardware."},
	{"close_device", close_device, METH_VARARGS, "Close the hardware"},
	{"get_device_list", get_device_list, METH_VARARGS, "Return a list of SoapySDR devices"},
	{"get_parameter", get_parameter, METH_VARARGS, "Get a SoapySDR parameter"},
	{"set_parameter", set_parameter, METH_VARARGS, "Set a SoapySDR parameter"},
	{NULL, NULL, 0, NULL}		/* Sentinel */
};

#if PY_MAJOR_VERSION < 3
// Python 2.7:
// Initialization, and registration of public symbol "initsoapy":
PyMODINIT_FUNC initsoapy (void)
{
	if (Py_InitModule ("soapy", QuiskMethods) == NULL) {
		printf("Py_InitModule failed!\n");
		return;
	}
	// Import pointers to functions and variables from module _quisk
	if (import_quisk_api()) {
		printf("Failure to import pointers from _quisk\n");
		return;		//Error
	}
}

// Python 3:
#else
static struct PyModuleDef soapymodule = {
	PyModuleDef_HEAD_INIT,
	"soapy",
	NULL,
	-1,
	QuiskMethods
} ;

PyMODINIT_FUNC PyInit_soapy(void)
{
	PyObject * m;

	m = PyModule_Create(&soapymodule);
	if (m == NULL)
		return NULL;

	// Import pointers to functions and variables from module _quisk
	if (import_quisk_api()) {
		printf("Failure to import pointers from _quisk\n");
		return m;		//Error
	}
	return m;
}
#endif
