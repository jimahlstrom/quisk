// This module provides C access to the WDSP SDR library.

#include <Python.h>
#include <stdlib.h>
#include <complex.h>

#define CLIP32			2147483647
#define CLIP16			32767

#define MAX_CHANNELS	32

static struct _channel {
	complex double * cBuf;	// circular sample buffer; save samples until nBuf reaches in_size
	int sizeBuf;		// current size of cBuf in complex samples; a multiple of in_size
	int nBuf;		// current number of complex samples in cBuf
	int in_size;		// count of complex samples needed for wdsp_fexchange0
	int in_use;		// is this channel being used?
	int Windex;		// index of write
	int Rindex;		// index of read
} wdspChannel[MAX_CHANNELS];

static void (*wdsp_fexchange0) (int channel, double * in, double * out, int * error);

int wdspFexchange0(int channel, complex double * cSamples, int nSamples)
{
// Call with an arbitrary number of input samples nSamples. cSamples is scaled to CLIP32.
// When we have enough samples process the samples by calling wdsp. Return the samples in cSamples.
// cSamples is complex double. nSamples is the count of complex samples.
	struct _channel * ptChannel = wdspChannel + channel;
	int i, error, in_size;

	if ( ! ptChannel->in_use) {
		ptChannel->Windex = 0;
		ptChannel->Rindex = 0;
		ptChannel->nBuf = 0;
		return nSamples;
	}
	if ( ! wdsp_fexchange0)
		return nSamples;
	if (nSamples <= 0)
		return nSamples;
	in_size = ptChannel->in_size;
	i = nSamples / in_size + 3;	// blocks need for samples plus a partial block
	if (i * in_size > ptChannel->sizeBuf) {
		i *= in_size;
		ptChannel->sizeBuf = i;
		ptChannel->cBuf = (complex double *)realloc(ptChannel->cBuf, i * sizeof(complex double));
	}
	for (i = 0; i < nSamples; i++) {	// Copy samples from cSamples to cBuf
		ptChannel->cBuf[ptChannel->Windex++] = cSamples[i] / CLIP32;
		if (ptChannel->Windex >= ptChannel->sizeBuf)
			ptChannel->Windex = 0;
	}
	ptChannel->nBuf += nSamples;
	nSamples = 0;
	while (ptChannel->nBuf >= in_size) {
		(*wdsp_fexchange0)(channel, (double *)(ptChannel->cBuf + ptChannel->Rindex), (double *)(cSamples + nSamples), &error);
		if (error)
			printf("WDSP: wdsp_fexchange0 error %d\n", error);
		ptChannel->Rindex += in_size;
		if (ptChannel->Rindex >= ptChannel->sizeBuf)
			ptChannel->Rindex = 0;
		nSamples += in_size;
		ptChannel->nBuf -= in_size;
	}
	for (i = 0; i < nSamples; i++)
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

