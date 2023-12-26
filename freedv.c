#include <Python.h>
#include <stdlib.h>
#include <math.h>
#include <complex.h>	// Use native C99 complex type for fftw3
#include <sys/types.h>

#include "quisk.h"
#include "freedv.h"
#include <stdint.h>

int DEBUG;

#define MAX_RECEIVERS	2

typedef struct {	// from comp.h
  float real;
  float imag;
} COMP;

struct freedv;		// from freedv_api.h
typedef void (*freedv_callback_rx)(void *, char);
typedef char (*freedv_callback_tx)(void *);
/* Protocol bits are packed MSB-first */
/* Called when a frame containing protocol data is decoded */
typedef void (*freedv_callback_protorx)(void *, char *);
/* Called when a frame containing protocol data is to be sent */
typedef void (*freedv_callback_prototx)(void *, char *);
/* Data packet callbacks */
/* Called when a packet has been received */
typedef void (*freedv_callback_datarx)(void *, unsigned char *packet, size_t size);
/* Called when a new packet can be send */
typedef void (*freedv_callback_datatx)(void *, unsigned char *packet, size_t *size);

/* advanced freedv open options required by some modes */
struct freedv_advanced {	// from freedv_api.h
	int interleave_frames;
};

#ifdef MS_WINDOWS
#include <windows.h>
HMODULE hLib;
#define GET_HANDLE1			hLib = LoadLibrary("libcodec2.dll")
#define GET_HANDLE2			hLib = LoadLibrary(".\\freedvpkg\\libcodec2.dll")
#define GET_HANDLE3			hLib = LoadLibrary(".\\freedvpkg\\libcodec2_32.dll")
#define GET_HANDLE4			hLib = LoadLibrary(".\\freedvpkg\\libcodec2_64.dll")
#define GET_ADDR(name)		(void *)GetProcAddress(hLib, name)
#define CLOSE_LIB			FreeLibrary(hLib)
#else
#include <dlfcn.h>
void * hLib;
#define GET_HANDLE1			hLib = dlopen("libcodec2.so", RTLD_LAZY)
#define GET_HANDLE2			hLib = dlopen("./freedvpkg/libcodec2.so", RTLD_LAZY)
#define GET_HANDLE3			hLib = dlopen("./freedvpkg/libcodec2_32.so", RTLD_LAZY)
#define GET_HANDLE4			hLib = dlopen("./freedvpkg/libcodec2_64.so", RTLD_LAZY)
#define GET_ADDR(name)		dlsym(hLib,  name)
#define CLOSE_LIB			dlclose(hLib)
#endif

static int requested_mode = -1;			// requested mode
int freedv_current_mode = -1;			// the current running mode
static int quisk_freedv_squelch;
static int interleave_frames = 1;
static int freedv_version = -1;
static int quisk_set_tx_bpf = 1;
static int handle_index = -1;
// need access to these here and in quisk.c
int n_speech_sample_rate = 8000;
int n_modem_sample_rate = 8000;

#define SPEECH_BUF_SIZE		12000		// speech buffer size increased from 3000
static struct _rx_channel{
	struct freedv * hFreedv;
	COMP * demod_in;
	int rxdata_index;
	short speech_out[SPEECH_BUF_SIZE];		// output buffer
	int speech_available;					// number of samples in output buffer
	int playing;							// are we currently returning speech samples?
} rx_channel[MAX_RECEIVERS] = { 0 };		// safe initialisation to 0

// freedv_version is the library version number, or
//   -1		no library was found
//   -2		a library was found, but freedv_get_version is missing

// FreeDV API functions:
// open, close
struct freedv * (*freedv_open)(int mode);
struct freedv * (*freedv_open_advanced)(int mode, struct freedv_advanced *adv);
void (*freedv_close)(struct freedv *freedv);
// Transmit
void (*freedv_tx)(struct freedv *freedv, short *, short *);
void (*freedv_comptx)(struct freedv *freedv, COMP *, short *);
// Receive
int (*freedv_nin)(struct freedv *freedv);
int (*freedv_rx)(struct freedv *freedv, short *, short demod_in[]);
int (*freedv_floatrx)(struct freedv *freedv, short *, float demod_in[]);
int (*freedv_comprx)(struct freedv *freedv, short *, COMP demod_in[]);
// Set parameters
void (*freedv_set_callback_txt)		(struct freedv *freedv, freedv_callback_rx rx, freedv_callback_tx tx, void *callback_state);
void (*freedv_set_test_frames)			(struct freedv *freedv, int test_frames);
void (*freedv_set_smooth_symbols)		(struct freedv *freedv, int smooth_symbols);
void (*freedv_set_squelch_en)			(struct freedv *freedv, int squelch_en);
void (*freedv_set_snr_squelch_thresh)	(struct freedv *freedv, float snr_squelch_thresh);
void (*freedv_set_tx_bpf)		(struct freedv *freedv, int val);
// Get parameters
int (*freedv_get_version)(void);
void (*freedv_get_modem_stats)(struct freedv *freedv, int *sync, float *snr_est);
int (*freedv_get_test_frames)			(struct freedv *freedv);
int (*freedv_get_n_speech_samples)		(struct freedv *freedv);
int (*freedv_get_n_max_modem_samples)	(struct freedv *freedv);
int (*freedv_get_n_nom_modem_samples)	(struct freedv *freedv);
int (*freedv_get_total_bits)			(struct freedv *freedv);
int (*freedv_get_total_bit_errors)		(struct freedv *freedv);
int (*freedv_get_modem_sample_rate)		(struct freedv *freedv);
int (*freedv_get_speech_sample_rate)	(struct freedv *freedv);
//
int (*freedv_get_sync)					(struct freedv *freedv);
void (*freedv_set_callback_protocol)	(struct freedv *freedv, freedv_callback_protorx rx, freedv_callback_prototx tx, void *callback_state);
void (*freedv_set_callback_data)	(struct freedv *freedv, freedv_callback_datarx datarx, freedv_callback_datatx datatx, void *callback_state);

//
// checkAvxSupport
//
// This function was copied from freedv, file fdmdv2_main.cpp and tweaked for use here
//
// Tests the underlying platform for AVX support.  2020 needs AVX support to run
// in real-time, and old processors do not offer AVX support
//

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <cpuid.h>
static int checkAvxSupport(void)
{

    int isAvxPresent = 0;
    uint32_t eax, ebx, ecx, edx;
    eax = ebx = ecx = edx = 0;
    __cpuid(1, eax, ebx, ecx, edx);

    if (ecx & (1<<27) && ecx & (1<<28)) {
        // CPU supports XSAVE and AVX
        uint32_t xcr0, xcr0_high;
        asm("xgetbv" : "=a" (xcr0), "=d" (xcr0_high) : "c" (0));
        isAvxPresent = (xcr0 & 6) == 6;    // AVX state saving enabled?
    }
	return isAvxPresent;
}
#else
static int checkAvxSupport(void)
{
    return 0;
}
#endif

/* Called when a new packet can be sent */
void my_datatx(void *callback_state, unsigned char *packet, size_t *size) {
	*size = 0;
}

static void GetAddrs(void)
{
	handle_index = 1;
	GET_HANDLE1;
	if ( ! hLib) {
		handle_index = 2;
		GET_HANDLE2;
		if ( ! hLib) {
			handle_index = 3;
			GET_HANDLE3;
			if ( ! hLib) {
				handle_index = 4;
				GET_HANDLE4;
			}
		}
	}
	if (hLib) {
		freedv_get_version = GET_ADDR("freedv_get_version");
		if (freedv_get_version != NULL) {
			freedv_version = freedv_get_version();
			if (DEBUG)
				printf("FreeDV codec2 library %d found, version %d\n", handle_index, freedv_version);
		}
		else {
			freedv_version = -2;
			if (DEBUG)
				printf("FreeDV codec2 library %d found, but no freedv_get_version() !!!\n", handle_index);
			CLOSE_LIB;
			return;
		}
	}
	else {
		handle_index = -1;
		if (DEBUG)
			printf("Could not find the FreeDV codec2 shared library\n");
		return;
	}

// open, close
	freedv_open = GET_ADDR("freedv_open");
	freedv_open_advanced = GET_ADDR("freedv_open_advanced");
	freedv_close = GET_ADDR("freedv_close");
// Transmit
	freedv_tx = GET_ADDR("freedv_tx");
	freedv_comptx = GET_ADDR("freedv_comptx");
// Receive
	freedv_nin = GET_ADDR("freedv_nin");
	freedv_rx = GET_ADDR("freedv_rx");
	freedv_floatrx = GET_ADDR("freedv_floatrx");
	freedv_comprx = GET_ADDR("freedv_comprx");
// Set parameters
	freedv_set_callback_txt = GET_ADDR("freedv_set_callback_txt");
	freedv_set_callback_protocol = GET_ADDR("freedv_set_callback_protocol");
	freedv_set_callback_data = GET_ADDR("freedv_set_callback_data");
	freedv_set_test_frames = GET_ADDR("freedv_set_test_frames");
	freedv_set_smooth_symbols = GET_ADDR("freedv_set_smooth_symbols");
	freedv_set_squelch_en = GET_ADDR("freedv_set_squelch_en");
	freedv_set_snr_squelch_thresh = GET_ADDR("freedv_set_snr_squelch_thresh");
	freedv_set_tx_bpf = GET_ADDR("freedv_set_tx_bpf");
// Get parameters
	freedv_get_modem_stats = GET_ADDR("freedv_get_modem_stats");
	freedv_get_test_frames = GET_ADDR("freedv_get_test_frames");
	freedv_get_n_speech_samples = GET_ADDR("freedv_get_n_speech_samples");
	freedv_get_n_max_modem_samples = GET_ADDR("freedv_get_n_max_modem_samples");
	freedv_get_n_nom_modem_samples = GET_ADDR("freedv_get_n_nom_modem_samples");
	freedv_get_total_bits = GET_ADDR("freedv_get_total_bits");
	freedv_get_total_bit_errors = GET_ADDR("freedv_get_total_bit_errors");
	freedv_get_sync = GET_ADDR("freedv_get_sync");		// requires version 11
	if( freedv_version >= 11)
		freedv_get_modem_sample_rate = GET_ADDR("freedv_get_modem_sample_rate");		// requires version 11 or later
	if (freedv_version >= 12) 
		freedv_get_speech_sample_rate = GET_ADDR("freedv_get_speech_sample_rate");		// requires version 12
	return;
}

static int quisk_freedv_rx(complex double * cSamples, double * dsamples, int count, int bank)	// Called from the sound thread.
{	// Input digital modulation is cSamples; decoded voice is dsamples.  Each "bank" is a stream of audio.
	// caller will have decimated input audio down to n_modem_sample_rate and reduced block size to match reduction
	// output data will be returned at n_speech_sample_rate with corresponding block sizes
	int i, nout, need, have, sync;
	int n_speech_samples;
	complex double cx;
	double scale = (double)CLIP32 / CLIP16;	// convert 32 bits to 16 bits
	struct freedv * hF;
	struct _rx_channel * pCh;
	int nCountOut = 1;	// In case we have a higher (or lower) value for o/p count

	if (cSamples == NULL) {		// shutdown
		for (i = 0; i < MAX_RECEIVERS; i++) {
			if (rx_channel[i].demod_in) {
				free(rx_channel[i].demod_in);
				rx_channel[i].demod_in = NULL;
			}
		}
		return 0;
	}

	if (bank < 0 || bank >= MAX_RECEIVERS)
		return 0;
	hF = rx_channel[bank].hFreedv;
	if ( ! hF)
		return 0;
	pCh = rx_channel + bank;
	n_speech_samples = freedv_get_n_speech_samples(hF);
	// if (say) input modem rate is the 2020 mode rate of 8000 sa/sec
	// and output rate is the corresponding 16000 sa/sec then for an input
	// block size of N, outbut 2N samples block size.
	// Apply a multiplier/divisor to 'count' for the output samples count
	if(n_speech_sample_rate >= n_modem_sample_rate ) {
		int nRatio = n_speech_sample_rate / n_modem_sample_rate;
		if( nRatio >= 1 && nRatio <= 6)
			nCountOut = nRatio * count;
	}
	else {
		int nRatio = n_modem_sample_rate / n_speech_sample_rate;
		if( nRatio >= 1 && nRatio <= 6)
			nCountOut = count / nRatio;
	}
	nout = 0;
	need = freedv_nin(hF);
	for (i = 0; i < count; i++) {
		cx = cRxFilterOut(cSamples[i], bank, 0);
		if (rxMode == FDV_L)		// lower sideband
			cx = conj(cx);
#if 1 // Try it the other way and remove scaling factor
		pCh->demod_in[pCh->rxdata_index].real = creal(cx);
		pCh->demod_in[pCh->rxdata_index].imag = cimag(cx);
#else
		pCh->demod_in[pCh->rxdata_index].real = (creal(cx) - cimag(cx));
		pCh->demod_in[pCh->rxdata_index].imag = 0;
#endif
		pCh->rxdata_index++;
		if (pCh->rxdata_index >= need) {
			if (pCh->speech_available + n_speech_samples < SPEECH_BUF_SIZE) {		// check for buffer space
				have = freedv_comprx(hF, pCh->speech_out + pCh->speech_available, pCh->demod_in);
				if (freedv_version > 10)
					sync = freedv_get_sync(hF);
				else
					freedv_get_modem_stats(hF, &sync, NULL);
				if (freedv_current_mode == 0) {		// mode 1600
					if (sync)		// throw away speech if not in sync
						pCh->speech_available += have;
				}
				else if (pCh->speech_available < SPEECH_BUF_SIZE * 2 / 3) {
					pCh->speech_available += have;	// keep speech if there is space
				}
				else {
					if (DEBUG) printf("Close to maximum in speech output buffer\n");
				}
			}
			else {		// no space in buffer
				if (DEBUG) printf("Overflow in speech output buffer\n");
			}
			pCh->rxdata_index = 0;
			need = freedv_nin(hF);
		}
	}
	if ( ! pCh->playing) {
		if (pCh->speech_available >= 2 * n_speech_samples) {
			pCh->playing = 1;
		}
		else {		// return zero samples
			for (i = 0; i < nCountOut; i++)
				dsamples[i] = 0;
			//if (DEBUG) printf("Rx buffer playing %d available %d\n", pCh->playing, pCh->speech_available);
			return nCountOut;
		}
	}
	for (nout = 0; nout < pCh->speech_available && nout < nCountOut; nout++)
		dsamples[nout] = pCh->speech_out[nout] * scale * 0.7;
	if (nout) {
		pCh->speech_available -= nout;
		memmove(pCh->speech_out, pCh->speech_out + nout, (pCh->speech_available) * sizeof(short));
	}
	if ( ! pCh->speech_available) {
		pCh->playing = 0;
		while (nout < nCountOut)
			dsamples[nout++] = 0;
	}
	//if (DEBUG) printf("Rx buffer playing %d available %d\n", pCh->playing, pCh->speech_available);
	return nout;
}

static int quisk_freedv_tx(complex double * cSamples, double * dsamples, int count)	// Called from the sound thread.
{	// Input voice samples are dsamples; output digital modulation is cSamples.
	int i, nout;
	int n_speech_samples;
	int n_nom_modem_samples;
	static COMP * mod_out = NULL;
	static short * speech_in = NULL;
	static int speech_index=0, mod_index=0;
	// g8kbb - add for 800XA
	static short *real_mod_out = NULL;

	if (dsamples == NULL) {		// shutdown
		if (mod_out)
			free(mod_out);
		mod_out = NULL;
		if (real_mod_out)
			free(real_mod_out);
		real_mod_out = NULL;
		if (speech_in)
			free(speech_in);
		speech_in = NULL;
		return 0;
	}
	if ( ! rx_channel[0].hFreedv)
		return 0;
	n_speech_samples = freedv_get_n_speech_samples(rx_channel[0].hFreedv);
	n_nom_modem_samples = freedv_get_n_nom_modem_samples(rx_channel[0].hFreedv);
	// find if need to output at faster rate
	int nRatio = n_modem_sample_rate / n_speech_sample_rate;
	if( nRatio < 1 || nRatio > 6 )
		nRatio = 1;
	if (mod_out == NULL) {		// initialize
		mod_out = (COMP *)malloc(sizeof(COMP) * n_nom_modem_samples);
		memset(mod_out, 0, sizeof(COMP) * n_nom_modem_samples);
		speech_in = (short*)malloc(sizeof(short) * n_speech_samples);
		speech_index=0;
		mod_index=0;
		real_mod_out = (short *)malloc(sizeof(short) * n_nom_modem_samples);
		memset(real_mod_out, 0, sizeof(short) * n_nom_modem_samples);
		//if( DEBUG ) printf("Initialise tx buffer size = %d, i/p sample size = %d\n", n_nom_modem_samples, n_speech_samples );
	}
	nout = 0;
	for (i = 0; i < count; i++) {
		speech_in[speech_index++] = (short)dsamples[i];
		if (speech_index >= n_speech_samples) {
			// Calculate a new block, but first write out the rest of the old block
			// 800xa uses real not complex
			if( freedv_current_mode == FREEDV_MODE_800XA ) {
				for ( ; mod_index < n_nom_modem_samples; mod_index++)
					cSamples[nout++] = real_mod_out[mod_index];
				freedv_tx(rx_channel[0].hFreedv, real_mod_out, speech_in);
			}
			else {
				for ( ; mod_index < n_nom_modem_samples; mod_index++)
					cSamples[nout++] = mod_out[mod_index].real + I * mod_out[mod_index].imag;
				freedv_comptx(rx_channel[0].hFreedv, mod_out, speech_in);
			}
			//if( DEBUG ) printf("Block processed, speech index=%d, n_speech_samples=%d, count=%d\n", speech_index, n_speech_samples, count );
			mod_index = 0;
			speech_index = 0;
		}
		else {		// write out samples
			for( int j=0; j<nRatio; j++) {
				if (mod_index < n_nom_modem_samples ) {
					if( freedv_current_mode == FREEDV_MODE_800XA ) {
						cSamples[nout++] = real_mod_out[mod_index];
					}
					else {
						cSamples[nout++] = mod_out[mod_index].real + I * mod_out[mod_index].imag;
					}
					mod_index++;
				}
			}
		}
	}
	if (rxMode == FDV_L)
		for (i = 0; i < nout; i++)
			cSamples[i] = conj(cSamples[i]);
	return nout;
}

#define TX_MSG_SIZE		80
static char quisk_tx_msg[TX_MSG_SIZE];

static char get_next_tx_char(void * callback_state)
{
	char c;
	static int index = 0;

	c = quisk_tx_msg[index++];
	if (index >= TX_MSG_SIZE)
		index = 0;
	if ( ! c) {
		index = 0;
		c = quisk_tx_msg[index++];
	}
	return c;
}

#define RX_MSG_SIZE		80
static char quisk_rx_msg[RX_MSG_SIZE + 1];

static void put_next_rx_char(void * callback_state, char ch)
{
	if (ch == '\n' || ch == '\r')
		ch = ' ';
	if (ch < 32 || ch > 126)	// printable characters
		return;
	if (strlen(quisk_rx_msg) < RX_MSG_SIZE)
		strncat(quisk_rx_msg, &ch, 1);
}

PyObject * quisk_freedv_get_rx_char(PyObject * self, PyObject * args)	// Called from the GUI thread.
{
	PyObject * txt;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	txt = PyString_FromString(quisk_rx_msg);
	quisk_rx_msg[0] = 0;
	return txt;
}

static void CloseFreedv(void)	// Called from the GUI thread or sound thread
{
	int i;

	for (i = 0; i < MAX_RECEIVERS; i++) {
		if (rx_channel[i].hFreedv) {
			freedv_close(rx_channel[i].hFreedv);
			rx_channel[i].hFreedv = NULL;
		}
#if 0 // g8kbb - unnecessary - quisk_freedv_rx() will do it
		if (rx_channel[i].demod_in) {
			free(rx_channel[i].demod_in);
			rx_channel[i].demod_in = NULL;
		}
#endif
	}
	quisk_freedv_rx(NULL, NULL, 0, 0);
	quisk_freedv_tx(NULL, NULL, 0);
	freedv_current_mode = -1;
}

static int OpenFreedv(void)	// Called from the GUI thread or sound thread
{
	int i, n_max_modem_samples;
	struct freedv * hF;

	if ( ! hLib)
		GetAddrs();		// Get the entry points for funtions in the codec2 library
	if (DEBUG) {	
		switch (handle_index) {
		case 1:
			printf("freedv_open: system codec2 library found, version %d\n", freedv_version);
			break;
		case 2:
			printf("freedv_open: codec2 library freedvpkg/libcodec2.dll|so found, version %d\n", freedv_version);
			break;
		case 3:
		case 4:
			printf("freedv_open: codec2 library freedvpkg/libcodec2_32|64.dll|so found, version %d\n", freedv_version);
			break;
		default:
			printf("freedv_open: Could not find the FreeDV codec2 shared library\n");
			break;
		}
	}
	if (freedv_version < 10) {
		CloseFreedv();
		requested_mode = -1;
		if (DEBUG)
			printf("freedv_open: Failure because version is less than 10\n");
		return 0;	// failure
	}
	if (requested_mode == FREEDV_MODE_2020) {
		if (checkAvxSupport() == 0) {
			CloseFreedv();
			requested_mode = -1;
			if (DEBUG)
				printf("freedv_open: Failure because mode 2020 requires Avx support\n");
			return 0;	// failure
		}
		if (handle_index >= 3) {	// Quisk provided codec2
			CloseFreedv();
			requested_mode = -1;
			if (DEBUG)
				printf("freedv_open: Failure because mode 2020 requires a system installation of codec2\n");
			return 0;	// failure
		}
	}
	if (requested_mode == FREEDV_MODE_700E) {
		if (handle_index >= 3) {	// Quisk provided codec2
			CloseFreedv();
			requested_mode = -1;
			if (DEBUG)
				printf("freedv_open: Failure because mode 700E requires a system installation of codec2\n");
			return 0;	// failure
		}
	}

   	if ((requested_mode == FREEDV_MODE_700D || requested_mode == FREEDV_MODE_700E) && freedv_open_advanced) {
       	struct freedv_advanced adv;
       	adv.interleave_frames = interleave_frames;
       	hF = freedv_open_advanced(requested_mode, &adv);
   	}
	else {
        	hF = freedv_open(requested_mode);
	}
	if (hF == NULL) {
		CloseFreedv();
		requested_mode = -1;
		return 0;	// failure
	}
	rx_channel[0].hFreedv = hF;
	quisk_dvoice_freedv(&quisk_freedv_rx, &quisk_freedv_tx);
	if (quisk_tx_msg[0])
		freedv_set_callback_txt(hF, &put_next_rx_char, &get_next_tx_char, NULL);
	else
		freedv_set_callback_txt(hF, &put_next_rx_char, NULL, NULL);
	if (freedv_set_callback_protocol)
 		freedv_set_callback_protocol(hF, NULL, NULL, NULL);
	if (freedv_set_callback_data)
		freedv_set_callback_data(hF, NULL, my_datatx, NULL);
	freedv_set_squelch_en(hF, quisk_freedv_squelch);
	if (freedv_set_tx_bpf)
		freedv_set_tx_bpf(hF, quisk_set_tx_bpf);
	n_max_modem_samples = freedv_get_n_max_modem_samples(hF);
	n_speech_sample_rate = 8000;
	if (freedv_version >= 12 && freedv_get_speech_sample_rate)
		n_speech_sample_rate = freedv_get_speech_sample_rate(hF);
	n_modem_sample_rate = 8000;
	if (freedv_version >= 11 && freedv_get_modem_sample_rate)
		n_modem_sample_rate = freedv_get_modem_sample_rate(hF);
	for (i = 0; i < MAX_RECEIVERS; i++) {
		rx_channel[i].rxdata_index = 0;
		rx_channel[i].speech_available = 0;
		rx_channel[i].playing = 0;
		if (rx_channel[i].demod_in)
			free(rx_channel[i].demod_in);
		rx_channel[i].demod_in = (COMP *)malloc(sizeof(COMP) * n_max_modem_samples);
		if( rx_channel[i].demod_in == NULL) {
			CloseFreedv();
			requested_mode = -1;
			return 0; // failure
		}
		if (i > 0) {
			rx_channel[i].hFreedv = freedv_open(requested_mode);
			if (rx_channel[i].hFreedv)
				freedv_set_squelch_en(rx_channel[i].hFreedv, quisk_freedv_squelch);
		}
	}
	if (DEBUG) printf("n_nom_modem_samples %d\n", freedv_get_n_nom_modem_samples(rx_channel[0].hFreedv));
	if (DEBUG) printf("n_speech_samples %d\n", freedv_get_n_speech_samples(rx_channel[0].hFreedv));
	if (DEBUG) printf("n_max_modem_samples %d\n", n_max_modem_samples);
	if (DEBUG) {
	 	if (freedv_version >= 11) printf("modem_sample_rate %d\n", n_modem_sample_rate);
		if (freedv_version >= 12) printf("speech_sample_rate %d\n", n_speech_sample_rate);
	}
	freedv_current_mode = requested_mode;
	return 1;		// success
}

void quisk_check_freedv_mode(void)
{	// see if we need to change the mode
	if (requested_mode == freedv_current_mode)
		return;
	if (DEBUG) printf("Change in mode to %d\n", requested_mode);
	CloseFreedv();
	if (requested_mode >= 0)
		OpenFreedv();
	else
		requested_mode = -1;
}

PyObject * quisk_freedv_open(PyObject * self, PyObject * args)	// Called from the GUI thread before freedv is open
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return PyInt_FromLong(OpenFreedv());
}

PyObject * quisk_freedv_close(PyObject * self, PyObject * args)	// Called from the GUI thread.
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	requested_mode = -1;		// request close
	Py_INCREF (Py_None);
	return Py_None;
}

PyObject * quisk_freedv_set_options(PyObject * self, PyObject * args, PyObject * keywds)	// Called from the GUI thread.
{  // Call with keyword arguments ONLY to change parameters.  Call before quisk_freedv_open() to set an initial mode.
	int mode=-1;				// Call again to change the mode.
	int bpf=-1;
	char * ptMsg=NULL;
	static char * kwlist[] = {"mode", "tx_msg", "DEBUG", "squelch", "interleave_frames", "set_tx_bpf", NULL} ;

	if (!PyArg_ParseTupleAndKeywords (args, keywds, "|isiiii", kwlist, &mode, &ptMsg, &DEBUG, &quisk_freedv_squelch, &interleave_frames, &bpf))
		return NULL;
	if (ptMsg)
		strMcpy(quisk_tx_msg, ptMsg, TX_MSG_SIZE);
	if (bpf != -1) {
		quisk_set_tx_bpf = bpf;
		if (freedv_set_tx_bpf && rx_channel[0].hFreedv)
			freedv_set_tx_bpf(rx_channel[0].hFreedv, quisk_set_tx_bpf);
	}
	if (mode == -1) {	// mode was not entered
		;
	}
	else if (mode == FREEDV_MODE_700E && freedv_version >= 14) {
		requested_mode = mode;
	}
	else if (mode == FREEDV_MODE_2020) {
		if (checkAvxSupport() && handle_index <= 2 && freedv_version >= 13)
			requested_mode = mode;
	}
	else if (freedv_version == 10) {
		if (mode == 0)
			requested_mode = mode;
	}
	else if (freedv_version == 11) {
		if (mode <= 2)
			requested_mode = mode;
	}
	else {
		requested_mode = mode;
	}
	return PyInt_FromLong(requested_mode);	// Return the mode
}

PyObject * quisk_freedv_get_snr(PyObject * self, PyObject * args)	// Called from the GUI thread.
{
	float snr_est = 0.0;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	if (rx_channel[0].hFreedv)
		freedv_get_modem_stats(rx_channel[0].hFreedv, NULL, &snr_est);
	return PyFloat_FromDouble(snr_est);
}

PyObject * quisk_freedv_get_version(PyObject * self, PyObject * args)	// Called from the GUI thread.
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	if ( ! hLib)
		GetAddrs();		// Get the entry points for funtions in the codec2 library
	return PyInt_FromLong(freedv_version);
}

PyObject * quisk_freedv_set_squelch_en(PyObject * self, PyObject * args)	// Called from the GUI thread.
{
	int i,state;

	if (!PyArg_ParseTuple (args, "i", &state))
		return NULL;
	quisk_freedv_squelch = state;
	for (i = 0; i < MAX_RECEIVERS; i++) {
		if( rx_channel[i].hFreedv )
			freedv_set_squelch_en(rx_channel[i].hFreedv, state);
	}
	return PyInt_FromLong(state);
}


