// This module provides C access to the WDSP SDR library.

#include <Python.h>
#include <stdlib.h>

#define CLIP32			2147483647
#define CLIP16			32767

#define MAX_CHANNELS	32
#define SAMPLE_BYTES	(sizeof(double) * 2)

static struct _channel {
	double * cBuf;	// save samples until nBuf reaches in_size
	int sizeBuf;	// current size of cBuf in count of complex samples
	int nBuf;	// current number of complex samples in cBuf
	int in_size;	// count of complex samples needed for wdsp_fexchange0
	int in_use;	// is this channel being used?
} wdspChannel[MAX_CHANNELS];

static void (*wdsp_fexchange0) (int channel, double * in, double * out, int * error);

int wdspFexchange0(int channel, double * cSamples, int nSamples)
{
// Call with an arbitrary number of input samples nSamples. cSamples is scaled to CLIP32.
// When we have enough samples process the samples by calling wdsp. Return the samples in the same array.
// cSamples is complex double. We treat it as interleaved Real/Imag doubles twice as long.
// nSamples is the count of complex samples.
	struct _channel * ptChannel = wdspChannel + channel;
	int i, error;
	double * ptD, * ptS;

	if ( ! ptChannel->in_use)
		return nSamples;
	if ( ! wdsp_fexchange0)
		return nSamples;
	if (nSamples <= 0)
		return nSamples;
	if (ptChannel->nBuf + nSamples >= ptChannel->sizeBuf) {
		ptChannel->sizeBuf = ptChannel->nBuf + nSamples * 3;
		ptChannel->cBuf = (double *)realloc(ptChannel->cBuf, ptChannel->sizeBuf * SAMPLE_BYTES);
	}
	ptS = cSamples;		// Copy samples from cSamples to cBuf
	ptD = ptChannel->cBuf + ptChannel->nBuf * 2;
	for (i = 0; i < nSamples; i++) {
		*ptD++ = *ptS++ / CLIP32;
		*ptD++ = *ptS++ / CLIP32;
	}
	ptChannel->nBuf += nSamples;
	nSamples = 0;
	while (ptChannel->nBuf >= ptChannel->in_size) {
		(*wdsp_fexchange0)(channel, ptChannel->cBuf, cSamples + nSamples * 2, &error);
		if (error)
			printf("WDSP: wdsp_fexchange0 error %d\n", error);
		nSamples += ptChannel->in_size;
		ptChannel->nBuf -= ptChannel->in_size;
		memmove(ptChannel->cBuf, ptChannel->cBuf + ptChannel->in_size * 2, ptChannel->nBuf * SAMPLE_BYTES);
	}
	for (i = 0; i < nSamples * 2; i++)
		cSamples[i] *= CLIP32;
	return nSamples;
}

PyObject * quisk_wdsp_set_parameter(PyObject * self, PyObject * args, PyObject * keywds)	// Called from the GUI thread.
{  // Call with channel and keyword arguments. Channel is required but may not be needed.
	int channel;
	int in_size = -1;
	int in_use = -1;
	intptr_t fexchange0 = 0;
	static char * kwlist[] = {"channel", "in_size", "fexchange0", "in_use", NULL} ;

	if (!PyArg_ParseTupleAndKeywords (args, keywds, "i|iKi", kwlist, &channel, &in_size, &fexchange0, &in_use))
		return NULL;
        if (channel >= 0 && channel < MAX_CHANNELS) {
		if (fexchange0)
			wdsp_fexchange0 = (void *)fexchange0;
		if (in_size > 0)
			wdspChannel[channel].in_size = in_size;
		if (in_use >= 0)
			wdspChannel[channel].in_use = in_use;
	}
	Py_INCREF (Py_None);
	return Py_None;
}

