#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdlib.h>
#include <math.h>
#include <complex.h>	// Use native C99 complex type for fftw3
#include <fftw3.h>
#include <sys/types.h>
#include <stdbool.h>

#ifdef MS_WINDOWS
#include <winsock2.h>
#include <iphlpapi.h>
static int cleanupWSA = 0;		// Must we call WSACleanup() ?
HWND quisk_mainwin_handle;		// Handle of the main window on Windows
#include <avrt.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#endif

///static HANDLE CWkey_mutex;
///CWkey_mutex = CreateMutex(NULL, FALSE, NULL);
///if ( ! CWkey_mutex)
///	return;
///while (WaitForSingleObject(CWkey_mutex, 0) == WAIT_TIMEOUT) ;
///#include <pthread.h>
///static pthread_mutex_t CWkey_mutex = PTHREAD_MUTEX_INITIALIZER;
///while (pthread_mutex_trylock(&CWkey_mutex) == EBUSY) ;

#include "quisk.h"
#include "filter.h"
#include <stdint.h>

#ifdef MS_WINDOWS
CRITICAL_SECTION QuiskCriticalSection;
#endif

#define DEBUG		0

// These are used for input/output of radio samples from/to a file.  The SAMPLES_FROM_FILE is 0 for
// normal operation, 1 to record samples to a file, 2 to play samples from a file.  Rate must be 48k.
#define SAMPLES_FROM_FILE	0

#define FM_FILTER_DEMPH		300.0				// Frequency of FM lowpass de-emphasis filter
#define AGC_DELAY			15					// Delay in AGC buffer in milliseconds
#define FFT_ARRAY_SIZE		4					// Number of FFTs
#define MULTIRX_FFT_MULT	8					// multirx FFT size is a multiple of graph size
#define MAX_RX_CHANNELS		3					// maximum paths to decode audio
#define MAX_RX_FILTERS		3					// maximum number of receiver filters
#define DGT_NARROW_FREQ		3000				// Use 6 ksps rate below this bandwidth
#define SQUELCH_FFT_SIZE	512

static int fft_error;			// fft error count
typedef struct fftd {
	fftw_complex * samples;		// complex data for fft
	int index;					// position of next fft sample
	int filled;					// whether the fft is ready to run
	int block;					// block number 0, 1, ...
} fft_data;

typedef struct mrx_fftd {		// FFT data for sub-receivers
	fftw_complex * samples;		// complex data for fft of size multirx_fft_width
	int index;					// position of next fft sample
} mrx_fft_data;

struct AgcState {		// Store state information for the AGC
	double max_out;		// Must initialize to maximum output level 0.0 to 1.0.
	int sample_rate;	// Must initialize this to the sample rate or zero.
	int buf_size;		// Must initialize this to zero.
	int index_read;
	int index_start;
	int is_clipping;
	double themax;
	double gain;
	double delta;
	double target_gain;
	double time_release;
	complex double * c_samp;
};

struct watfall_row_t {
	int x_origin;
	struct watfall_row_t * next_row;
	struct watfall_row_t * prior_row;
	uint8_t pixels[2];	// extend to size
} ;

struct watfall_t {
	uint8_t red[256];
	uint8_t green[256];
	uint8_t blue[256];
	int width;
	int max_height;
	struct watfall_row_t * current_row;
} ;

static fft_data fft_data_array[FFT_ARRAY_SIZE];		// Data for several FFTs
static int fft_data_index = 0;						// Write the current samples to this FFT
static fftw_plan quisk_fft_plan;
static char fftw_wisdom_name[QUISK_SC_SIZE];		// wisdom patch provided by Eoin Mcloughlin, EI7HSB
static double * fft_window;		// Window for FFT data
static double * current_graph;	// current graph data as returned

static PyObject * QuiskError;		// Exception for this module
static PyObject * pyApp;		// Application instance
static int fft_size;			// size of fft, e.g. 1024
int data_width;				// number of points to return as graph data; fft_size * n
static int graph_width;			// width of the graph in pixels
rx_mode_type rxMode;			// 0 to 13: CWL, CWU, LSB, USB, AM, FM, EXT, DGT_U, DGT_L, DGT_IQ, IMD, FDV_U, FDV_L, DGT_FM
int quisk_noise_blanker;		// noise blanker level, 0 for off
int quiskTxHoldState;			// hold Tx until the repeater frequency shift is complete
int quisk_is_vna;			// zero for normal program, one for the VNA program
static int py_sample_rx_bytes=2;	// number of bytes in each I or Q sample: 1, 2, 3, or 4
static int py_sample_rx_endian;		// order of sample array: 0 == little endian; 1 == big endian
static int py_bscope_bytes;
static int py_bscope_endian;
static int quisk_auto_notch;	// auto notch control
PyObject * quisk_pyConfig=NULL;	// Configuration module instance
static int graphX;			// Origin of first X value for graph data
static int graphY;			// Origin of 0 dB for graph data
static double graphScale;		// Scale factor for graph
static complex double testtonePhase;	// Phase increment for test tone
double quisk_audioVolume;		// Audio output level, 0.0 to 1.0
double quisk_ctcss_freq;		// CTCSS repeater access tone frequency in Hertz, or zero
static double cFilterI[MAX_RX_FILTERS][MAX_FILTER_SIZE];	// Digital filter coefficients for receivers
static double cFilterQ[MAX_RX_FILTERS][MAX_FILTER_SIZE];	// Digital filter coefficients
static int sizeFilter;			// Number of coefficients for filters
int quisk_isFDX;			// Are we in full duplex mode?
static int filter_bandwidth[MAX_RX_FILTERS];		// Current filter bandwidth in Hertz
static int filter_start_offset; 	// Current filter +/- start offset frequency from rx_tune_freq in Hertz for filter zero
static int quisk_decim_srate;				// Sample rate after decimation
static int quisk_filter_srate=48000;		// Frequency for filters
static int split_rxtx;						// Are we in split rx/tx mode?
static int kill_audio;					// Replace radio sound with silence
static int quisk_transmit_mode;			// Set transmit mode. No hang time on release.
static int fft_sample_rate;				// Sample rate on the graph (not the audio channel) -fft_srate/2 < freq < +fft_srate/2
static int scan_blocks=0;			// Number of FFT blocks for scan; or zero
static int scan_sample_rate=1;      // Sample rate to use while scanning
static double scan_valid=0.84;		// Fraction of each FFT block that is valid
static int scan_vfo0;
static int scan_deltaf;
static int graph_refresh;				// Graph refresh rate from the config file
static int multiple_sample_rates=0;     // Hardware supports multiple different sample rates
static int vfo_screen;                  // The VFO (center) frequency on the FFT screen
static int vfo_audio;                   // VFO frequency for the audio channel
static int is_PTT_down;					// state 0/1 of PTT button
static int sample_bytes=3;				// number of bytes in each I or Q sample
static int waterfall_scroll_mode = 1;		// draw the first lines multiple times
static int quisk_use_sidetone;		// is there a sidetone volume control?
static int hl2_txbuf_errors;		// errors in the Hermes-Lite2 Tx buffer
static int hl2_txbuf_state;		// state machine for errors in the Hermes-Lite2 Tx buffer
static int freedv_monitor;		// pass the Freedv audio to the speakers instead of to the demodulator

static complex double PySampleBuf[SAMP_BUFFER_SIZE];	// buffer for samples returned from Python
static int PySampleCount;				// count of samples in buffer
static bool quisk_tx_inhibit;

static int multirx_data_width;			// width of graph data to return
static int multirx_fft_width;			// size of FFT samples
int quisk_multirx_count;			// number of additional receivers zero or 1, 2, 3, ...
static int quisk_multirx_state;			// state of hermes receivers
static mrx_fft_data multirx_fft_data[QUISK_MAX_SUB_RECEIVERS];		// FFT data for the sub-receivers
static int multirx_fft_next_index;								// index of the receiver for the next FFT to return
static double multirx_fft_next_time;							// timing interval for multirx FFT
static int multirx_fft_next_state;								// state of multirx FFT: 0 == filling, 1 == ready, 2 == done
static fftw_plan multirx_fft_next_plan;							// fftw3 plan for multirx FFTs
static fftw_complex * multirx_fft_next_samples;					// sample buffer for multirx FFT
static int multirx_play_method;			// 0== both, 1==left, 2==right
static int multirx_play_channel = -1;	// index of the channel to play; or -1
static int multirx_freq[QUISK_MAX_SUB_RECEIVERS];			// tune frequency for channel
static int multirx_mode[QUISK_MAX_SUB_RECEIVERS];			// mode CW, SSB, etc. for channel

static complex double * multirx_cSamples[QUISK_MAX_SUB_RECEIVERS];		// samples for the sub-receivers
static int multirx_sample_size;					// current size of the sub-receiver sample array

double quisk_sidetoneVolume;		// Audio output level of the CW sidetone, 0.0 to 1.0
static complex double sidetonePhase;	// Phase increment for sidetone
int quisk_sidetoneCtrl;			// sidetone control value 0 to 1000
int quisk_sidetoneFreq;			// frequency in hertz for the sidetone
int quisk_start_cw_delay = 15;		// milliseconds to delay output on CW key down
int quisk_start_ssb_delay = 100;	// milliseconds to discard output on SSB etc. key down
static int maximum_tx_secs;		// Failsafe timeout for Tx in seconds
static int TxRxSilenceMsec = 50;	// Play silence in msec on Tx to Rx
static int key_is_down = 0;		// internal key state up or down

static double agcReleaseGain=80;		// AGC maximum gain
static double agc_release_time = 1.0;	// Release time in seconds
static double squelch_level=-999.0;	// setting of FM squelch control
static int ssb_squelch_enabled;
static int ssb_squelch_level;
static int quisk_invert_spectrum = 0;	// Invert the input RF spectrum
static void process_agc(struct AgcState *, complex double *, int, int);

static double Smeter;			// Measured RMS signal strength
static int rx_tune_freq;		// Receive tuning frequency as +/- sample_rate / 2, including RIT
int quisk_tx_tune_freq;			// Transmit tuning frequency as +/- sample_rate / 2
static int rit_freq;			// RIT frequency in Hertz

#define RX_UDP_SIZE		1442		// Expected size of UDP samples packet
static SOCKET rx_udp_socket = INVALID_SOCKET;		// Socket for receiving ADC samples from UDP
int quisk_rx_udp_started = 0;		// Have we received any data yet?
int quisk_using_udp = 0;			// Are we using rx_udp_socket?  No longer used, but provided for backward compatibility.
static double rx_udp_gain_correct = 0;		// Small correction for different decimation rates
static double rx_udp_clock;			// Clock frequency for UDP samples
int quisk_use_rx_udp;						// from the config file
play_state_t quisk_play_state;

static int is_little_endian;		// Test byte order; is it little-endian?
unsigned char quisk_pc_to_hermes[17 * 4];			// data to send from PC to Hermes hardware
unsigned char quisk_hermeslite_writequeue[5]; // One-time writes to Hermes-Lite
unsigned int quisk_hermeslite_writepointer = 0;
static unsigned char quisk_hermes_to_pc[5 * 4];		// data received from the Hermes hardware
static unsigned char quisk_hermeslite_response[5]; // response from Hermes-Lite commands
unsigned int quisk_hermes_code_version = -1;		// code version returned by the Hermes hardware
unsigned int quisk_hermes_board_id = -1;		// board ID returned by the Hermes hardware
static double hermes_temperature;	// average temperature
static double hermes_fwd_power;		// average forward power
static double hermes_rev_power;		// average reverse power
static double hermes_fwd_peak;		// peak forward power
static double hermes_rev_peak;		// peak reverse power
static double hermes_pa_current;	// average power amp current
static int hermes_count_temperature;	// number of temperature samples
static int hermes_count_current;	// number of current samples
static int hardware_ptt;		// hardware PTT switch

int quisk_hardware_cwkey;		// hardware CW key from UDP or USB
static int old_hardware_cwkey;		// previous hardware CW key
int quisk_remote_cwkey;			// remote CW key (sent from control head)
static int old_remote_cwkey;		// previous remote CW key

enum quisk_rec_state quisk_record_state = IDLE;
static float * quisk_record_buffer;
static int quisk_record_bufsize;
static int quisk_record_index;
static int quisk_play_index;
static int quisk_mic_index;
static int quisk_record_full;

// These are used to measure the frequency of a continuous RF signal.
static void measure_freq(complex double *, int, int);
static double measured_frequency;
static int measure_freq_mode=0;

//These are used to measure the demodulated audio voltage
static double measured_audio;
static double measure_audio_sum;
static int measure_audio_count;
static int measure_audio_time=1;

// This is used to measure the squelch level
static struct  _MeasureSquelch {
	int squelch_active;
	// These are used for FM squelch
	double rf_sum;
	double squelch;
	int rf_count;
	// These are used for SSB squelch
	double * in_fft;
	int index;
	int sq_open;
} MeasureSquelch[MAX_RX_CHANNELS];

// These are used for playback of a WAV file.
static int wavStart;			// Sound data starts at this offset
// Open two WAV files with the same name. Two wavFp are needed because the same file is used on asynchronous streams.
static FILE * wavFpSound;		// File pointer to play the Audio WAV file or Samples WAV file
static FILE * wavFpMic;			// File pointer to play the same Audio WAV file replacing the Microphone sound
int quisk_close_file_play;

// These are used for bandscope data from Hermes
static int enable_bandscope = 1;
static fftw_plan bandscopePlan=NULL;
static unsigned int bandscopeState = 0;
static unsigned int bandscopeBlockCount = 4;
static int bandscope_size = 0;
static double * bandscopeSamples = NULL;	// bandscope samples are normalized to max 1.0 with bandscopeScale
static double bandscopeScale = 32768;		// maximum value of the samples sent to the bandscope
static double * bandscopeWindow = NULL;
static double * bandscopeAverage = NULL;
static double * bandscopePixels = NULL;
static complex double * bandscopeFFT = NULL;
static double hermes_adc_level = 0.0;		// maximum bandscope sample from the ADC, 0.0 to 1.0

#if SAMPLES_FROM_FILE
static struct QuiskWav hWav;

int QuiskWavWriteOpen(struct QuiskWav * hWav, char * file_name, short format, short nChan, short bytes, int rate, double scale)
{
	unsigned int u;		// must be 4 bytes
	unsigned short s;	// must be 2 bytes

    hWav->format = format;
    hWav->nChan = nChan;
    hWav->bytes_per_sample = bytes;
    hWav->sample_rate = rate;
    hWav->scale = scale;
	hWav->samples = 0;    // number of samples written
	hWav->fp = fopen(file_name, "wb");
	if ( ! hWav->fp)
		return 0;
	if (format == 0)	// RAW format - no header
		return 1;
	if (fwrite("RIFF", 1, 4, hWav->fp) != 4) {
		fclose(hWav->fp);
		hWav->fp = NULL;
		return 0;
	}
    if (format == 1)    // PCM
	    u = 36;
    else
	    u = 50;
	fwrite(&u, 4, 1, hWav->fp);
	fwrite("WAVE", 1, 4, hWav->fp);
	fwrite("fmt ", 1, 4, hWav->fp);
    if (format == 1)    // PCM
	    u = 16;
    else
	    u = 18;
	fwrite(&u, 4, 1, hWav->fp);
	fwrite(&format, 2, 1, hWav->fp);        // format
	fwrite(&nChan, 2, 1, hWav->fp);         // number of channels
	fwrite(&rate, 4, 1, hWav->fp);          // sample rate
	u = rate * bytes * nChan;
	fwrite(&u, 4, 1, hWav->fp);
	s = bytes * nChan; 
	fwrite(&s, 2, 1, hWav->fp);
    s = bytes * 8;
	fwrite(&s, 2, 1, hWav->fp);
    if (format != 1) {
	    s = 0; 
	    fwrite(&s, 2, 1, hWav->fp);
	    fwrite("fact", 1, 4, hWav->fp);
	    u = 4;
	    fwrite(&u, 4, 1, hWav->fp);
	    u = 0;
	    fwrite(&u, 4, 1, hWav->fp);
    }
	fwrite("data", 1, 4, hWav->fp);
	u = 0;
	fwrite(&u, 4, 1, hWav->fp);
    return 1;
}

void QuiskWavWriteC(struct QuiskWav * hWav, complex double * cSamples, int nSamples)
{  // Record the samples to a WAV file, two float samples I/Q.  Always use IEEE format 3.
	int j;	// TODO: add other formats
	float samp;			// must be 4 bytes

    if ( ! hWav->fp)
        return;
    // append the samples
	hWav->samples += (unsigned int)nSamples;
	fseek(hWav->fp, 0, SEEK_END);		// seek to the end
	for (j = 0; j < nSamples; j++) {
		samp = creal(cSamples[j]) * hWav->scale;
		fwrite(&samp, 4, 1, hWav->fp);
		samp = cimag(cSamples[j]) * hWav->scale;
		fwrite(&samp, 4, 1, hWav->fp);
	}
    // write the sizes to the header
    QuiskWavWriteD(hWav, NULL, 0);
}

void QuiskWavWriteD(struct QuiskWav * hWav, double * dSamples, int nSamples)
{  // Record the samples to a file, one channel.
	int j;
	float samp;			// must be 4 bytes
	unsigned int u;		// must be 4 bytes
	int i;      		// must be 4 bytes
    char c;             // must be 1 byte
    short s;            // must be 2 bytes

    if ( ! hWav->fp)
        return;
    // append the samples
	hWav->samples += (unsigned int)nSamples;
	fseek(hWav->fp, 0, SEEK_END);		// seek to the end
    if ( ! dSamples) {
        ;       // Only update the header
    }
    else if (hWav->format == 3) {        // float
    	for (j = 0; j < nSamples; j++) {
		    samp = dSamples[j] * hWav->scale;
		    fwrite(&samp, 4, 1, hWav->fp);
	    }
	}
    else {      // PCM integer
        switch (hWav->bytes_per_sample) {
        case 1:
    	    for (j = 0; j < nSamples; j++) {
                c = (char)(dSamples[j] * hWav->scale);
                fwrite(&c, 1, 1, hWav->fp);
            }
            break;
        case 2:
    	    for (j = 0; j < nSamples; j++) {
                s = (short)(dSamples[j] * hWav->scale);
                fwrite(&s, 2, 1, hWav->fp);
            }
            break;
        case 4:
    	    for (j = 0; j < nSamples; j++) {
                i = (int)(dSamples[j] * hWav->scale);
                fwrite(&i, 4, 1, hWav->fp);
            }
            break;
        }
    }
    // write the sizes to the header
    if (hWav->format == 0) {        // RAW format
		;
	}
    else if (hWav->format == 3) {        // float
	    fseek(hWav->fp, 54, SEEK_SET);	// seek from the beginning
	    u = hWav->bytes_per_sample * hWav->nChan * hWav->samples;
	    fwrite(&u, 4, 1, hWav->fp);
	    fseek(hWav->fp, 4, SEEK_SET);
	    u += 50 ;
	    fwrite(&u, 4, 1, hWav->fp);
	    fseek(hWav->fp, 46, SEEK_SET);
	    u = hWav->samples * hWav->nChan;
	    fwrite(&u, 4, 1, hWav->fp);
    }
    else {
	    fseek(hWav->fp, 40, SEEK_SET);
	    u = hWav->bytes_per_sample * hWav->nChan * hWav->samples;
	    fwrite(&u, 4, 1, hWav->fp);
	    fseek(hWav->fp, 4, SEEK_SET);
	    u += 36 ;
	    fwrite(&u, 4, 1, hWav->fp);
    }
    if (hWav->samples > 536870000)    // 2**32 / 8
		QuiskWavClose(hWav);
}

int QuiskWavReadOpen(struct QuiskWav * hWav, char * file_name, short format, short nChan, short bytes, int rate, double scale)
{	// TODO: Get parameters from the WAV file header.
	char name[5];
	int size;

    hWav->format = format;
    hWav->nChan = nChan;
    hWav->bytes_per_sample = bytes;
    hWav->sample_rate = rate;
    hWav->scale = scale;
    hWav->fp = fopen(file_name, "rb");
   	if (!hWav->fp)
    	return 0;
	if (hWav->format == 0) {	// RAW format
		fseek(hWav->fp, 0, SEEK_END);		// seek to the end
		hWav->fpEnd = ftell(hWav->fp);
		hWav->fpStart = hWav->fpPos = 0;
		return 1;
	}
   	hWav->fpEnd = 0;
   	while (1) {		// WAV format
   		if (fread (name, 4, 1, hWav->fp) != 1)
   			return 0;
   		if (fread (&size, 4, 1, hWav->fp) != 1)
   			return 0;
   		name[4] = 0;
   		//QuiskPrintf("name %s size %d\n", name, size);
   		if (!strncmp(name, "RIFF", 4))
   			fseek (hWav->fp, 4, SEEK_CUR);	// Skip "WAVE"
   		else if (!strncmp(name, "data", 4)) {	// sound data starts here
   			hWav->fpStart = ftell(hWav->fp);
   			hWav->fpEnd = hWav->fpStart + size;
			hWav->fpPos = hWav->fpStart;
   			break;
   		}
   		else	// Skip other records
   			fseek (hWav->fp, size, SEEK_CUR);
   	}
   	//QuiskPrintf("start %d  end %d\n", hWav->fpStart, hWav->fpEnd);
   	if (!hWav->fpEnd) {		// Failure to find "data" record
   		fclose(hWav->fp);
   		hWav->fp = NULL;
		return 0;
   	}
    return 1;
}

void QuiskWavReadC(struct QuiskWav * hWav, complex double * cSamples, int nSamples)
{	// Always uses format 3.  TODO: add other formats.
	int i;
    float fi, fq;
	double di, dq;

#if 0
	double noise;
	noise = 1.6E6;
	for (i = 0; i < nSamples; i++) {
		di = ((float)random() / RAND_MAX - 0.5) * noise;
		dq = ((float)random() / RAND_MAX - 0.5) * noise;
		cSamples[i] = di + I * dq;
	}
#endif
    if (hWav->fp && nSamples > 0) {
    	fseek (hWav->fp, hWav->fpPos, SEEK_SET);
    	for (i = 0; i < nSamples; i++) {
    		if (fread(&fi, 4, 1, hWav->fp) != 1)
    			break;
    		if (fread(&fq, 4, 1, hWav->fp) != 1)
    			break;
    		di = fi * hWav->scale;
    		dq = fq * hWav->scale;
    		cSamples[i] += di + I * dq;
    		hWav->fpPos += hWav->bytes_per_sample * hWav->nChan;
    		if (hWav->fpPos >= hWav->fpEnd)
    			hWav->fpPos = hWav->fpStart;
    	}
    }
}

void QuiskWavReadD(struct QuiskWav * hWav, double * dSamples, int nSamples)
{
	int j;
    float samp;
	int i;      		// must be 4 bytes
    char c;             // must be 1 byte
    short s;            // must be 2 bytes

    if (hWav->fp && nSamples > 0) {
    	fseek (hWav->fp, hWav->fpPos, SEEK_SET);
    	for (j = 0; j < nSamples; j++) {
			if (hWav->format == 3) {		// float
				if (fread(&samp, 4, 1, hWav->fp) != 1)
					return;
			}
			else {      // PCM integer
				switch (hWav->bytes_per_sample) {
				case 1:
					if (fread(&c, 1, 1, hWav->fp) != 1)
						return;
					samp = c;
					break;
				case 2:
					if (fread(&s, 2, 1, hWav->fp) != 1)
						return;
					samp = s;
					break;
				case 4:
					if (fread(&i, 4, 1, hWav->fp) != 1)
						return;
					samp = i;
					break;
				}
			}
    		dSamples[j] = samp * hWav->scale;
    		hWav->fpPos += hWav->bytes_per_sample * hWav->nChan;
    		if (hWav->fpPos >= hWav->fpEnd)
    			hWav->fpPos = hWav->fpStart;
    	}
    }
}

void QuiskWavClose(struct QuiskWav * hWav)
{
    if (hWav->fp) {
        fclose(hWav->fp);
        hWav->fp = NULL;
    }
}
#endif

// These are used for digital voice codecs
ty_dvoice_codec_rx  pt_quisk_freedv_rx;
ty_dvoice_codec_tx  pt_quisk_freedv_tx;

void quisk_dvoice_freedv(ty_dvoice_codec_rx rx, ty_dvoice_codec_tx tx)
{
	pt_quisk_freedv_rx = rx;
	pt_quisk_freedv_tx = tx;
}
#if 0
static int fFracDecim(double * dSamples, int nSamples, double fdecim)
{  // fractional decimation by fdecim > 1.0
	int i, nout;
	double xm0, xm1, xm2, xm3;
	static double dindex = 1;
	static double y0=0, y1=0, y2=0, y3=0;
	static int in=0, out=0;

	in += nSamples;
	nout = 0;
	for (i = 0; i < nSamples; i++) {
		y3 = dSamples[i];
		if (dindex < 1 || dindex >= 2.4)
			QuiskPrintf ("dindex %.5f  fdecim %.8f\n", dindex, fdecim);
		if (dindex < 2) {
#if 0
				dSamples[nout++] = (1 - (dindex - 1)) * y1 + (dindex - 1) * y2;
#else
				xm0 = dindex - 0;
				xm1 = dindex - 1;
				xm2 = dindex - 2;
				xm3 = dindex - 3;
				dSamples[nout++] = xm1 * xm2 * xm3 * y0 / -6.0 + xm0 * xm2 * xm3 * y1 / 2.0 +
					xm0 * xm1 * xm3 * y2 / -2.0 + xm0 * xm1 * xm2 * y3 / 6.0;
#endif
			out++;
			dindex += fdecim - 1;
			y0 = y1;
			y1 = y2;
			y2 = y3;
		}
		else {
			if (dindex > 2.5) QuiskPrintf ("Skip at %.2f\n", dindex);
			y0 = y1;
			y1 = y2;
			y2 = y3;
			dindex -= 1;
		}
	}
	//QuiskPrintf ("in %d out %d\n", in, out);
	return nout;
}
#endif
static int cFracDecim(complex double * cSamples, int nSamples, double fdecim)
{
// Fractional decimation of I/Q signals works poorly because it introduces aliases and birdies.
	int i, nout;
	double xm0, xm1, xm2, xm3;
	static double dindex = 1;
	static complex double c0=0, c1=0, c2=0, c3=0;
	static int in=0, out=0;

	in += nSamples;
	nout = 0;
	for (i = 0; i < nSamples; i++) {
		c3 = cSamples[i];
		if (dindex < 1 || dindex >= 2.4)
			QuiskPrintf ("dindex %.5f  fdecim %.8f\n", dindex, fdecim);
		if (dindex < 2) {
#if 0
				cSamples[nout++] = (1 - (dindex - 1)) * c1 + (dindex - 1) * c2;
#else
				xm0 = dindex - 0;
				xm1 = dindex - 1;
				xm2 = dindex - 2;
				xm3 = dindex - 3;
				cSamples[nout++] =
					(xm1 * xm2 * xm3 * c0 / -6.0 + xm0 * xm2 * xm3 * c1 / 2.0 +
					xm0 * xm1 * xm3 * c2 / -2.0 + xm0 * xm1 * xm2 * c3 / 6.0);
#endif
			out++;
			dindex += fdecim - 1;
			c0 = c1;
			c1 = c2;
			c2 = c3;
		}
		else {
			if (dindex > 2.5) QuiskPrintf ("Skip at %.2f\n", dindex);
			c0 = c1;
			c1 = c2;
			c2 = c3;
			dindex -= 1;
		}
	}
	//QuiskPrintf ("in %d out %d\n", in, out);
	return nout;
}

// Create an fftw plan; attempt to use loaded wisdom cache or update existing cache.
static fftw_plan quisk_create_or_cache_fftw_plan_dft_1d(int fft_size, fftw_complex *in, fftw_complex *out, int sign, unsigned flags)
{
	fftw_plan plan = fftw_plan_dft_1d(fft_size, in, out, sign, flags | FFTW_WISDOM_ONLY);
	if(!plan) {
		// Nothing in the wisdom file for this config; create and save new wisdom
		plan = fftw_plan_dft_1d(fft_size, in, out, sign, flags);
		fftw_export_wisdom_to_filename(fftw_wisdom_name);
	}
	return plan;
}

#define QUISK_NB_HWINDOW_SECS	500.E-6	// half-size of blanking window in seconds
static void NoiseBlanker(complex double * cSamples, int nSamples)
{
	static complex double * cSaved = NULL;
	static double  * dSaved = NULL;
	static double save_sum;
	static int save_size, hwindow_size, state, index, win_index;
	static int sample_rate = -1;
	int i, j, k, is_pulse;
	double mag, limit;
	complex double samp;
#if DEBUG
	static time_t time0 = 0;
	static int debug_count = 0;
#endif

	if (quisk_noise_blanker <= 0)
		return;
	if (quisk_sound_state.sample_rate != sample_rate) {	// Initialization
		sample_rate = quisk_sound_state.sample_rate;
		state = 0;
		index = 0;
		win_index = 0;
		save_sum = 0.0;
		hwindow_size = (int)(sample_rate * QUISK_NB_HWINDOW_SECS + 0.5);
		save_size = hwindow_size * 3;	// number of samples in the average
		i = save_size * sizeof(double);
		dSaved = (double *) realloc(dSaved, i);
		memset (dSaved, 0, i);
		i = save_size * sizeof(complex double);
		cSaved = (complex double *)realloc(cSaved, i);
		memset (cSaved, 0, i);
#if DEBUG
		QuiskPrintf ("Noise blanker: save_size %d  hwindow_size %d\n",
			save_size, hwindow_size);
#endif
	}
	switch(quisk_noise_blanker) {
	case 1:
	default:
		limit = 6.0;
		break;
	case 2:
		limit = 4.0;
		break;
	case 3:
		limit = 2.5;
		break;
	}
	for (i = 0; i < nSamples; i++) {
		// output oldest sample, save newest
		samp = cSamples[i];				// newest sample
		cSamples[i] = cSaved[index];	// oldest sample
		cSaved[index] = samp;
		// use newest sample
		mag = cabs(samp);
		save_sum -= dSaved[index];	// remove oldest sample magnitude
		dSaved[index] = mag;		// save newest sample magnitude
		save_sum += mag;			// update sum of samples
		if (mag <= save_sum / save_size * limit)	// see if we have a large pulse
			is_pulse = 0;
		else
			is_pulse = 1;
		switch (state) {
		case 0:		// Normal state
			if (is_pulse) {		// wait for a pulse
				state = 1;
				k = index;
				for (j = 0; j < hwindow_size; j++) {	// apply window to prior samples
					cSaved[k--] *= (double)j / hwindow_size;
					if (k < 0)
						k = save_size - 1;
				}
			}
			else if (win_index) {		// pulses have stopped, increase window to 1.0
				cSaved[index] *= (double)win_index / hwindow_size;
				if (++win_index >= hwindow_size)
					win_index = 0;	// no more window
			}
			break;
		case 1:		// we got a pulse
			cSaved[index] = 0;	// zero samples until the pulses stop
			if ( ! is_pulse) {
				// start raising the window, but be prepared to window another pulse
				state = 0;
				win_index = 1;
			}
			break;
		}
#if DEBUG
		if (debug_count) {
			QuiskPrintf ("%d", is_pulse);
			if (--debug_count == 0)
				QuiskPrintf ("\n");
		}
		else if (is_pulse && time(NULL) != time0) {
			time0 = time(NULL);
			debug_count = hwindow_size * 2;
			QuiskPrintf ("%d", is_pulse);
		}
#endif
		if (++index >= save_size)
			index = 0;
	}
	return;
}

#define NOTCH_DEBUG			0
#define NOTCH_DATA_SIZE					2048
#define NOTCH_FILTER_DESIGN_SIZE		NOTCH_DATA_SIZE / 4
#define NOTCH_FILTER_SIZE		(NOTCH_FILTER_DESIGN_SIZE - 1)
#define NOTCH_FILTER_FFT_SIZE	(NOTCH_FILTER_SIZE / 2 + 1)
#define NOTCH_DATA_START_SIZE	(NOTCH_FILTER_SIZE - 1)
#define NOTCH_DATA_OUTPUT_SIZE	(NOTCH_DATA_SIZE - NOTCH_DATA_START_SIZE)
#define NOTCH_FFT_SIZE			(NOTCH_DATA_SIZE / 2 + 1)
static void dAutoNotch(double * dsamples, int nSamples, int sidetone, int rate)
{
	int i, j, k, i1, i2, inp, signal, delta_sig, delta_i1, half_width;
	double d, d1, d2, avg;
	static int old1, count1, old2, count2;
	static int index;
	static fftw_plan planFwd=NULL;
	static fftw_plan planRev,fltrFwd, fltrRev;
	static double data_in[NOTCH_DATA_SIZE];
	static double data_out[NOTCH_DATA_SIZE];
	static complex double notch_fft[NOTCH_FFT_SIZE];
	static double fft_window[NOTCH_DATA_SIZE];
	static double fltr_in[NOTCH_DATA_SIZE];
	static double fltr_out[NOTCH_FILTER_DESIGN_SIZE];
	static complex double fltr_fft[NOTCH_FFT_SIZE];
	static double average_fft[NOTCH_FFT_SIZE];
	static int fltrSig;
#if NOTCH_DEBUG
	static char * txt;
	double dmax;
#endif

	if ( ! planFwd) {		// set up FFT plans
		planFwd = fftw_plan_dft_r2c_1d(NOTCH_DATA_SIZE, data_in, notch_fft, FFTW_MEASURE);
		planRev = fftw_plan_dft_c2r_1d(NOTCH_DATA_SIZE, notch_fft, data_out, FFTW_MEASURE);		// destroys notch_fft
		fltrFwd = fftw_plan_dft_r2c_1d(NOTCH_DATA_SIZE, fltr_in, fltr_fft, FFTW_MEASURE);
		fltrRev = fftw_plan_dft_c2r_1d(NOTCH_FILTER_DESIGN_SIZE, fltr_fft, fltr_out, FFTW_MEASURE);
		for (i = 0; i < NOTCH_FILTER_SIZE; i++)
			fft_window[i] = 0.50 - 0.50 * cos(2. * M_PI * i / (NOTCH_FILTER_SIZE));	// Hanning
			//fft_window[i] = 0.54 - 0.46 * cos(2. * M_PI * i / (NOTCH_FILTER_SIZE));	// Hamming
	}
	if ( ! dsamples) {		// initialize
		index = NOTCH_DATA_START_SIZE;
		fltrSig = -1;
		old1 = old2 = 0;
		count1 = count2 = -4;
		memset(data_out, 0, sizeof(double) * NOTCH_DATA_SIZE);
		memset(data_in, 0, sizeof(double) * NOTCH_DATA_SIZE);
		memset(average_fft, 0, sizeof(double) * NOTCH_FFT_SIZE);
		return;
	}
	if ( ! quisk_auto_notch)
		return;
	// index into FFT data = frequency * 2 * NOTCH_FFT_SIZE / rate
	// index into filter design = frequency * 2 * NOTCH_FILTER_FFT_SIZE / rate
	for (inp = 0; inp < nSamples; inp++) {
		data_in[index] = dsamples[inp];
		dsamples[inp] = data_out[index];
		if (++index >= NOTCH_DATA_SIZE) {	// we have a full FFT of samples
			index = NOTCH_DATA_START_SIZE;
			fftw_execute(planFwd);		// Calculate forward FFT
			// Find maximum FFT bins
			delta_sig = (300 * 2 * NOTCH_FFT_SIZE + rate / 2) / rate;	// small frequency interval
			delta_i1 = (400 * 2 * NOTCH_FFT_SIZE + rate / 2) / rate;	// small frequency interval
			if (sidetone != 0)			// For CW, accept a signal at the frequency of the RIT
				signal = (abs(sidetone) * 2 * NOTCH_FFT_SIZE + rate / 2) / rate;
			else
				signal = -999;
			avg = 1;
#if NOTCH_DEBUG
			dmax = 0;
#endif
			d1 = 0;
			i1 = 0;		// First maximum signal
			for (i = 0; i < NOTCH_FFT_SIZE; i++) {
				d = cabs(notch_fft[i]);
				avg += d;
				//average_fft[i] = 0.9 * average_fft[i] + 0.1 * d;
				average_fft[i] = 0.5 * average_fft[i] + 0.5 * d;
				if (abs(i - signal) > delta_sig && average_fft[i] > d1) {
					d1 = average_fft[i];
					i1 = i;
#if NOTCH_DEBUG
					dmax = d;
#endif
				}
			}
			if (abs(i1 - old1) < 3)		// See if the maximum bin i1 is changing
				count1++;
			else
				count1--;
			if (count1 > 4)
				count1 = 4;
			else if (count1 < -1)
				count1 = -1;
			if (count1 < 0)
				old1 = i1;
			avg /= NOTCH_FFT_SIZE;
			d2 = 0;
			i2 = 0;		// Next maximum signal not near the first
			for (i = 0; i < NOTCH_FFT_SIZE; i++) {
				if (abs(i - signal) > delta_sig && abs(i - i1) > delta_i1 && average_fft[i] > d2) {
					d2 = average_fft[i];
					i2 = i;
				}
			}
			if (abs(i2 - old2) < 3)		// See if the maximum bin i2 is changing
				count2++;
			else
				count2--;
			if (count2 > 4)
				count2 = 4;
			else if (count2 < -2)
				count2 = -2;
			if (count2 < 0)
				old2 = i2;

			if (count1 > 0 && count2 > 0)
				k = i1 + 10000 * i2;	// trial filter index
			else if(count1 > 0)
				k = i1;
			else
				k = 0;
			// Make the filter if it is different
			if (fltrSig != k) {
				fltrSig = k;
				half_width = (100 * 2 * NOTCH_FILTER_FFT_SIZE + rate / 2) / rate;	// half the width of the notch
				if (half_width < 3)
					half_width = 3;
				for (i = 0; i < NOTCH_FILTER_FFT_SIZE; i++)
					fltr_fft[i] = 1.0;
				k = (i1 + 2) / 4;		// Ratio of index values is 4
#if NOTCH_DEBUG
				txt = "Fxx";
#endif
				if (count1 > 0) {
#if NOTCH_DEBUG
					txt = "F1";
#endif
					for (i = -half_width; i <= half_width; i++) {
						j = k + i;
						if (j >= 0 && j < NOTCH_FILTER_FFT_SIZE)
							fltr_fft[j] = 0.0;
					}
				}
				k = (i2 + 2) / 4;		// Ratio of index values is 4
				if (count1 > 0 && count2 > 0) {
#if NOTCH_DEBUG
					txt = "F12";
#endif
					for (i = -half_width; i <= half_width; i++) {
						j = k + i;
						if (j >= 0 && j < NOTCH_FILTER_FFT_SIZE)
							fltr_fft[j] = 0.0;
					}
				}
				fftw_execute(fltrRev);
				// center the coefficient zero, make the filter symetric, reduce the size by one
				memmove(fltr_out + NOTCH_FILTER_DESIGN_SIZE / 2 - 1, fltr_out, sizeof(double) * (NOTCH_FILTER_SIZE / 2 - 1));
				for (i = NOTCH_FILTER_DESIGN_SIZE / 2 - 2, j = NOTCH_FILTER_DESIGN_SIZE / 2; i >= 0; i--, j++)
					fltr_out[i] = fltr_out[j];
				for (i = 0; i < NOTCH_FILTER_SIZE; i++)
					fltr_in[i] = fltr_out[i] * fft_window[i] / NOTCH_FILTER_DESIGN_SIZE;
				for (i = NOTCH_FILTER_SIZE; i < NOTCH_DATA_SIZE; i++)
					fltr_in[i] = 0.0;
				fftw_execute(fltrFwd);		// The filter is fltr_fft[]
			}
#if NOTCH_DEBUG
			QuiskPrintf("Max %12.0lf  frequency index1 %3d %5d %12.0lf  index2 %3d %5d %12.0lf  avg %12.0lf  %s\n", dmax, count1, i1, d1, count2, i2, d2, avg, txt);
#endif
			for (i = 0; i < NOTCH_FFT_SIZE; i++)	// Apply the filter
				notch_fft[i] *= fltr_fft[i];
			fftw_execute(planRev);		// Calculate inverse FFT
			memmove(data_in, data_in + NOTCH_DATA_OUTPUT_SIZE, NOTCH_DATA_START_SIZE * sizeof(double));
			for (i = NOTCH_DATA_START_SIZE; i < NOTCH_DATA_SIZE; i++)
				data_out[i] /= NOTCH_DATA_SIZE / 20;	// Empirical
		}
	}
	return;
}

static int audio_fft_ready=0;
static double * audio_average_fft;
void quisk_calc_audio_graph(double scale, complex double * csamples, double * dsamples, int nSamples, int real)
{ // Calculate an FFT for the audio data. Samples are either csamples or dsamples; the other is NULL.
  // The "scale" is the 0 dB reference. If "real", use the real part of csamples.
	int i, k, inp;
	static int index;
	static int count_fft;
	static int audio_fft_size;
	static int audio_fft_count;
	static fftw_plan plan = NULL;
	static double * fft_window;
	static complex double * audio_fft;

	if ( ! plan) {		// malloc new space and initialize
		index = 0;
		count_fft = 0;
		audio_fft_size = data_width;
		//audio_fft_count = 48000 / audio_fft_size / 5;		// Display refresh rate.
		audio_fft_count = 8000 / audio_fft_size / 5;		// Display refresh rate.
		if (audio_fft_count <= 0)
			audio_fft_count = 1;
		fft_window = (double *)malloc(audio_fft_size * sizeof(double));
		audio_average_fft = (double *)malloc(audio_fft_size * sizeof(double));
		audio_fft = (complex double *)malloc(audio_fft_size * sizeof(complex double));
		plan = fftw_plan_dft_1d(audio_fft_size, audio_fft, audio_fft, FFTW_FORWARD, FFTW_MEASURE);
		for (i = 0; i < audio_fft_size; i++) {
			audio_average_fft[i] = 0;
			fft_window[i] = 0.50 - 0.50 * cos(2. * M_PI * i / audio_fft_size);	// Hanning window loss 50%
		}
		return;
	}
	if (audio_fft_ready == 0) {	// calculate a new audio FFT
		if (dsamples || real)	// Lyons 2Ed p61
			scale *= audio_fft_size / 2.0;
		else
			scale *= audio_fft_size;
		scale *= audio_fft_count;
		scale *= 0.5;	// correct for Hanning window loss
		for (inp = 0; inp < nSamples; inp++) {
			if (dsamples)
				audio_fft[index] = dsamples[inp] / scale;
			else if (real)
				audio_fft[index] = creal(csamples[inp]) / scale;
			else
				audio_fft[index] = csamples[inp] / scale;
			if (++index >= audio_fft_size) {	// we have a full FFT of samples
				index = 0;
				for (i = 0; i < audio_fft_size; i++)
					audio_fft[i] *= fft_window[i];	// multiply by window
				fftw_execute(plan);		// Calculate forward FFT
				count_fft++;
				k = 0;
				for (i = audio_fft_size / 2; i < audio_fft_size; i++)		// Negative frequencies
					audio_average_fft[k++] += cabs(audio_fft[i]);
				for (i = 0; i < audio_fft_size / 2; i++)			// Positive frequencies
					audio_average_fft[k++] += cabs(audio_fft[i]);
				if (count_fft >= audio_fft_count) {
					audio_fft_ready = 1;
					count_fft = 0;
				}
			}
		}
	}
}

static PyObject * get_audio_graph(PyObject * self, PyObject * args)
{
	int i;
	double d2;
	PyObject * tuple2;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;

	if ( ! audio_fft_ready) {		// a new graph is not yet available
		Py_INCREF (Py_None);
		return Py_None;
	}
	tuple2 = PyTuple_New(data_width);
	for (i = 0; i < data_width; i++) {
		d2 = audio_average_fft[i];
		if (d2 < 1E-10)
			d2 = 1E-10;
		d2 = 20.0 * log10(d2);
		PyTuple_SetItem(tuple2, i, PyFloat_FromDouble(d2));
		audio_average_fft[i] = 0;
	}
	audio_fft_ready = 0;
	return tuple2;
}

static void d_delay(double * dsamples, int nSamples, int bank, int samp_delay)
{ // delay line (FIFO) to delay dsamples by samp_delay samples
	int i;
	double sample;
	static struct {
		double * buffer;
		int index;
		int buf_size;
	} delay[MAX_RX_CHANNELS] = {{NULL, 0, 0}};

	if ( ! delay[0].buffer)
		for (i = 1; i < MAX_RX_CHANNELS; i++)
			delay[i].buffer = NULL;
	if ( ! delay[bank].buffer) {
		delay[bank].buffer = (double *)malloc(samp_delay * sizeof(double));
		delay[bank].index = 0;
		delay[bank].buf_size = samp_delay;
		for (i = 0; i < samp_delay; i++)
			delay[bank].buffer[i] = 0;
	}
	for (i = 0; i < nSamples; i++) {
		sample = delay[bank].buffer[delay[bank].index];
		delay[bank].buffer[delay[bank].index] = dsamples[i];
		dsamples[i] = sample;
		if (++delay[bank].index >= delay[bank].buf_size)
			delay[bank].index = 0;
	}
}

static void ssb_squelch(double * dsamples, int nSamples, int samp_rate, struct _MeasureSquelch * MS)
{
	int i, bw, bw1, bw2, inp;
	double d, arith_avg, geom_avg, ratio;
	complex double c;
	static fftw_plan plan = NULL;
	static double * fft_window;
	static complex double * out_fft;
#ifdef QUISK_PRINT_LEVELS
	static int timer = 0;
	timer += nSamples;
#endif

	if ( ! MS->in_fft) {
		MS->in_fft = (double *)fftw_malloc(SQUELCH_FFT_SIZE * sizeof(double));
		MS->index = 0;
		MS->sq_open = 0;
	}
	if ( ! plan) {		// malloc new space and initialize
		fft_window = (double *)malloc(SQUELCH_FFT_SIZE * sizeof(double));
		out_fft = (complex double *)fftw_malloc((SQUELCH_FFT_SIZE / 2 + 1) * sizeof(complex double));
		// out_fft[0] is DC, then positive frequencies, then out_fft[N/2] is Nyquist.
		plan = fftw_plan_dft_r2c_1d(SQUELCH_FFT_SIZE, MS->in_fft, out_fft, FFTW_MEASURE);
		for (i = 0; i < SQUELCH_FFT_SIZE; i++)
			fft_window[i] = 0.50 - 0.50 * cos(2. * M_PI * i / SQUELCH_FFT_SIZE);	// Hanning window
		return;
	}
	for (inp = 0; inp < nSamples; inp++) {
		MS->in_fft[MS->index++] = dsamples[inp];
		if (MS->index >= SQUELCH_FFT_SIZE) {	// we have a full FFT of samples
			MS->index = 0;
			for (i = 0; i < SQUELCH_FFT_SIZE; i++)
				MS->in_fft[i] *= fft_window[i];	// multiply by window
			fftw_execute_dft_r2c(plan, MS->in_fft, out_fft);	// Calculate forward FFT
			bw = filter_bandwidth[0];	// Calculate the FFT bins within the filter bandwidth
			if (bw > 3000)
				bw = 3000;
			bw1 = 300 * SQUELCH_FFT_SIZE / samp_rate;		// start 300 Hz
			bw2 = (bw + 300) * SQUELCH_FFT_SIZE / samp_rate;	// end 300 Hz + bw
			arith_avg = 0.0;
			geom_avg = 0.0;
			for (i = bw1; i < bw2; i++) {
				c = out_fft[i] / CLIP16;
				d = creal(c) * creal(c) + cimag(c) * cimag(c);
				if (d > 1E-4) {
					arith_avg += d;
					geom_avg += log(d);
				}
			}
#ifdef QUISK_PRINT_SPECTRUM
			// Combine FFT into spectral bands
			int j;
			for (i = 0; i < 128; i += 16) {
				d = 0;
				for (j = i; j < i + 16; j++) {
					c = out_fft[i] / CLIP16;
					d += creal(c) * creal(c) + cimag(c) * cimag(c);
				}
				d = log(d / 16);
				QuiskPrintf ("%12.3f", d);
				if (i == 112)
					QuiskPrintf("\n");
			}
#endif
			if (arith_avg > 1E-4) {
				bw = bw2 - bw1;
				arith_avg = log(arith_avg / bw);
				geom_avg /= bw;
				ratio = arith_avg - geom_avg;
			}
			else {
				ratio = 1.0;
			}
			// For band noise, ratio is 0.57
			if (ratio > ssb_squelch_level * 0.005)
				MS->sq_open = samp_rate;	// one second timer
#ifdef QUISK_PRINT_LEVELS
			if (timer >= samp_rate / 2) {
				timer = 0;
				QuiskPrintf ("squelch %6d A %6.3f G %6.3f A-G %6.3f\n",
					MS->sq_open, arith_avg, geom_avg, ratio);
#ifdef QUISK_PRINT_SPECTRUM
				for (i = 0; i < 128; i += 16)
					QuiskPrintf ("%5d - %4d", i * samp_rate / SQUELCH_FFT_SIZE, (i + 16) * samp_rate / SQUELCH_FFT_SIZE);
				QuiskPrintf ("\n");
#endif
			}
#endif
		}
	}
	MS->sq_open -= nSamples;
	if (MS->sq_open < 0)
		MS->sq_open = 0;
	MS->squelch_active = MS->sq_open == 0;
}

static complex double dRxFilterOut(complex double sample, int bank, int nFilter)
{	// Rx FIR filter; bank is the static storage index, and must be different for different data streams.
	// Multiple filters are at nFilter.
	complex double cx;
	int j, k;
	static int init = 0;
	static struct stStorage {
		int indexFilter;						// current index into sample buffer
		complex double bufFilterC[MAX_FILTER_SIZE];	// Digital filter sample buffer
	} Storage[MAX_RX_CHANNELS];
	struct stStorage * ptBuf = Storage + bank;
	double * filtI;

	if ( ! init) {
		init = 1;
		for (j = 0; j < MAX_RX_CHANNELS; j++)
			memset(Storage + j, 0, sizeof(struct stStorage));
	}

	if ( ! sizeFilter)
		return sample;
	if (ptBuf->indexFilter >= sizeFilter)
		ptBuf->indexFilter = 0;
	ptBuf->bufFilterC[ptBuf->indexFilter] = sample;
	cx = 0;
	filtI = cFilterI[nFilter];
	j = ptBuf->indexFilter;
	for (k = 0; k < sizeFilter; k++) {
		cx += ptBuf->bufFilterC[j] * filtI[k];
		if (++j >= sizeFilter)
			j = 0;
	}
	ptBuf->indexFilter++;
	return cx;
}

complex double cRxFilterOut(complex double sample, int bank, int nFilter)
{	// Rx FIR filter; bank is the static storage index, and must be different for different data streams.
	// Multiple filters are at nFilter.
	double accI, accQ;
	double * filtI, * filtQ;
	int j, k;
	static int init = 0;
	static struct stStorage {
		int indexFilter;						// current index into sample buffer
		double bufFilterI[MAX_FILTER_SIZE];		// Digital filter sample buffer
		double bufFilterQ[MAX_FILTER_SIZE];		// Digital filter sample buffer
	} Storage[MAX_RX_CHANNELS];
	struct stStorage * ptBuf = Storage + bank;

	if ( ! init) {
		init = 1;
		for (j = 0; j < MAX_RX_CHANNELS; j++)
			memset(Storage + j, 0, sizeof(struct stStorage));
	}

	if ( ! sizeFilter)
		return sample;
	if (ptBuf->indexFilter >= sizeFilter)
		ptBuf->indexFilter = 0;
	ptBuf->bufFilterI[ptBuf->indexFilter] = creal(sample);
	ptBuf->bufFilterQ[ptBuf->indexFilter] = cimag(sample);
	filtI = cFilterI[nFilter];
	filtQ = cFilterQ[nFilter];
	accI = accQ = 0;
	j = ptBuf->indexFilter;
	for (k = 0; k < sizeFilter; k++) {
		accI += ptBuf->bufFilterI[j] * filtI[k];
		accQ += ptBuf->bufFilterQ[j] * filtQ[k];
		if (++j >= sizeFilter)
			j = 0;
	}
	ptBuf->indexFilter++;
	return accI + I * accQ;
}

static void AddTestTone(complex double * cSamples, int nSamples)
{
	int i;
	//int freq;
	//static int old_freq=0;
	static complex double testtoneVector = 21474836.47;	// -40 dB
	static complex double audioVector = 1.0;
	complex double audioPhase;

	switch (rxMode) {
	default:
		//testtonePhase = cexp(I * 2 * M_PI * (quisk_sidetoneCtrl - 500) / 1000.0);
		for (i = 0; i < nSamples; i++) {
			cSamples[i] += testtoneVector;
			testtoneVector *= testtonePhase;
		}
		break;
	case AM:		// AM
		//audioPhase = cexp(I * 2 * M_PI * quisk_sidetoneCtrl * 5 / sample_rate);
		audioPhase = cexp(I * 2.0 * M_PI * 1000 / quisk_sound_state.sample_rate);
		for (i = 0; i < nSamples; i++) {
			cSamples[i] += testtoneVector * (1.0 + creal(audioVector));
			testtoneVector *= testtonePhase;
			audioVector *= audioPhase;
		}
		break;
	case FM:		 // FM
	case DGT_FM:
#if 0
		freq = quisk_sidetoneCtrl * 5;
		audioPhase = cexp(I * 2 * M_PI * freq / quisk_sound_state.sample_rate);
		if (old_freq != freq) {
			old_freq = freq;
			printf("test tone frequency %d Hz\n", freq);
		}
#else
		audioPhase = cexp(I * 2.0 * M_PI * 1000 / quisk_sound_state.sample_rate);
#endif
		for (i = 0; i < nSamples; i++) {
			cSamples[i] += testtoneVector * cexp(I * creal(audioVector));
			testtoneVector *= testtonePhase;
			audioVector *= audioPhase;
		}
		break;
	}
}

static int IsSquelch(int freq)
{  // measure the signal level for squelch
	int i, i1, i2, iBandwidth;
	double meter;

	// This uses current_graph with width data_width
	iBandwidth = 5000 * data_width / fft_sample_rate;	// bandwidth determines number of pixels to average
	if (iBandwidth < 1)
		iBandwidth = 1;
	i1 = (int)((double)freq * data_width / fft_sample_rate + data_width / 2.0 - iBandwidth / 2.0 + 0.5);
	i2 = i1 + iBandwidth;
	meter = 0;
	if (i1 >= 0 && i2 < data_width) {	// too close to edge?
		for (i = i1; i < i2; i++)
			meter += current_graph[i];
	}
	meter /= iBandwidth;
	if (meter == 0 || meter < squelch_level)
		return 1;   // meter == 0 means Rx freq is off-screen so squelch is on
	else
		return 0;
}

static PyObject * set_record_state(PyObject * self, PyObject * args)
{  // called when a Record or Play button is pressed, or with -1 to poll
	int button;

	if (!PyArg_ParseTuple (args, "i", &button))
		return NULL;
	if ( ! quisk_record_buffer) {	// initialize
		quisk_record_bufsize = (int)(QuiskGetConfigDouble("max_record_minutes", 0.25) * quisk_sound_state.playback_rate * 60.0 + 0.2);
		quisk_record_buffer = (float *)malloc(sizeof(float) * quisk_record_bufsize);
	}
	switch (button) {
	case 0:			// press record radio
	case 4:			// press record microphone
		quisk_record_index = 0;
		quisk_play_index = 0;
		quisk_mic_index = 0;
		quisk_record_full = 0;
		if (button == 0)
			quisk_record_state = TMP_RECORD_SPEAKERS;
		else
			quisk_record_state = TMP_RECORD_MIC;
		break;
	case 1:			// release record
		quisk_record_state = IDLE;
		break;
	case 2:			// press play
		if (quisk_record_full) {
			quisk_play_index = quisk_record_index + 1;
			if (quisk_play_index >= quisk_record_bufsize)
				quisk_play_index = 0;
		}
		else {
			quisk_play_index = 0;
		}
		quisk_mic_index = quisk_play_index;
		quisk_record_state = TMP_PLAY_SPKR_MIC;
		break;
	case 3:			// release play
		quisk_record_state = IDLE;
		quisk_close_file_play = 1;
		break;
	case 5:			// press play file
		if (wavFpSound)
			fseek (wavFpSound, wavStart, SEEK_SET);
		if (wavFpMic)
			fseek (wavFpMic, wavStart, SEEK_SET);
		quisk_record_state = FILE_PLAY_SPKR_MIC;
		break;
	case 6:			// press play samples file
		if (wavFpSound)
			fseek (wavFpSound, wavStart, SEEK_SET);
		quisk_record_state = FILE_PLAY_SAMPLES;
		break;
	}
	return PyInt_FromLong(quisk_record_state != TMP_PLAY_SPKR_MIC && quisk_record_state != FILE_PLAY_SPKR_MIC && quisk_record_state != FILE_PLAY_SAMPLES);
}

void quisk_tmp_record(complex double * cSamples, int nSamples, double scale)		// save sound
{
	int i;

	for (i = 0; i < nSamples; i++) {
		quisk_record_buffer[quisk_record_index++] = creal(cSamples[i]) * scale;
		if (quisk_record_index >= quisk_record_bufsize) {
			quisk_record_index = 0;
			quisk_record_full = 1;
		}
	}
}

void quisk_tmp_playback(complex double * cSamples, int nSamples, double volume)
{  // replace radio sound with saved sound
	int i;
	double d;

	for (i = 0; i < nSamples; i++) {
		d = quisk_record_buffer[quisk_play_index++] * volume;
		cSamples[i] = d + I * d;
		if (quisk_play_index >= quisk_record_bufsize)
			quisk_play_index = 0;
		if (quisk_play_index == quisk_record_index) {
			quisk_record_state = IDLE;
			return;
		}
	}
}

static PyObject * tmp_record_save(PyObject * self, PyObject * args)
{
	const char * fname;
	int i, start;
	complex double * ptC;
	struct wav_file file_rec_tmp;

	if (!PyArg_ParseTuple (args, "s", &fname))
		return NULL;
	memset(&file_rec_tmp, 0, sizeof(struct wav_file));
	strMcpy(file_rec_tmp.file_name, fname, QUISK_PATH_SIZE);
	quisk_record_audio(&file_rec_tmp, NULL, -1);	// Open file
	if ( ! file_rec_tmp.fp) {
		QuiskPrintf("Failed to open file %s\n", fname);
	}
	else {
		if (quisk_record_full) {
			start = quisk_record_index + 1;
			if (start >= quisk_record_bufsize)
				start = 0;
		}
		else {
			start = 0;
		}
		ptC = malloc(quisk_record_bufsize * sizeof(complex double));
		for (i = 0; i < quisk_record_bufsize; i++)
			ptC[i] = quisk_record_buffer[i];
		if (start > 0)
			quisk_record_audio(&file_rec_tmp, ptC + start, quisk_record_bufsize - start);
		quisk_record_audio(&file_rec_tmp, ptC, quisk_record_index);
		free(ptC);
		quisk_record_audio(&file_rec_tmp, NULL, -2);		// Close file
	}
	Py_INCREF (Py_None);
	return Py_None;
}

void quisk_tmp_microphone(complex double * cSamples, int nSamples)
{  // replace microphone samples with saved sound
	int i;
	double d;

	for (i = 0; i < nSamples; i++) {
		d = quisk_record_buffer[quisk_mic_index++];
		cSamples[i] = d + I * d;
		if (quisk_mic_index >= quisk_record_bufsize)
			quisk_mic_index = 0;
		if (quisk_mic_index == quisk_record_index) {
			quisk_record_state = IDLE;
			return;
		}
	}
}

static void wav_files_close(void)
{
	if (wavFpMic)
		fclose(wavFpMic);
	if (wavFpSound)
		fclose(wavFpSound);
	wavFpSound = wavFpMic = NULL;
}

static PyObject * open_wav_file_play(PyObject * self, PyObject * args)
{
// Open the same file twice and record the start of the sound data. 
// One will be used to replace the speaker sound, the other replaces the mic sound.
// Use only one for the I/Q samples.
// The WAV file must be recorded at 48000 Hertz in S16_LE format monophonic for audio files.
// The WAV file must be recorded at the sample_rate in IEEE format stereo for the I/Q samples file.
	const char * fname;
	char name[5];
	int size, rate=0;

	if (!PyArg_ParseTuple (args, "s", &fname))
		return NULL;
	wav_files_close();
	wavFpSound = fopen(fname, "rb");
	if (!wavFpSound) {
		QuiskPrintf("open wav file failed\n");
		return PyInt_FromLong(-1);
	}
	wavStart = 0;
	while (1) {
		if (fread (name, 4, 1, wavFpSound) != 1)
			break;
		if (fread (&size, 4, 1, wavFpSound) != 1)
			break;
		name[4] = 0;
		// QuiskPrintf("name %s size %d\n", name, size);
		if (!strncmp(name, "RIFF", 4))
			fseek (wavFpSound, 4, SEEK_CUR);	// Skip "WAVE"
		else if (!strncmp(name, "fmt ", 4)) {	// format data starts here
			if (fread (&rate, 4, 1, wavFpSound) != 1)	// skip these fields
				break;
			if (fread (&rate, 4, 1, wavFpSound) != 1)	// sample rate
				break;
			//QuiskPrintf ("rate %d\n", rate);
			fseek (wavFpSound, size - 8, SEEK_CUR);	// skip remainder
		}
		else if (!strncmp(name, "data", 4)) {	// sound data starts here
			wavStart = ftell(wavFpSound);
			break;
		}
		else	// Skip other records
			fseek (wavFpSound, size, SEEK_CUR);
	}
	if (!wavStart) {		// Failure to find "data" record
		fclose(wavFpSound);
		wavFpSound = NULL;
		QuiskPrintf("open wav failed to find the data chunk\n");
		return PyInt_FromLong(-2);
	}
	wavFpMic = fopen(fname, "rb");
	if (!wavFpMic) {
		QuiskPrintf("open microphone wav file failed\n");
		fclose(wavFpSound);
		wavFpSound = NULL;
		return PyInt_FromLong(-4);
	}
	return PyInt_FromLong(rate);
}

void quisk_file_playback(complex double * cSamples, int nSamples, double volume)
{
	// Replace radio sound by file samples.
	// The sample rate must equal quisk_sound_state.mic_sample_rate.
	int i;
	short sh;
	double d;

	if (wavFpSound) {
		for (i = 0; i < nSamples; i++) {
			if (fread(&sh, 2, 1, wavFpSound) != 1) {
				quisk_record_state = IDLE;
				break;
			}
			d = sh * ((double)CLIP32 / CLIP16) * volume;
			cSamples[i] = d + I * d;
		}
	}
}

void quisk_play_samples(complex double * cSamples, int nSamples)
{
	int i;
	float fre, fim;

	if (wavFpSound) {
		for (i = 0; i < nSamples; i++) {
			if (fread(&fre, 4, 1, wavFpSound) != 1 || fread(&fim, 4, 1, wavFpSound) != 1) {
				quisk_record_state = IDLE;
				break;
			}
			fre *= CLIP32;
			fim *= CLIP32;
			cSamples[i] = fre + I * fim;
		}
	}
}

#define BUF2CHAN_SIZE	12000
static int Buffer2Chan(double * samp1, int count1, double * samp2, int count2)
{	// return the minimum of count1 and count2, buffering as necessary
	int nout;
	static int nbuf1=0, nbuf2=0;
	static double buf1[BUF2CHAN_SIZE], buf2[BUF2CHAN_SIZE];

	if (samp1 == NULL) {	// initialize
		nbuf1 = nbuf2 = 0;
		return 0;
	}
	if (nbuf1 == 0 && nbuf2 == 0 && count1 == count2)	// nothing to do
		return count1;
	if (count1 + nbuf1 >= BUF2CHAN_SIZE || count2 + nbuf2 >= BUF2CHAN_SIZE) {	// overflow
		if (DEBUG || DEBUG_IO)
			QuiskPrintf("Overflow in Buffer2Chan nbuf1 %d nbuf2 %d size %d\n", nbuf1, nbuf2, BUF2CHAN_SIZE);
		nbuf1 = nbuf2 = 0;
	}
	memcpy(buf1 + nbuf1, samp1, count1 * sizeof(double));	// add samples to buffer
	nbuf1 += count1;
	memcpy(buf2 + nbuf2, samp2, count2 * sizeof(double));
	nbuf2 += count2;
	if (nbuf1 <= nbuf2)
		nout = nbuf1;		// number of samples to output
	else
		nout = nbuf2;
	//if (count1 + nbuf1 >= 2000 || count2 + nbuf2 >= 2000)
	//	QuiskPrintf("Buffer2Chan nbuf1 %d nbuf2 %d nout %d\n", nbuf1, nbuf2, nout);
	memcpy(samp1, buf1, nout * sizeof(double));			// output samples
	nbuf1 -= nout;
	memmove(buf1, buf1 + nout, nbuf1 * sizeof(double));
	memcpy(samp2, buf2, nout * sizeof(double));
	nbuf2 -= nout;
	memmove(buf2, buf2 + nout, nbuf2 * sizeof(double));
	return nout;
}

void quisk_file_microphone(complex double * cSamples, int nSamples)
{
	// Replace mic samples by file samples.
	// The sample rate must equal quisk_sound_state.mic_sample_rate.
	int i;
	short sh;
	double d;

	if (wavFpMic) {
		for (i = 0; i < nSamples; i++) {
			if (fread(&sh, 2, 1, wavFpMic) != 1) {
				quisk_record_state = IDLE;
				break;
			}
			d = sh * ((double)CLIP32 / CLIP16);
			cSamples[i] = d + I * d;
		}
	}
}

int PlanDecimation(int * pt2, int * pt3, int * pt5)	// search for a suitable decimation scheme
{
	int i, best, try, i2, i3, i5, decim2, decim3, decim5;

	best = quisk_sound_state.sample_rate;
	decim2 = decim3 = decim5 = 0;
	for (i2 = 0; i2 <= 6; i2++) {		// limit to number of /2 filters, currently 6
		for (i3 = 0; i3 <= 3; i3++) {		// limit to number of /3 filters, currently 3
			for (i5 = 0; i5 <= 3; i5++) {		// limit to number of /5 filters, currently 3
				try = quisk_sound_state.sample_rate;
				for (i = 0; i < i2; i++)
					try /= 2;
				for (i = 0; i < i3; i++)
					try /= 3;
				for (i = 0; i < i5; i++)
					try /= 5;
				if (try >= 48000 && try < best) {
					decim2 = i2;
					decim3 = i3;
					decim5 = i5;
					best = try;
				}
			}
		}
	}
	if (best >= 50000)	// special rate converter
		best = best * 24 / 25;
	if (DEBUG)
		QuiskPrintf ("Plan Decimation: rate %i, best %i, decim2 %i, decim3 %i, decim5 %i\n",
			quisk_sound_state.sample_rate, best, decim2, decim3, decim5);
	if (best > 72000)
		QuiskPrintf("Failure to plan a suitable decimation in quisk_process_decimate\n");
	if (pt2) {	// return decimations
		*pt2 = decim2;
		*pt3 = decim3;
		*pt5 = decim5;
	}
	return best;
}

static int quisk_process_decimate(complex double * cSamples, int nSamples, int bank, rx_mode_type rx_mode)
{	// Changes here will require changes to get_filter_rate();
	int i, i2, i3, i5;
	static int decim2, decim3, decim5;
	static int old_rate = 0;
	static struct stStorage {
		struct quisk_cHB45Filter HalfBand1;
		struct quisk_cHB45Filter HalfBand2;
		struct quisk_cHB45Filter HalfBand3;
		struct quisk_cHB45Filter HalfBand4;
		struct quisk_cHB45Filter HalfBand5;
		struct quisk_cFilter filtSdriq111;
		struct quisk_cFilter filtSdriq53;
		struct quisk_cFilter filtSdriq133;
		struct quisk_cFilter filtSdriq167;
		struct quisk_cFilter filtSdriq185;
		struct quisk_cFilter filtDecim3;
		struct quisk_cFilter filtDecim3B;
		struct quisk_cFilter filtDecim3C;
		struct quisk_cFilter filtDecim5;
		struct quisk_cFilter filtDecim5B;
		struct quisk_cFilter filtDecim5S;
		struct quisk_cFilter filtDecim48to24;
		struct quisk_cFilter filtI3D25;
		struct quisk_cFilter filt300D5;
	} Storage[MAX_RX_CHANNELS] ;

	if ( ! cSamples) {	// Initialize all filters
		for (i = 0; i < MAX_RX_CHANNELS; i++) {
			memset(&Storage[i].HalfBand1, 0, sizeof(struct quisk_cHB45Filter));
			memset(&Storage[i].HalfBand2, 0, sizeof(struct quisk_cHB45Filter));
			memset(&Storage[i].HalfBand3, 0, sizeof(struct quisk_cHB45Filter));
			memset(&Storage[i].HalfBand4, 0, sizeof(struct quisk_cHB45Filter));
			memset(&Storage[i].HalfBand5, 0, sizeof(struct quisk_cHB45Filter));
			quisk_filt_cInit(&Storage[i].filtSdriq111, quiskFilt111D2Coefs, sizeof(quiskFilt111D2Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtSdriq53, quiskFilt53D1Coefs, sizeof(quiskFilt53D1Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtSdriq133, quiskFilt133D2Coefs, sizeof(quiskFilt133D2Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtSdriq167, quiskFilt167D3Coefs, sizeof(quiskFilt167D3Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtSdriq185, quiskFilt185D3Coefs, sizeof(quiskFilt185D3Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim3,  quiskFilt144D3Coefs, sizeof(quiskFilt144D3Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim3B, quiskFilt144D3Coefs, sizeof(quiskFilt144D3Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim3C, quiskFilt144D3Coefs, sizeof(quiskFilt144D3Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim5,  quiskFilt240D5CoefsSharp, sizeof(quiskFilt240D5CoefsSharp)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim5B, quiskFilt240D5CoefsSharp, sizeof(quiskFilt240D5CoefsSharp)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim5S, quiskFilt240D5CoefsSharp, sizeof(quiskFilt240D5CoefsSharp)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim48to24, quiskFilt48dec24Coefs, sizeof(quiskFilt48dec24Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtI3D25, quiskFiltI3D25Coefs, sizeof(quiskFiltI3D25Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filt300D5, quiskFilt300D5Coefs, sizeof(quiskFilt300D5Coefs)/sizeof(double));
		}
		return 0;
	}
	if (quisk_sound_state.sample_rate != old_rate) {
		old_rate = quisk_sound_state.sample_rate;
		PlanDecimation(&decim2, &decim3, &decim5);
	}
	// Decimate: Lower the sample rate to 48000 sps (or approx).  Filters are designed for
	// a pass bandwidth of 20 kHz and a stop bandwidth of 24 kHz.
	// We use 48 ksps to accommodate wide digital modes.
	switch((quisk_sound_state.sample_rate + 100) / 1000) {
	case 41:
		quisk_decim_srate = 48000;
		break;
	case 53:	// SDR-IQ
		quisk_decim_srate = quisk_sound_state.sample_rate;
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq53, 1);
		break;
	case 111:	// SDR-IQ
		quisk_decim_srate = quisk_sound_state.sample_rate / 2;
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq111, 2);
		break;
	case 133:	// SDR-IQ
		quisk_decim_srate = quisk_sound_state.sample_rate / 2;
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq133, 2);
		break;
	case 185:	// SDR-IQ
		quisk_decim_srate = quisk_sound_state.sample_rate / 3;
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq185, 3);
		break;
	case 370:
		quisk_decim_srate = quisk_sound_state.sample_rate / 6;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand2);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq185, 3);
		break;
	case 740:
		quisk_decim_srate = quisk_sound_state.sample_rate / 12;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand2);
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand3);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq185, 3);
		break;
	case 1333:
		quisk_decim_srate = quisk_sound_state.sample_rate / 24;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand1);
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand2);
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand3);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtSdriq167, 3);
		break;
	default:
		quisk_decim_srate = quisk_sound_state.sample_rate;
		i2 = decim2;	// decimate by 2 except for the final /2 filter
		if (i2 > 1) {
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand1);
			quisk_decim_srate /= 2;
			i2--;
		}
		if (i2 > 1) {
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand2);
			quisk_decim_srate /= 2;
			i2--;
		}
		if (i2 > 1) {
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand3);
			quisk_decim_srate /= 2;
			i2--;
		}
		if (i2 > 1) {
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand4);
			quisk_decim_srate /= 2;
			i2--;
		}
		if (i2 > 1) {
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
			quisk_decim_srate /= 2;
			i2--;
		}
		i3 = decim3;	// decimate by 3
		if (i3 > 0) {
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim3, 3);
			quisk_decim_srate /= 3;
			i3--;
		}
		if (i3 > 0) {
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim3B, 3);
			quisk_decim_srate /= 3;
			i3--;
		}
		if (i3 > 0) {
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim3C, 3);
			quisk_decim_srate /= 3;
			i3--;
		}
		i5 = decim5;	// decimate by 5
		if (i5 > 0) {
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim5, 5);
			quisk_decim_srate /= 5;
			i5--;
		}
		if (i5 > 0) {
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim5B, 5);
			quisk_decim_srate /= 5;
			i5--;
		}
		if (i5 > 0) {
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim5S, 5);
			quisk_decim_srate /= 5;
			i5--;
		}
		if (i2 > 0) {	// decimate by 2 last - Unnecessary???
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
			quisk_decim_srate /= 2;
			i2--;
		}
		if (quisk_decim_srate >= 50000) {
			quisk_decim_srate = quisk_decim_srate * 24 / 25;
			nSamples = quisk_cInterpDecim(cSamples, nSamples, &Storage[bank].filt300D5, 6, 5);	// 60 kSps
			nSamples = quisk_cInterpDecim(cSamples, nSamples, &Storage[bank].filtDecim5S, 4, 5);	// 48 kSps
		}
		if (i2 != 0 || i3 != 0 || i5 != 0)
			QuiskPrintf ("Failure in quisk.c in integer decimation for rate %d\n", quisk_sound_state.sample_rate);
		if (DEBUG && quisk_decim_srate != 48000)
			QuiskPrintf("Failure to achieve rate 48000. Rate is %i\n", quisk_decim_srate);
		break;
	}
	return nSamples;
}

static int quisk_process_demodulate(complex double * cSamples, double * dsamples, int nSamples, int bank, int nFilter, rx_mode_type rx_mode)
{	// Changes here will require changes to get_filter_rate();
	int i;
	bool isLSB;
	complex double cx;
	double d, di, dd;
	static struct AgcState Agc1 = {0.3, 16000, 0}, Agc2 = {0.3, 16000, 0};
//static int count=0;
//static double phase=0;
	static struct stStorage {
		complex double fm_1;			// Sample delayed by one
		double dc_remove;		// DC removal for AM
		double FM_www;
		double FM_nnn, FM_a_0, FM_a_1, FM_b_1, FM_x_1, FM_y_1;   // filter for FM
		//double FM_phase;
		struct quisk_cHB45Filter HalfBand4;
		struct quisk_cHB45Filter HalfBand5;
		struct quisk_dHB45Filter HalfBand6;
		struct quisk_dHB45Filter HalfBand7;
		struct quisk_dFilter filtAudio48p3;
		struct quisk_dFilter filtAudio24p3;
		struct quisk_dFilter filtAudio24p4;
		struct quisk_dFilter filtAudio12p2;
		struct quisk_dFilter filtAudio24p6;
		struct quisk_dFilter filtAudioFmHp;
		struct quisk_cFilter filtDecim16to8;
		struct quisk_cFilter filtDecim48to24;
		struct quisk_cFilter filtDecim48to16;
		//struct quisk_dFilter filtFMdiff;
	} Storage[MAX_RX_CHANNELS] ;

	if ( ! cSamples) {	// Initialize all filters
		for (i = 0; i < MAX_RX_CHANNELS; i++) {
			memset(&Storage[i].HalfBand4, 0, sizeof(struct quisk_cHB45Filter));
			memset(&Storage[i].HalfBand5, 0, sizeof(struct quisk_cHB45Filter));
			memset(&Storage[i].HalfBand6, 0, sizeof(struct quisk_dHB45Filter));
			memset(&Storage[i].HalfBand7, 0, sizeof(struct quisk_dHB45Filter));
			quisk_filt_dInit(&Storage[i].filtAudio48p3, quiskLpFilt48Coefs, sizeof(quiskLpFilt48Coefs)/sizeof(double));
			quisk_filt_dInit(&Storage[i].filtAudio24p3, quiskAudio24p3Coefs, sizeof(quiskAudio24p3Coefs)/sizeof(double));
			quisk_filt_dInit(&Storage[i].filtAudio24p4, quiskAudio24p4Coefs, sizeof(quiskAudio24p4Coefs)/sizeof(double));
			quisk_filt_dInit(&Storage[i].filtAudio12p2, quiskAudio24p4Coefs, sizeof(quiskAudio24p4Coefs)/sizeof(double));
			quisk_filt_dInit(&Storage[i].filtAudio24p6, quiskAudio24p6Coefs, sizeof(quiskAudio24p6Coefs)/sizeof(double));
			quisk_filt_dInit(&Storage[i].filtAudioFmHp, quiskAudioFmHpCoefs, sizeof(quiskAudioFmHpCoefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim16to8, quiskFilt16dec8Coefs, sizeof(quiskFilt16dec8Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim48to24, quiskFilt48dec24Coefs, sizeof(quiskFilt48dec24Coefs)/sizeof(double));
			quisk_filt_cInit(&Storage[i].filtDecim48to16, quiskAudio24p3Coefs, sizeof(quiskAudio24p3Coefs)/sizeof(double));
			//quisk_filt_dInit(&Storage[i].filtFMdiff, quiskDiff48Coefs, sizeof(quiskDiff48Coefs)/sizeof(double));
			//quisk_filt_differInit(&Storage[i].filtFMdiff, 9);
			Storage[i].fm_1 = 10;
			Storage[i].FM_www = tan(M_PI * FM_FILTER_DEMPH / 48000);   // filter for FM at 48 ksps
			Storage[i].FM_nnn = 1.0 / (1.0 + Storage[i].FM_www);
			Storage[i].FM_a_0 = Storage[i].FM_www * Storage[i].FM_nnn;
			Storage[i].FM_a_1 = Storage[i].FM_a_0;
			Storage[i].FM_b_1 = Storage[i].FM_nnn * (Storage[i].FM_www - 1.0);
			//QuiskPrintf ("dsamples[i] = y_1 = di * %12.6lf + x_1 * %12.6lf - y_1 * %12.6lf\n", FM_a_0, FM_a_1, FM_b_1);
		}
		return 0;
	}

	//quisk_calc_audio_graph(pow(2, 31) - 1, cSamples, NULL, nSamples, 0);
	// Filter and demodulate signal, copy capture buffer cSamples to play buffer dsamples.
	// quisk_decim_srate is the sample rate after integer decimation.
	MeasureSquelch[bank].squelch_active = 0;
	switch(rx_mode) {
	case CWL:		// lower sideband CW at 6 ksps
		quisk_filter_srate = quisk_decim_srate / 8;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand4);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			dsamples[i] = dd = creal(cx) + cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
		}
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, rit_freq, quisk_filter_srate);
		if (ssb_squelch_enabled) {
			ssb_squelch(dsamples, nSamples, quisk_filter_srate, MeasureSquelch + bank);
			d_delay(dsamples, nSamples, bank, SQUELCH_FFT_SIZE);
		}
		nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio12p2, 2);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand6);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		break;
	case CWU:		// upper sideband CW at 6 ksps
		quisk_filter_srate = quisk_decim_srate / 8;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand4);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			dsamples[i] = dd = creal(cx) - cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
		}
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, rit_freq, quisk_filter_srate);
		if (ssb_squelch_enabled) {
			ssb_squelch(dsamples, nSamples, quisk_filter_srate, MeasureSquelch + bank);
			d_delay(dsamples, nSamples, bank, SQUELCH_FFT_SIZE);
		}
		nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio12p2, 2);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand6);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		break;
	case LSB:	// lower sideband SSB at 12 ksps
		quisk_filter_srate = quisk_decim_srate / 4;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			dsamples[i] = dd = creal(cx) + cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
		}
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, 0, quisk_filter_srate);
		if (ssb_squelch_enabled) {
			ssb_squelch(dsamples, nSamples, quisk_filter_srate, MeasureSquelch + bank);
			d_delay(dsamples, nSamples, bank, SQUELCH_FFT_SIZE);
		}
		quisk_calc_audio_graph(pow(2, 31) - 1, NULL, dsamples, nSamples, 1);
		nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio24p4, 2);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		//quisk_calc_audio_graph(pow(2, 31) - 1, NULL, dsamples, nSamples, 1);
		break;
	case USB:	 // upper sideband SSB at 12 ksps
	default:
		quisk_filter_srate = quisk_decim_srate / 4;
		nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			dsamples[i] = dd = creal(cx) - cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
		}
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, 0, quisk_filter_srate);
		if (ssb_squelch_enabled) {
			ssb_squelch(dsamples, nSamples, quisk_filter_srate, MeasureSquelch + bank);
			d_delay(dsamples, nSamples, bank, SQUELCH_FFT_SIZE);
		}
		nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio24p4, 2);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		//quisk_calc_audio_graph(pow(2, 31) - 1, NULL, dsamples, nSamples, 1);
		break;
	case AM:		// AM at 24 ksps
		quisk_filter_srate = quisk_decim_srate / 2;
		nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		for (i = 0; i < nSamples; i++) {
			cx = dRxFilterOut(cSamples[i], bank, nFilter);
			di = cabs(cx);
			d = di + Storage[bank].dc_remove * 0.99;	// DC removal; R.G. Lyons page 553
			di = d - Storage[bank].dc_remove;
			Storage[bank].dc_remove = d;
			dsamples[i] = di;
			if(bank == 0) {
				measure_audio_sum += di * di;
				measure_audio_count += 1;
			}
		}
		nSamples = quisk_dFilter(dsamples, nSamples, &Storage[bank].filtAudio24p6);
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, 0, quisk_filter_srate);
		if (ssb_squelch_enabled) {
			ssb_squelch(dsamples, nSamples, quisk_filter_srate, MeasureSquelch + bank);
			d_delay(dsamples, nSamples, bank, SQUELCH_FFT_SIZE);
		}
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		break;
	case FM:		// FM at 48 ksps
	case DGT_FM:
		quisk_filter_srate = quisk_decim_srate;
#if 1
		for (i = 0; i < nSamples; i++) {
			cx = dRxFilterOut(cSamples[i], bank, nFilter);
			MeasureSquelch[bank].rf_sum += cabs(cx);
			MeasureSquelch[bank].rf_count += 1;
			// Phase difference in successive samples
			di = carg(cx * conj(Storage[bank].fm_1));
			Storage[bank].fm_1 = cx;
			dsamples[i] = di;
		}
#endif
#if 0
		count += nSamples;
		for (i = 0; i < nSamples; i++) {
			cx = dRxFilterOut(cSamples[i], bank, nFilter);
			// Integrate phase difference in successive samples and then differentiate. Phase drifts.
			di = carg(cx * conj(Storage[bank].fm_1));
			Storage[bank].fm_1 = cx;
			MeasureSquelch[bank].audio_sum += fabs(di);
			Storage[bank].FM_phase += di;
			dsamples[i] = Storage[bank].FM_phase;
		}
		if (count >= 48000) {
			count = 0;
			printf("Phase %12.4lf\n", dsamples[0]);
		}
		nSamples = quisk_dFilter(dsamples, nSamples, &Storage[bank].filtFMdiff);
#endif
		for (i = 0; i < nSamples; i++) {
			dsamples[i] *= 20e5;
			di = dsamples[i];
			// FM de-emphasis
			dsamples[i] = Storage[bank].FM_y_1 = di * Storage[bank].FM_a_0 +
				Storage[bank].FM_x_1 * Storage[bank].FM_a_1 - Storage[bank].FM_y_1 * Storage[bank].FM_b_1;
			Storage[bank].FM_x_1 = di;
		}
		nSamples = quisk_dDecimate(dsamples, nSamples, &Storage[bank].filtAudio48p3, 4);
		nSamples = quisk_dFilter(dsamples, nSamples, &Storage[bank].filtAudioFmHp);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand6);
		nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		if(bank == 0) {
			dAutoNotch(dsamples, nSamples, 0, quisk_filter_srate);
			for (i = 0; i < nSamples; i++) {
				measure_audio_sum += dsamples[i] * dsamples[i];
				measure_audio_count += 1;
			}
		}
		if (MeasureSquelch[bank].rf_count >= 2400) {
			MeasureSquelch[bank].squelch = MeasureSquelch[bank].rf_sum / MeasureSquelch[bank].rf_count / CLIP32;
			if (MeasureSquelch[bank].squelch > 1.E-10)
				MeasureSquelch[bank].squelch = 20 * log10(MeasureSquelch[bank].squelch);
			else
				MeasureSquelch[bank].squelch = -200.0;
			MeasureSquelch[bank].rf_sum = MeasureSquelch[bank].rf_count = 0;
			//printf("RF %12.4lf level %12.4lf\n", MeasureSquelch[bank].squelch, squelch_level);
		}
		MeasureSquelch[bank].squelch_active = MeasureSquelch[bank].squelch < squelch_level;
		break;
	case DGT_U:     // digital mode DGT-U at 48 ksps
		if (filter_bandwidth[nFilter] < DGT_NARROW_FREQ) {	// filter at 6 ksps
			quisk_filter_srate = quisk_decim_srate / 8;
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand4);
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		}
		else {	// filter at 48 ksps
			quisk_filter_srate = quisk_decim_srate;
		}
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			dsamples[i] = dd = creal(cx) - cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
		}
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, 0, quisk_filter_srate);
		if (filter_bandwidth[nFilter] < DGT_NARROW_FREQ) {
			nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio12p2, 2);
			nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand6);
			nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		}
		break;
	case DGT_L:     // digital mode DGT-L
		if (filter_bandwidth[nFilter] < DGT_NARROW_FREQ) {	// filter at 6 ksps
			quisk_filter_srate = quisk_decim_srate / 8;
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand5);
			nSamples = quisk_cDecim2HB45(cSamples, nSamples, &Storage[bank].HalfBand4);
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to24, 2);
		}
		else {	// filter at 48 ksps
			quisk_filter_srate = quisk_decim_srate;
		}
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			dsamples[i] = dd = creal(cx) + cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
		}
		if(bank == 0)
			dAutoNotch(dsamples, nSamples, 0, quisk_filter_srate);
		if (filter_bandwidth[nFilter] < DGT_NARROW_FREQ) {
			nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio12p2, 2);
			nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand6);
			nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
		}
		break;
	case DGT_IQ:     // digital mode DGT-IQ at 48 ksps
		quisk_filter_srate = quisk_decim_srate;
		if (filter_bandwidth[nFilter] < 19000) {		// No filtering for wide bandwidth
			for (i = 0; i < nSamples; i++)
				cSamples[i] = dRxFilterOut(cSamples[i], bank, nFilter);
		}
		if(bank == 0) {
			for (i = 0; i < nSamples; i++) {
				measure_audio_sum = measure_audio_sum + cSamples[i] * conj(cSamples[i]);
				measure_audio_count += 1;
			}
		}
		break;
	case FDV_U:	// digital voice
	case FDV_L:	// Extra modes added by Dave Roberts, G8KBB, June 2020. Modified by N2ADR 2024.
		quisk_check_freedv_mode();
		// current coding assumes input rate n_modem_sample_rate is 8000 or 48000.
		// decimate:
		switch (n_modem_sample_rate) {
		case 8000:
			quisk_filter_srate = quisk_decim_srate / 6;
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim48to16, 3);
			nSamples = quisk_cDecimate(cSamples, nSamples, &Storage[bank].filtDecim16to8, 2);
			break;
		case 48000:
			quisk_filter_srate = quisk_decim_srate;
			break;
		default:
			quisk_filter_srate = quisk_decim_srate / 6;
			QuiskPrintf("Bad FreeDV modem rate in quisk_process_demodulate\n");
			break;
		}
		// filter and demodulate to mono audio:
		isLSB = (rx_mode == FDV_L);
		complex double cBuf[nSamples];
		for (i = 0; i < nSamples; i++) {
			cx = cRxFilterOut(cSamples[i], bank, nFilter);
			if (isLSB)
				dd = creal(cx) + cimag(cx);
			else
				dd = creal(cx) - cimag(cx);
			if(bank == 0) {
				measure_audio_sum += dd * dd;
				measure_audio_count += 1;
			}
			cBuf[i] = dd;
		}
		// Perhaps decimate by an additional fraction
		if (quisk_decim_srate != 48000) {
			dd = quisk_decim_srate / 48000.0;
			nSamples = cFracDecim(cBuf, nSamples, dd);
			quisk_decim_srate = 48000;
		}
		if (bank == 0)
			process_agc(&Agc1, cBuf, nSamples, 0);
		else
			process_agc(&Agc2, cBuf, nSamples, 0);
		// pass data decimated down to n_modem_sample_rate
		// return data is at n_speech_sample_rate
#if 0
		static double dmax_in = 0;
		for (i = 0; i < nSamples; i++) {
			dd = fabs(creal(cBuf[i])) / CLIP16;
			if (dd > dmax_in)
				dmax_in = dd;
		}
		if (bank == 0 && measure_audio_count >= quisk_filter_srate * measure_audio_time) {
			//printf("Max sample in %.0lf\n", dmax_in);
			dmax_in = 0;
		}
#endif
		if (nSamples > n_max_modem_samples) {
			printf ("FreeDV: nSamples %d exceeds max_modem_samples %d\n", nSamples, n_max_modem_samples);
			nSamples = n_max_modem_samples;
		}
		short sBuf[nSamples];
		for (i = 0; i < nSamples; i++)
			sBuf[i] = (short)(creal(cBuf[i]) / CLIP16);
		if (freedv_monitor) {
			for (i = 0; i < nSamples; i++)
				dsamples[i] = (float)sBuf[i] * CLIP16;
		}
		else {
			if (pt_quisk_freedv_rx)
				nSamples = (* pt_quisk_freedv_rx)(sBuf, dsamples, nSamples, bank);
		}
#if 0
		static double dmax_out = 0;
		for (i = 0; i < nSamples; i++) {
			dd = fabs(dsamples[i]) / CLIP16;
			if (dd > dmax_out)
				dmax_out = dd;
		}
		if (bank == 0 && measure_audio_count >= quisk_filter_srate * measure_audio_time) {
			//printf("      Max sample out %.0lf\n", dmax_out);
			dmax_out = 0;
		}
#endif
		// current coding assumes output rate n_speech_sample_rate is 8000 or 16000
		switch (n_speech_sample_rate) {
		case 8000:
			nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio24p3, 3);
			nSamples = quisk_dInterp2HB45(dsamples, nSamples, &Storage[bank].HalfBand7);
			break;
		case 16000:
			nSamples = quisk_dInterpolate(dsamples, nSamples, &Storage[bank].filtAudio24p3, 3);
			break;
		}
		break;
	}
	if (bank == 0 && measure_audio_count >= quisk_filter_srate * measure_audio_time) {
		measured_audio = sqrt(measure_audio_sum / measure_audio_count) / CLIP32 * 1e6;
		measure_audio_sum = measure_audio_count = 0;
	}
	return nSamples;
}

static void process_agc(struct AgcState * dat, complex double * csamples, int count, int is_cpx)
{
	int i;
	double out_magn, buf_magn, dtmp, clip_gain;
	complex double csample;
#if DEBUG
	static int printit=0;
	static double maxout=1;
	char * clip;
#endif

	if ( ! dat->buf_size) {		// initialize
		if (dat->sample_rate == 0)
			dat->sample_rate = quisk_sound_state.playback_rate;
		dat->buf_size = dat->sample_rate * AGC_DELAY / 1000;	// total delay in samples
		//QuiskPrintf("play rate %d  buf_size %d\n", dat->sample_rate, dat->buf_size);
		dat->index_read = 0;				// Index to output; and then write a new sample here
		dat->index_start = 0;				// Start index for measure of maximum sample
		dat->is_clipping = 0;				// Are we decreasing gain to handle a clipping condition?
		dat->themax = 1.0;					// Maximum sample in the buffer
		dat->gain = 100;					// Current output gain
		dat->delta = 0;						// Amount to change dat->gain at each sample
		dat->target_gain = 100;				// Move to this gain unless we clip
		dat->time_release = 1.0 - exp( - 1.0 / dat->sample_rate / agc_release_time);			// long time constant for AGC release
		dat->c_samp = (complex double *) malloc(dat->buf_size * sizeof(complex double));		// buffer for complex samples
		for (i = 0; i < dat->buf_size; i++)
			dat->c_samp[i] = 0;
		return;
	}
	for (i = 0; i < count; i++) {
		csample = csamples[i];
		csamples[i] = dat->c_samp[dat->index_read] * dat->gain;		// FIFO output
		if (is_cpx)
			out_magn = cabs(csamples[i]);
		else
			out_magn = fabs(creal(csamples[i]));
		//if(dat->is_clipping == 1)
			//QuiskPrintf("      index %5d  out_magn %.5lf  gain %.2lf  delta  %.5lf\n",dat->index_read, out_magn / CLIP32, dat->gain, dat->delta);
#if DEBUG
		if (out_magn > maxout)
			maxout = out_magn;
#endif
		if (out_magn > CLIP32)	{
			csamples[i] /= out_magn;
#if DEBUG
			QuiskPrintf("Clip out_magn %8.5lf  is_clipping %d  index_read %5d  index_start %5d  gain %8.5lf\n",
				out_magn / CLIP32, dat->is_clipping, dat->index_read, dat->index_start, dat->gain);
#endif
		}
		dat->c_samp[dat->index_read] = csample;		// write new sample at read index
		if (is_cpx)
			buf_magn = cabs(csample);
		else
			buf_magn = fabs(creal(csample));
		if (dat->is_clipping == 0) {
			if (buf_magn * dat->gain > dat->max_out * CLIP32) {
				dat->target_gain = dat->max_out * CLIP32 / buf_magn;
				dat->delta = (dat->gain - dat->target_gain) / dat->buf_size;
				dat->is_clipping = 1;
				dat->themax = buf_magn;
				// QuiskPrintf("Start index %5d  buf_magn %10.8lf  target %8.2lf  gain %8.2lf  delta  %8.5lf\n",
				// 	dat->index_read, buf_magn / CLIP32, dat->target_gain, dat->gain, dat->delta);
				dat->gain -= dat->delta;
			}
			else if (dat->index_read == dat->index_start) {
				clip_gain = dat->max_out * CLIP32 / dat->themax;		// clip gain based on the maximum sample in the buffer
				if (0) //rxMode == FM || rxMode == DGT_FM)		// mode is FM
					dat->target_gain = clip_gain;
				else if (agcReleaseGain > clip_gain)
					dat->target_gain = clip_gain;
				else
					dat->target_gain = agcReleaseGain;
				dat->themax = buf_magn;
				dat->gain = dat->gain * (1.0 - dat->time_release) + dat->target_gain * dat->time_release;
				// QuiskPrintf("New   index %5d  themax %7.5lf  clip_gain %5.0lf  agcReleaseGain %5.0lf\n",
				// 	dat->index_start, dat->themax / CLIP32, clip_gain, agcReleaseGain);
			}
			else {
				if (dat->themax < buf_magn)
					dat->themax = buf_magn;
				dat->gain = dat->gain * (1.0 - dat->time_release) + dat->target_gain * dat->time_release;
			}
		}
		else {		// dat->is_clipping == 1;  we are handling a clip condition
			if (buf_magn > dat->themax) {
				dat->themax = buf_magn;
				dat->target_gain = dat->max_out * CLIP32 / buf_magn;
				dtmp = (dat->gain - dat->target_gain) / dat->buf_size;	// new value of delta
				if (dtmp > dat->delta) {
					dat->delta = dtmp;
					// QuiskPrintf(" Strt index %5d  buf_magn %10.8lf  target %8.2lf  gain %8.2lf  delta  %8.5lf\n",
					// 	dat->index_read, buf_magn / CLIP32, dat->target_gain, dat->gain, dat->delta);
				}
				else {
					// QuiskPrintf(" Plus index %5d  buf_magn %10.8lf  target %8.2lf  gain %8.2lf  delta  %8.5lf\n",
					// 	dat->index_read, buf_magn / CLIP32, dat->target_gain, dat->gain, dat->delta);
				}
			}
			dat->gain -= dat->delta;
			if (dat->gain <= dat->target_gain) {
				dat->is_clipping = 0;
				dat->gain = dat->target_gain;
				// QuiskPrintf("End   index %5d  buf_magn %10.8lf  target %8.2lf  gain %8.2lf  delta  %8.5lf  themax %10.8lf\n",
				// 	dat->index_read, buf_magn / CLIP32, dat->target_gain, dat->gain, dat->delta, dat->themax / CLIP32);
				dat->themax = buf_magn;
				dat->index_start = dat->index_read;
			}
		}
		if (++dat->index_read >= dat->buf_size)
			dat->index_read = 0;
#if DEBUG
		if (printit++ >= dat->sample_rate * 500 / 1000) {
			printit = 0;
			dtmp = 20 * log10(maxout / CLIP32);
			if (dtmp >= 0)
				clip = "Clip";
			else
				clip = "";
			QuiskPrintf("Out agcGain %5.0lf   target_gain %9.0lf   gain %9.0lf  output %7.2lf %s\n",
				agcReleaseGain, dat->target_gain, dat->gain, dtmp, clip);
			maxout = 1;
		}
#endif
	}
	return;
}

int quisk_process_samples(complex double * cSamples, int nSamples)
{
// Called when samples are available.
// Samples range from about 2^16 to a max of 2^31.
	int i, n, nout, squelch_real=0, squelch_imag=0;
	double d, di, tune;
	double double_filter_decim;
	complex double phase;
	int orig_nSamples;
	fft_data * ptFFT;
	rx_mode_type rx_mode;

	static int size_dsamples = 0;		// Current dimension of dsamples, dsamples2, orig_cSamples, buf_cSamples
	static int old_split_rxtx = 0;		// Prior value of split_rxtx
	static int old_multirx_play_channel = 0;		// Prior value of multirx_play_channel
	static double * dsamples = NULL;
	static double * dsamples2 = NULL;
	static complex double * orig_cSamples = NULL;
	static complex double * buf_cSamples = NULL;
	static complex double rxTuneVector = 1;
	static complex double txTuneVector = 1;
	static complex double aux1TuneVector = 1;
	static complex double aux2TuneVector = 1;
	static complex double sidetoneVector = BIG_VOLUME;
	static double dOutCounter = 0;		// Cumulative net output samples for sidetone etc.
	static int sidetoneIsOn = 0;		// The status of the sidetone
	static double sidetoneEnvelope;		// Shape the rise and fall times of the sidetone
	static double keyupEnvelope = 1.0;	// Shape the rise time on key up
	static int playSilence;
	static struct quisk_cHB45Filter HalfBand7 = {NULL, 0, 0};
	static struct quisk_cHB45Filter HalfBand8 = {NULL, 0, 0};
	static struct quisk_cHB45Filter HalfBand9 = {NULL, 0, 0};
	static struct AgcState Agc1 = {0.7, 0, 0}, Agc2 = {0.7, 0, 0}, Agc3 = {0.7, 0, 0};

#if DEBUG
	static int printit;
	static time_t time0;
	static double levelA=0, levelB=0, levelC=0, levelD=0, levelE=0;

	if (time(NULL) != time0) {
		time0 = time(NULL);
		printit = 1;
	}
	else {
		printit = 0;
	}
#endif
	if (nSamples <= 0)
		return nSamples;
	if (nSamples > size_dsamples) {
		if (dsamples)
			free(dsamples);
		if (dsamples2)
			free(dsamples2);
		if (orig_cSamples)
			free(orig_cSamples);
		if (buf_cSamples)
			free(buf_cSamples);
		size_dsamples = nSamples * 2;
		dsamples = (double *)malloc(size_dsamples * sizeof(double));
		dsamples2 = (double *)malloc(size_dsamples * sizeof(double));
		orig_cSamples = (complex double *)malloc(size_dsamples * sizeof(complex double));
		buf_cSamples = (complex double *)malloc(size_dsamples * sizeof(complex double));
	}

#if SAMPLES_FROM_FILE == 1
    QuiskWavWriteC(&hWav, cSamples, nSamples);
#elif SAMPLES_FROM_FILE == 2
    QuiskWavReadC(&hWav, cSamples, nSamples);
#endif

	orig_nSamples = nSamples;
	if (split_rxtx) {
		memcpy(orig_cSamples, cSamples, nSamples * sizeof(complex double));
		if ( ! old_split_rxtx)		// start of new split mode
			Buffer2Chan(NULL, 0, NULL, 0);
	}
	if (multirx_play_channel != old_multirx_play_channel)	// change in play channel
		Buffer2Chan(NULL, 0, NULL, 0);
	old_split_rxtx = split_rxtx;
	old_multirx_play_channel = multirx_play_channel;

	if (quisk_is_key_down() && !quisk_isFDX) {	// The key is down; replace this data block
		dOutCounter += (double)nSamples * quisk_sound_state.playback_rate /
				quisk_sound_state.sample_rate;
		nout = (int)dOutCounter;			// number of samples to output
		dOutCounter -= nout;
		playSilence = (int)(quisk_sound_state.playback_rate * 1E-3 * TxRxSilenceMsec);	// Play silence after sidetone ends, number of samples
		keyupEnvelope = 0;
		if (quisk_active_sidetone == 2 && QUISK_CWKEY_DOWN) {	// Play sidetone instead of radio for CW
			if (! sidetoneIsOn) {			// turn on sidetone
				sidetoneIsOn = 1;
				sidetoneEnvelope = 0;
				sidetoneVector = BIG_VOLUME;
			}
			for (i = 0 ; i < nout; i++) {
				if (sidetoneEnvelope < 1.0) {
					sidetoneEnvelope += 1. / (quisk_sound_state.playback_rate * 5e-3);	// 5 milliseconds
					if (sidetoneEnvelope > 1.0)
						sidetoneEnvelope = 1.0;
				}
				d = creal(sidetoneVector) * quisk_sidetoneVolume * sidetoneEnvelope;
				cSamples[i] = d + I * d;
				sidetoneVector *= sidetonePhase;
			}
		}
		else {			// Otherwise play silence
			for (i = 0 ; i < nout; i++)
				cSamples[i] = 0;
		}
		return nout;
	}
	// Key is up
	if(sidetoneIsOn) {		// decrease sidetone until it is off
		dOutCounter += (double)nSamples * quisk_sound_state.playback_rate /
				quisk_sound_state.sample_rate;
		nout = (int)dOutCounter;			// number of samples to output
		dOutCounter -= nout;
		for (i = 0; i < nout; i++) {
			sidetoneEnvelope -= 1. / (quisk_sound_state.playback_rate * 5e-3);	// 5 milliseconds
			if (sidetoneEnvelope < 0) {
				sidetoneIsOn = 0;
				sidetoneEnvelope = 0;
				break;		// sidetone is zero
			}
			d = creal(sidetoneVector) * quisk_sidetoneVolume * sidetoneEnvelope;
			cSamples[i] = d + I * d;
			sidetoneVector *= sidetonePhase;
		}
		for ( ; i < nout; i++) {	// continue with playSilence, even if zero
			cSamples[i] = 0;
			playSilence--;
		}
		return nout;
	}
	if (playSilence > 0) {		// Continue to play silence after the key is up
		dOutCounter += (double)nSamples * quisk_sound_state.playback_rate /
				quisk_sound_state.sample_rate;
		nout = (int)dOutCounter;			// number of samples to output
		dOutCounter -= nout;
		for (i = 0; i < nout; i++)
			cSamples[i] = 0;
		playSilence -= nout;
		return nout;
	}
	// We are done replacing sound with a sidetone or silence.
	// Filter and demodulate the samples as radio sound.

	// Add a test tone to the data
	if (testtonePhase)
		AddTestTone(cSamples, nSamples);

	// Invert spectrum
	if (quisk_invert_spectrum) {
		for (i = 0; i < nSamples; i++) {
			cSamples[i] = conj(cSamples[i]);
			}
	}

	NoiseBlanker(cSamples, nSamples);

	// Put samples into the fft input array.
	// Thanks to WB4JFI for the code to add a third FFT buffer, July 2010.
	// Changed to multiple FFTs May 2014.
	if (multiple_sample_rates == 0) {
	    ptFFT = fft_data_array + fft_data_index;
	    for (i = 0; i < nSamples; i++) {
		    ptFFT->samples[ptFFT->index] = cSamples[i];
		    if (++(ptFFT->index) >= fft_size) {		// check sample count
			    n = fft_data_index + 1;				// next FFT data location
			    if (n >= FFT_ARRAY_SIZE)
				    n = 0;
			    if (fft_data_array[n].filled == 0) {				// Is the next buffer empty?
				    fft_data_array[n].index = 0;
				    fft_data_array[n].block = 0;
				    fft_data_array[fft_data_index].filled = 1;	// Mark the previous buffer ready.
				    fft_data_index = n;							// Write samples into the new buffer.
				    ptFFT = fft_data_array + fft_data_index;
			    }
			    else {				// no place to write samples
				    ptFFT->index = 0;
				    fft_error++;
			    }
		    }
	    }
	}

	// Tune the data to frequency
	if (multiple_sample_rates == 0)
		tune = rx_tune_freq;
	else
		tune = rx_tune_freq + vfo_screen - vfo_audio;
	if (tune != 0) {
		phase = cexp((I * -2.0 * M_PI * tune) / quisk_sound_state.sample_rate);
		for (i = 0; i < nSamples; i++) {
			cSamples[i] *= rxTuneVector;
			rxTuneVector *= phase;
		}
	}

	if (rxMode == EXT) {		// External filter and demodulate
		d = (double)quisk_sound_state.sample_rate / quisk_sound_state.playback_rate;	// total decimation needed
		nSamples = quisk_extern_demod(cSamples, nSamples, d);
		goto start_agc;
	}

	// Perhaps write sample data to the soundcard output without decimation
	if (TEST_AUDIO == 1) {		// Copy I channel capture to playback
		di = 1.e4 * quisk_audioVolume;
		for (i = 0; i < nSamples; i++)
			cSamples[i] = creal(cSamples[i]) * di;
		return nSamples;
	}
	else if (TEST_AUDIO == 2) {	// Copy Q channel capture to playback
		di = 1.e4 * quisk_audioVolume;
		for (i = 0; i < nSamples; i++)
			cSamples[i] = cimag(cSamples[i]) * di;
		return nSamples;
	}
#if DEBUG
	for (i = 0; i < nSamples; i++) {
		d = cabs(cSamples[i]);
		if (levelA < d)
			levelA = d;
	}
#endif

	nSamples = quisk_process_decimate(cSamples, nSamples, 0, rxMode);

#if DEBUG
	for (i = 0; i < nSamples; i++) {
		d = cabs(cSamples[i]);
		if (levelB < d)
			levelB = d;
	}
#endif

	if (measure_freq_mode)
		measure_freq(cSamples, nSamples, quisk_decim_srate);

	nSamples = quisk_process_demodulate(cSamples, dsamples, nSamples, 0, 0, rxMode);

	squelch_real = 0;	// keep track of the squelch for the two play channels
	squelch_imag = 0;
	if (rxMode == DGT_IQ) {
		;		// This mode is already stereo
	}
	else if (split_rxtx) {		// Demodulate a second channel from the same receiver
		phase = cexp((I * -2.0 * M_PI * (quisk_tx_tune_freq + rit_freq)) / quisk_sound_state.sample_rate);
		// Tune the second channel to frequency
		for (i = 0; i < orig_nSamples; i++) {
			orig_cSamples[i] *= txTuneVector;
			txTuneVector *= phase;
		}
		n = quisk_process_decimate(orig_cSamples, orig_nSamples, 1, rxMode);
		n = quisk_process_demodulate(orig_cSamples, dsamples2, n, 1, 0, rxMode);
		nSamples = Buffer2Chan(dsamples, nSamples, dsamples2, n);		// buffer dsamples and dsamples2 so the count is equal
		// dsamples was demodulated on bank 0, dsamples2 on bank 1
		switch(split_rxtx) {
		default:
		case 1:		// stereo, higher frequency is real
			if (quisk_tx_tune_freq < rx_tune_freq) {
				squelch_real = MeasureSquelch[0].squelch_active;
				squelch_imag = MeasureSquelch[1].squelch_active;
				for (i = 0; i < nSamples; i++)
					cSamples[i] = dsamples[i] + I * dsamples2[i];
			}
			else {
				squelch_real = MeasureSquelch[1].squelch_active;
				squelch_imag = MeasureSquelch[0].squelch_active;
				for (i = 0; i < nSamples; i++)
					cSamples[i] = dsamples2[i] + I * dsamples[i];
			}
			break;
		case 2:		// stereo, lower frequency is real
			if (quisk_tx_tune_freq >= rx_tune_freq) {
				squelch_real = MeasureSquelch[0].squelch_active;
				squelch_imag = MeasureSquelch[1].squelch_active;
				for (i = 0; i < nSamples; i++)
					cSamples[i] = dsamples[i] + I * dsamples2[i];
			}
			else {
				squelch_real = MeasureSquelch[1].squelch_active;
				squelch_imag = MeasureSquelch[0].squelch_active;
				for (i = 0; i < nSamples; i++)
					cSamples[i] = dsamples2[i] + I * dsamples[i];
			}
			break;
		case 3:		// mono receive channel
			squelch_real = squelch_imag = MeasureSquelch[0].squelch_active;
			for (i = 0; i < nSamples; i++)
				cSamples[i] = dsamples[i] + I * dsamples[i];
			break;
		case 4:		// mono transmit channel
			squelch_real = squelch_imag = MeasureSquelch[1].squelch_active;
			for (i = 0; i < nSamples; i++)
				cSamples[i] = dsamples2[i] + I * dsamples2[i];
			break;
		}
	}
	else if (multirx_play_channel >= 0 && multirx_cSamples[multirx_play_channel]) {		// Demodulate a second channel from a different receiver
		memcpy(buf_cSamples, multirx_cSamples[multirx_play_channel], orig_nSamples * sizeof(complex double));
		phase = cexp((I * -2.0 * M_PI * (multirx_freq[multirx_play_channel])) / quisk_sound_state.sample_rate);
		// Tune the second channel to frequency
		for (i = 0; i < orig_nSamples; i++) {
			buf_cSamples[i] *= aux1TuneVector;
			aux1TuneVector *= phase;
		}
		n = quisk_process_decimate(buf_cSamples, orig_nSamples, 1, multirx_mode[multirx_play_channel]);
		n = quisk_process_demodulate(buf_cSamples, dsamples2, n, 1, 1, multirx_mode[multirx_play_channel]);
		nSamples = Buffer2Chan(dsamples, nSamples, dsamples2, n);		// buffer dsamples and dsamples2 so the count is equal
		switch(multirx_play_method) {
		default:
		case 0:		// play both
			squelch_real = squelch_imag = MeasureSquelch[1].squelch_active;
			for (i = 0; i < nSamples; i++)
				cSamples[i] = dsamples2[i] + I * dsamples2[i];
			break;
		case 1:		// play left
			squelch_real = MeasureSquelch[0].squelch_active;
			squelch_imag = MeasureSquelch[1].squelch_active;
			for (i = 0; i < nSamples; i++)
				cSamples[i] = dsamples[i] + I * dsamples2[i];
			break;
		case 2:		// play right
			squelch_real = MeasureSquelch[1].squelch_active;
			squelch_imag = MeasureSquelch[0].squelch_active;
			for (i = 0; i < nSamples; i++)
				cSamples[i] = dsamples2[i] + I * dsamples[i];
			break;
		}
	}
	else {		// monophonic sound played on both channels
		squelch_real = squelch_imag = MeasureSquelch[0].squelch_active;
		for (i = 0; i < nSamples; i++) {
			d = dsamples[i];
			cSamples[i] = d + I * d;
		}
	}

	// play sub-receiver 1 audio on a digital output device
	rx_mode = multirx_mode[0];
	if (quisk_multirx_count > 0 &&
	(rx_mode == DGT_U || rx_mode == DGT_L || rx_mode == DGT_IQ || rx_mode == DGT_FM)  &&
	quiskPlaybackDevices[QUISK_INDEX_SUB_RX1]->driver) {
		phase = cexp((I * -2.0 * M_PI * (multirx_freq[0])) / quisk_sound_state.sample_rate);
		// Tune the channel to frequency
		for (i = 0; i < orig_nSamples; i++) {
			multirx_cSamples[0][i] *= aux2TuneVector;
			aux2TuneVector *= phase;
		}
		n = quisk_process_decimate(multirx_cSamples[0], orig_nSamples, 2, rx_mode);
		n = quisk_process_demodulate(multirx_cSamples[0], dsamples2, n, 2, 2, rx_mode);
		if (rx_mode == DGT_IQ) {		// DGT-IQ
			process_agc(&Agc3, multirx_cSamples[0], n, 1);
		}
		else {
			for (i = 0; i < n; i++)
				multirx_cSamples[0][i] = dsamples2[i] + I * dsamples2[i];
			process_agc(&Agc3, multirx_cSamples[0], n, 0);
		}
		play_sound_interface(quiskPlaybackDevices[QUISK_INDEX_SUB_RX1], n, multirx_cSamples[0], 1, digital_output_level);
	}

	// Perhaps decimate by an additional fraction
	if (quisk_decim_srate != 48000) {
		double_filter_decim = quisk_decim_srate / 48000.0;
		nSamples = cFracDecim(cSamples, nSamples, double_filter_decim);
		quisk_decim_srate = 48000;
	}
	// Process the Rx path with the WDSP library
	nSamples = wdspFexchange0(QUISK_WDSP_RX, (double *)cSamples, nSamples);

	// Interpolate the samples from 48000 sps to the play rate.
	switch (quisk_sound_state.playback_rate / 48000) {
	case 1:
		break;
	case 2:
		nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand7);
		break;
	case 4:
		nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand7);
		nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand8);
		break;
	case 8:
		nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand7);
		nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand8);
		nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand9);
		break;
	default:
		QuiskPrintf ("Failure in quisk.c in integer interpolation %d %d\n", quisk_decim_srate, quisk_sound_state.playback_rate);
		break;
	}

	// Find the peak signal amplitude
start_agc:
	if (rxMode == EXT || rxMode == DGT_IQ) {		// Ext and DGT-IQ stereo sound
		process_agc(&Agc1, cSamples, nSamples, 1);
	}
	else if (rxMode == FDV_U || rxMode == FDV_L) {	// Agc already done
		;
	}
	else if (split_rxtx || multirx_play_channel >= 0) {		// separate AGC for left and right channels
		for (i = 0; i < nSamples; i++) {
			orig_cSamples[i] = cimag(cSamples[i]);
			cSamples[i] = creal(cSamples[i]);
		}
		process_agc(&Agc1, cSamples, nSamples, 0);
		process_agc(&Agc2, orig_cSamples, nSamples, 0);
		for (i = 0; i < nSamples; i++)
			cSamples[i] = creal(cSamples[i]) + I * creal(orig_cSamples[i]);
	}
	else {					// monophonic sound
		process_agc(&Agc1, cSamples, nSamples, 0);
	}
#if DEBUG
	if (printit) {
		d = CLIP32;
		//QuiskPrintf ("Levels: %12.8lf  %12.8lf %12.8lf  %12.8lf  %12.8lf\n",
		//	levelA/d, levelB/d, levelC/d, levelD/d, levelE/d);
		levelA = levelB = levelC = levelD = levelE = 0;
	}
#endif

	if (kill_audio) {
		squelch_real = squelch_imag = 1;
		for (i = 0; i < nSamples; i++)
			cSamples[i] = 0;
	}
	else if (squelch_real && squelch_imag) {
		for (i = 0; i < nSamples; i++)
			cSamples[i] = 0;
	}
	else if (squelch_imag) {
		for (i = 0; i < nSamples; i++)
			cSamples[i] = creal(cSamples[i]);
	}
	else if (squelch_real) {
		for (i = 0; i < nSamples; i++)
			cSamples[i] = I * cimag(cSamples[i]);
	}
	if (keyupEnvelope < 1.0) {		// raise volume slowly after the key goes up
		di = 1. / (quisk_sound_state.playback_rate * 5e-3);		// 5 milliseconds
		for (i = 0; i < nSamples; i++) {
			keyupEnvelope += di;
			if (keyupEnvelope > 1.0) {
				keyupEnvelope = 1.0;
				break;
			}
			cSamples[i] *= keyupEnvelope;
		}
	}
	if (quisk_record_state == TMP_RECORD_SPEAKERS && ! (squelch_real && squelch_imag))
		quisk_tmp_record(cSamples, nSamples, 1.0);		// save radio sound
	return nSamples;
}

static PyObject * get_state(PyObject * self, PyObject * args)
{
	int unused = 0;

	if (args && !PyArg_ParseTuple (args, ""))	// args=NULL internal call
		return NULL;
	return  Py_BuildValue("iiiiiNiNiiiiiiiii",
		quisk_sound_state.rate_min,
		quisk_sound_state.rate_max,
		quisk_sound_state.sample_rate,
		quisk_sound_state.chan_min,
		quisk_sound_state.chan_max,
		PyUnicode_DecodeUTF8(quisk_sound_state.msg1, strlen(quisk_sound_state.msg1), "replace"),
		unused,
		PyUnicode_DecodeUTF8(quisk_sound_state.err_msg, strlen(quisk_sound_state.err_msg), "replace"),
		quisk_sound_state.read_error,
		quisk_sound_state.write_error,
		quisk_sound_state.underrun_error,
		quisk_sound_state.latencyCapt,
		quisk_sound_state.latencyPlay,
		quisk_sound_state.interrupts,
		fft_error,
		mic_max_display,
		quisk_sound_state.data_poll_usec
		);
}

static PyObject * get_squelch(PyObject * self, PyObject * args)
{
	int freq;

	if (!PyArg_ParseTuple (args, "i", &freq))
		return NULL;
	return PyInt_FromLong(IsSquelch(freq));
}

static PyObject * get_overrange(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return PyInt_FromLong(quisk_get_overrange());
}

static PyObject * get_filter_rate(PyObject * self, PyObject * args)
{	// Return the filter sample rate as used by quisk_process_samples.
	// Changes to quisk_process_decimate or quisk_process_demodulate will require changes here.
	int rate, decim_srate, filter_srate, mode, bandwidth;
	// mode is -1 to use the rxMode
	if (!PyArg_ParseTuple (args, "ii", &mode, &bandwidth))
		return NULL;
	rate = quisk_sound_state.sample_rate;
	switch((rate + 100) / 1000) {
	case 41:
		decim_srate = 48000;
	case 53:	// SDR-IQ
		decim_srate = rate;
		break;
	case 111:	// SDR-IQ
		decim_srate = rate / 2;
		break;
	case 133:	// SDR-IQ
		decim_srate = rate / 2;
		break;
	case 185:	// SDR-IQ
		decim_srate = rate / 3;
		break;
	case 370:
		decim_srate = rate / 6;
		break;
	case 740:
		decim_srate = rate / 12;
		break;
	case 1333:
		decim_srate = rate / 24;
		break;
	default:
		decim_srate = PlanDecimation(NULL, NULL, NULL);
		break;
	}
	if (mode < 0) {
		mode = rxMode;
		bandwidth = filter_bandwidth[0];
	}
	switch(mode) {
	case CWL:		// lower sideband CW at 6 ksps
	case CWU:		// upper sideband CW at 6 ksps
		filter_srate = decim_srate / 8;
		break;
	case LSB:	 // lower sideband SSB at 12 ksps
	case USB:	 // upper sideband SSB at 12 ksps
	default:
		filter_srate = decim_srate / 4;
		break;
	case AM:		// AM at 24 ksps
		filter_srate = decim_srate / 2;
		break;
	case FM:		// FM at 48 ksps
	case DGT_FM:	// digital FM at 48 ksps
		filter_srate = decim_srate;
		break;
	case DGT_U:     // digital modes DGT-*
	case DGT_L:
		if (bandwidth < DGT_NARROW_FREQ)
			filter_srate = decim_srate / 8;
		else
			filter_srate = decim_srate;
		break;
	case DGT_IQ:	// digital mode at 48 ksps
		filter_srate = decim_srate;
		break;
	case FDV_U:	 // digital voice
	case FDV_L:
		filter_srate = n_modem_sample_rate;
		break;
	}
	//QuiskPrintf("Filter rate %d\n", filter_srate);
	return PyInt_FromLong(filter_srate);
}

static PyObject * get_smeter(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return PyFloat_FromDouble(Smeter);
}

static PyObject * get_hermes_adc(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return PyFloat_FromDouble(hermes_adc_level);
}

static void init_bandscope(void)
{
	int i, j;

	if (bandscope_size > 0) {
		bandscopePixels = (double *)malloc(graph_width * sizeof(double));
		bandscopeSamples = (double *)malloc(bandscope_size * sizeof(double));
		bandscopeWindow = (double *)malloc(bandscope_size * sizeof(double));
		bandscopeAverage = (double *)malloc((bandscope_size / 2 + 1 + 1) * sizeof(double));
		bandscopeFFT = (complex double *)malloc((bandscope_size / 2 + 1) * sizeof(complex double));
		bandscopePlan = fftw_plan_dft_r2c_1d(bandscope_size, bandscopeSamples, bandscopeFFT, FFTW_MEASURE);
		// Create the fft window
		for (i = 0, j = -bandscope_size / 2; i < bandscope_size; i++, j++)
			bandscopeWindow[i] = 0.5 + 0.5 * cos(2. * M_PI * j / bandscope_size);	// Hanning
		// zero the average array
		for (i = 0; i < bandscope_size / 2 + 1; i++)
			bandscopeAverage[i] = 0;
	}
}

static PyObject * add_rx_samples(PyObject * self, PyObject * args)
{
	int i;
	int ii, qq;	// ii, qq must be four bytes
	unsigned char * pt_ii;
	unsigned char * pt_qq;
	Py_buffer view;
	PyObject * samples;

	if (!PyArg_ParseTuple (args, "O", &samples))
		return NULL;
	if ( ! PyObject_CheckBuffer(samples)) {
		QuiskPrintf("add_rx_samples: Invalid object sent as samples\n");
		Py_INCREF (Py_None);
		return Py_None;
	}
	if (PyObject_GetBuffer(samples, &view, PyBUF_SIMPLE) != 0) {
		QuiskPrintf("add_rx_samples: Can not view sample buffer\n");
		Py_INCREF (Py_None);
		return Py_None;
	}
	if (view.len % (py_sample_rx_bytes * 2) != 0) {
		QuiskPrintf ("add_rx_samples: Odd number of bytes in sample buffer\n");
	}
	else if (PySampleCount + view.len / py_sample_rx_bytes / 2 > SAMP_BUFFER_SIZE * 8 / 10) {
		QuiskPrintf ("add_rx_samples: buffer is too full\n");
	}
	else if (py_sample_rx_endian == 0) {	// byte order of samples is little-endian
		void * buf;
		void * buf_end;
		buf = view.buf;
		buf_end = buf + view.len;
		pt_ii = (unsigned char *)&ii + 4 - py_sample_rx_bytes;
		pt_qq = (unsigned char *)&qq + 4 - py_sample_rx_bytes;
		while (buf < buf_end) {
			ii = qq = 0;
			memcpy(pt_ii, buf, py_sample_rx_bytes);
			buf += py_sample_rx_bytes;
			memcpy(pt_qq, buf, py_sample_rx_bytes);
			buf += py_sample_rx_bytes;
			PySampleBuf[PySampleCount++] = ii + qq * I;
		}
	}
	else {		// byte order of samples is big-endian
		unsigned char * buf;
		unsigned char * buf_end;
		buf = view.buf;
		buf_end = buf + view.len;
		while (buf < buf_end) {
			ii = qq = 0;
			pt_ii = (unsigned char *)&ii + 3;
			pt_qq = (unsigned char *)&qq + 3;
			for (i = 0; i < py_sample_rx_bytes; i++)
				*pt_ii-- = *buf++;
			for (i = 0; i < py_sample_rx_bytes; i++)
				*pt_qq-- = *buf++;
			PySampleBuf[PySampleCount++] = ii + qq * I;
		}
	}
	PyBuffer_Release(&view);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * add_bscope_samples(PyObject * self, PyObject * args)
{
	int i, count;
	int ii;		// ii must be four bytes
	unsigned char * pt_ii;
	Py_buffer view;
	PyObject * samples;

	if (!PyArg_ParseTuple (args, "O", &samples))
		return NULL;
	if (bandscope_size <= 0) {
		QuiskPrintf("add_bscope_samples: The bandscope was not initialized with InitBscope()\n");
		Py_INCREF (Py_None);
		return Py_None;
	}
	if ( ! PyObject_CheckBuffer(samples)) {
		QuiskPrintf("add_bscope_samples: Invalid object sent as samples\n");
		Py_INCREF (Py_None);
		return Py_None;
	}
	if (PyObject_GetBuffer(samples, &view, PyBUF_SIMPLE) != 0) {
		QuiskPrintf("add_bscope_samples: Can not view sample buffer\n");
		Py_INCREF (Py_None);
		return Py_None;
	}
	count = 0;
	if (view.len != bandscope_size * py_bscope_bytes) {
		QuiskPrintf ("add_bscope_samples: Wrong number of bytes in sample buffer\n");
	}
	else if (py_bscope_endian == 0) {	// byte order of samples is little-endian
		void * buf;
		void * buf_end;
		buf = view.buf;
		buf_end = buf + view.len;
		pt_ii = (unsigned char *)&ii + 4 - py_bscope_bytes;
		while (buf < buf_end) {
			ii = 0;
			memcpy(pt_ii, buf, py_bscope_bytes);
			buf += py_bscope_bytes;
			bandscopeSamples[count++] = (double)ii / CLIP32;
		}
	}
	else {		// byte order of samples is big-endian
		unsigned char * buf;
		unsigned char * buf_end;
		buf = view.buf;
		buf_end = buf + view.len;
		while (buf < buf_end) {
			ii = 0;
			pt_ii = (unsigned char *)&ii + 3;
			for (i = 0; i < py_bscope_bytes; i++)
				*pt_ii-- = *buf++;
			bandscopeSamples[count++] = (double)ii / CLIP32;
		}
	}
	PyBuffer_Release(&view);
	bandscopeState = 99;
	Py_INCREF (Py_None);
	return Py_None;
}

static void py_sample_start(void)
{
}

static void py_sample_stop(void)
{
	if (bandscopePlan) {
		fftw_destroy_plan(bandscopePlan);
		bandscopePlan = NULL;
	}
}

static int py_sample_read(complex double * cSamples)
{
	int n;

	memcpy(cSamples, PySampleBuf, PySampleCount * sizeof(complex double));
	n = PySampleCount;
	PySampleCount = 0;
	return n;
}

static PyObject * get_params(PyObject * self, PyObject * args)
{
	const char * name;

	if (!PyArg_ParseTuple (args, "s", &name))
		return NULL;
	if (strcmp(name, "QUISK_HAVE_PULSEAUDIO") == 0) {
#ifdef QUISK_HAVE_PULSEAUDIO
		return PyInt_FromLong(1);
#else
		return PyInt_FromLong(0);
#endif
	}
	if (strcmp(name, "rx_udp_started") == 0)
		return PyInt_FromLong(quisk_rx_udp_started);
	if (strcmp(name, "serial_ptt") == 0)
		return PyInt_FromLong(quisk_serial_ptt);
	if (strcmp(name, "hl2_txbuf_errors") == 0)
		return PyInt_FromLong(hl2_txbuf_errors);
	if (strcmp(name, "quisk_tx_inhibit") == 0)
		return PyInt_FromLong((long)quisk_tx_inhibit);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * write_fftw_wisdom(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	fftw_export_wisdom_to_filename(fftw_wisdom_name);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * read_fftw_wisdom(PyObject * self, PyObject * args)
{
	char * wisdom;
	PyObject * pyBytes;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	wisdom = fftw_export_wisdom_to_string();
	pyBytes = PyByteArray_FromStringAndSize(wisdom, strlen(wisdom));
	free(wisdom);
	return pyBytes;
}
	
static PyObject * set_params(PyObject * self, PyObject * args, PyObject * keywds)
{  /* Call with keyword arguments ONLY; change local parameters */
	static char * kwlist[] = {"quisk_is_vna", "rx_bytes", "rx_endian", "read_error", "clip", 
	"bscope_bytes", "bscope_endian", "bscope_size", "bandscopeScale", "hermes_pause", 
	"freedv_monitor", NULL} ;
	int i, nbytes, read_error, clip, bscope_size, hermes_pause;

	nbytes = read_error = clip = bscope_size = hermes_pause = -1;
	if (!PyArg_ParseTupleAndKeywords (args, keywds, "|iiiiiiiidii", kwlist,
	&quisk_is_vna, &nbytes, &py_sample_rx_endian, &read_error, &clip,
	&py_bscope_bytes, &py_bscope_endian, &bscope_size, &bandscopeScale, &hermes_pause,
	&freedv_monitor))
		return NULL;
	if (nbytes != -1) {
		py_sample_rx_bytes = nbytes;
		quisk_sample_source4(py_sample_start, py_sample_stop, py_sample_read, NULL);
	}
	if (read_error != -1)
		quisk_sound_state.read_error++;
	if (clip != -1)
		quisk_sound_state.overrange++;
	if (bscope_size > 0) {
		if (bandscope_size == 0) {
			bandscope_size = bscope_size;
			init_bandscope();
		}
		else if (bscope_size != bandscope_size) {
			QuiskPrintf ("Illegal attempt to change bscope_size\n");
		}
	}
	if (hermes_pause != -1) {
		i = quisk_multirx_state;
		if (hermes_pause) {	// pause the hermes samples
			if (quisk_multirx_state < 20)
				quisk_multirx_state = 20;
		}
		else {			// resume the hermes samples
			if (quisk_multirx_state >= 20)
			        quisk_multirx_state = 0;
		}
		return PyInt_FromLong(i);
	}
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * get_hermes_TFRC(PyObject * self, PyObject * args)
{	// return average temperature, forward and reverse power and current
	PyObject * ret;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	if (hermes_count_temperature > 0) {
		hermes_temperature /= hermes_count_temperature;
		hermes_fwd_power /= hermes_count_temperature;
	}
	else {
		hermes_temperature  = 0.0;
		hermes_fwd_power  = 0.0;
	}
	if (hermes_count_current > 0) {
		hermes_rev_power /= hermes_count_current;
		hermes_pa_current /= hermes_count_current;
	}
	else {
		hermes_rev_power  = 0.0;
		hermes_pa_current  = 0.0;
	}
	ret = Py_BuildValue("dddddd", hermes_temperature, hermes_fwd_power, hermes_rev_power, hermes_pa_current, hermes_fwd_peak, hermes_rev_peak);
	hermes_temperature = 0;
	hermes_fwd_power = 0;
	hermes_rev_power = 0;
	hermes_fwd_peak = 0;
	hermes_rev_peak = 0;
	hermes_pa_current = 0;
	hermes_count_temperature = 0;
	hermes_count_current = 0;
	return ret;
}

static PyObject * measure_frequency(PyObject * self, PyObject * args)
{
	int mode;

	if (!PyArg_ParseTuple (args, "i", &mode))
		return NULL;
	if (mode >= 0)	// mode >= 0 set the mode; mode < 0, just return the frequency
		measure_freq_mode = mode;
	return PyFloat_FromDouble(measured_frequency);
}

static PyObject * measure_audio(PyObject * self, PyObject * args)
{
	int time;

	if (!PyArg_ParseTuple (args, "i", &time))
		return NULL;
	if (time > 0)	// set the average time
		measure_audio_time = time;
	return PyFloat_FromDouble(measured_audio);
}

static PyObject * add_tone(PyObject * self, PyObject * args)
{  /* Add a test tone to the captured audio data */
	int freq;

	if (!PyArg_ParseTuple (args, "i", &freq))
		return NULL;
	if (freq && quisk_sound_state.sample_rate)
		testtonePhase = cexp((I * 2.0 * M_PI * freq) / quisk_sound_state.sample_rate);
	else
		testtonePhase = 0;
	Py_INCREF (Py_None);
	return Py_None;
}

static void close_udp(void)
{
	short msg = 0x7373;		// shutdown

	quisk_using_udp = 0;
	if (rx_udp_socket != INVALID_SOCKET) {
		shutdown(rx_udp_socket, QUISK_SHUT_RD);
		send(rx_udp_socket, (char *)&msg, 2, 0);
		send(rx_udp_socket, (char *)&msg, 2, 0);
		QuiskSleepMicrosec(3000000);
		close(rx_udp_socket);
		rx_udp_socket = INVALID_SOCKET;
	}
	quisk_rx_udp_started = 0;
#ifdef MS_WINDOWS
	if (cleanupWSA) {
		cleanupWSA = 0;
		WSACleanup();
	}
#endif
}

static void close_udp10(void)		// Metis-Hermes protocol
{
	int i;
	unsigned char buf[64];

	quisk_using_udp = 0;
	if (rx_udp_socket != INVALID_SOCKET) {
		shutdown(rx_udp_socket, QUISK_SHUT_RD);
		buf[0] = 0xEF;
		buf[1] = 0xFE;
		buf[2] = 0x04;
		buf[3] = 0x00;
		for (i = 4; i < 64; i++)
			buf[i] = 0;
		send(rx_udp_socket, (char *)buf, 64, 0);
		QuiskSleepMicrosec(5000);
		send(rx_udp_socket, (char *)buf, 64, 0);
		QuiskSleepMicrosec(2000000);
		close(rx_udp_socket);
		rx_udp_socket = INVALID_SOCKET;
	}
	quisk_rx_udp_started = 0;
	quisk_multirx_state = 0;
	if (bandscopePlan) {
		fftw_destroy_plan(bandscopePlan);
		bandscopePlan = NULL;
	}
#ifdef MS_WINDOWS
	if (cleanupWSA) {
		cleanupWSA = 0;
		WSACleanup();
	}
#endif
}

static PyObject * close_rx_udp(PyObject * self, PyObject * args)
{  // Not necessary to call from Python because close_udp() is called from sound.c
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	//close_udp();
	Py_INCREF (Py_None);
	return Py_None;
}

static int quisk_read_rx_udp(complex double * samp)	// Read samples from UDP
{		// Size of complex sample array is SAMP_BUFFER_SIZE
	ssize_t bytes;
	unsigned char buf[1500];	// Maximum Ethernet is 1500 bytes.
	static unsigned char seq0;	// must be 8 bits
	int i, n, nSamples, xr, xi, index, want_samples;
	unsigned char * ptxr, * ptxi;
	struct timeval tm_wait;
	fd_set fds;

	// Data from the receiver is little-endian
	if ( ! rx_udp_gain_correct) {
		int dec;
		dec = (int)(rx_udp_clock / quisk_sound_state.sample_rate + 0.5);
		if ((dec / 5) * 5 == dec)		// Decimation by a factor of 5
			rx_udp_gain_correct = 1.31072;
		else						// Decimation by factors of two
			rx_udp_gain_correct = 1.0;
	}
	if ( ! quisk_rx_udp_started) {	// we never received any data
		// send our return address until we receive UDP blocks
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 5000;
		FD_ZERO (&fds);
		FD_SET (rx_udp_socket, &fds);
		if (select (rx_udp_socket + 1, &fds, NULL, NULL, &tm_wait) == 1) {	// see if data is available
			bytes = recv(rx_udp_socket, (char *)buf, 1500,  0);	// throw away the first block
			seq0 = buf[0] + 1;	// Next expected sequence number
			quisk_rx_udp_started = 1;
#if DEBUG_IO
			QuiskPrintf("Udp data started\n");
#endif
		}
		else {		// send our return address to the sample source
			buf[0] = buf[1] = 0x72;	// UDP command "register return address"
			send(rx_udp_socket, (char *)buf, 2, 0);
			return 0;
		}
	}
	nSamples = 0;
	want_samples = (int)(quisk_sound_state.data_poll_usec * 1e-6 * quisk_sound_state.sample_rate + 0.5);
	while (nSamples < want_samples) {		// read several UDP blocks
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 100000; // Linux seems to have problems with very small time intervals
		FD_ZERO (&fds);
		FD_SET (rx_udp_socket, &fds);
		i = select (rx_udp_socket + 1, &fds, NULL, NULL, &tm_wait);
		if (i == 1)
			;
		else if (i == 0) {
#if DEBUG_IO
			QuiskPrintf("Udp socket timeout\n");
#endif
			return 0;
		}
		else {
#if DEBUG_IO
			QuiskPrintf("Udp select error %d\n", i);
#endif
			return 0;
		}
		bytes = recv(rx_udp_socket, (char *)buf, 1500,  0);	// blocking read
		if (bytes != RX_UDP_SIZE) {		// Known size of sample block
			quisk_sound_state.read_error++;
#if DEBUG_IO
			QuiskPrintf("read_rx_udp: Bad block size\n");
#endif
			continue;
		}
		// buf[0] is the sequence number
		// buf[1] is the status:
		//		bit 0:  key up/down state
		//		bit 1:	set for ADC overrange (clip)
		if (buf[0] != seq0) {
#if DEBUG_IO
			QuiskPrintf("read_rx_udp: Bad sequence want %3d got %3d\n",
					(unsigned int)seq0, (unsigned int)buf[0]);
#endif
			quisk_sound_state.read_error++;
		}
		seq0 = buf[0] + 1;		// Next expected sequence number
		n = buf[1] & 0x01;		// bit zero is key state and the PTT state
		quisk_hardware_cwkey = n;
		hardware_ptt = n;
		if (quisk_hardware_cwkey != old_hardware_cwkey) {
			old_hardware_cwkey = quisk_hardware_cwkey;
			quisk_set_play_state();
		}
		if (buf[1] & 0x02)					// bit one is ADC overrange
			quisk_sound_state.overrange++;
		index = 2;
		ptxr = (unsigned char *)&xr;
		ptxi = (unsigned char *)&xi;
		// convert 24-bit samples to 32-bit samples; int must be 32 bits.
		if (is_little_endian) {
			while (index < bytes) {		// This works for 3, 2, 1 byte samples
				xr = xi = 0;
				memcpy (ptxr + (4 - sample_bytes), buf + index, sample_bytes);
				index += sample_bytes;
				memcpy (ptxi + (4 - sample_bytes), buf + index, sample_bytes);
				index += sample_bytes;
				samp[nSamples++] = (xr + xi * I) * rx_udp_gain_correct;
				xr = xi = 0;
				memcpy (ptxr + (4 - sample_bytes), buf + index, sample_bytes);
				index += sample_bytes;
				memcpy (ptxi + (4 - sample_bytes), buf + index, sample_bytes);
				index += sample_bytes;
				samp[nSamples++] = (xr + xi * I) * rx_udp_gain_correct;
			}
		}
		else {		// big-endian
			while (index < bytes) {		// This works for 3-byte samples only
				*(ptxr    ) = buf[index + 2];
				*(ptxr + 1) = buf[index + 1];
				*(ptxr + 2) = buf[index    ];
				*(ptxr + 3) = 0;
				index += 3;
				*(ptxi    ) = buf[index + 2];
				*(ptxi + 1) = buf[index + 1];
				*(ptxi + 2) = buf[index    ];
				*(ptxi + 3) = 0;
				index += 3;
				samp[nSamples++] = (xr + xi * I) * rx_udp_gain_correct;;
				*(ptxr    ) = buf[index + 2];
				*(ptxr + 1) = buf[index + 1];
				*(ptxr + 2) = buf[index    ];
				*(ptxr + 3) = 0;
				index += 3;
				*(ptxi    ) = buf[index + 2];
				*(ptxi + 1) = buf[index + 1];
				*(ptxi + 2) = buf[index    ];
				*(ptxi + 3) = 0;
				index += 3;
				samp[nSamples++] = (xr + xi * I) * rx_udp_gain_correct;;

			}
		}
	}
	return nSamples;
}

static int quisk_hermes_is_ready(int rx_udp_socket)
{		// Start Hermes; return 1 when we are ready to receive data
	unsigned char buf[1500];
	int i, dummy;
	struct timeval tm_wait;
	fd_set fds;

	if (rx_udp_socket == INVALID_SOCKET)
		return 0;
	switch (quisk_multirx_state) {
	case 0:			// Start or restart
	case 20:		// Temporary shutdown
		quisk_rx_udp_started = 0;
		buf[0] = 0xEF;
		buf[1] = 0xFE;
		buf[2] = 0x04;
		buf[3] = 0x00;
		for (i = 4; i < 64; i++)
			buf[i] = 0;
		send(rx_udp_socket, (char *)buf, 64, 0);		// send Stop
		quisk_multirx_state++;
		QuiskSleepMicrosec(2000);
		return 0;
	case 1:
	case 21:
		buf[0] = 0xEF;
		buf[1] = 0xFE;
		buf[2] = 0x04;
		buf[3] = 0x00;
		for (i = 4; i < 64; i++)
			buf[i] = 0;
		send(rx_udp_socket, (char *)buf, 64, 0);		// send Stop
		quisk_multirx_state++;
		QuiskSleepMicrosec(9000);
		return 0;
	case 2:
	case 22:
		while (1) {
			tm_wait.tv_sec = 0;			// throw away all pending records
			tm_wait.tv_usec = 0;
			FD_ZERO (&fds);
			FD_SET (rx_udp_socket, &fds);
			if (select (rx_udp_socket + 1, &fds, NULL, NULL, &tm_wait) != 1)
				break;
			recv(rx_udp_socket, (char *)buf, 1500,  0);
		}
		// change to state 3 for startup
		// change to state 23 for temporary shutdown
		quisk_multirx_state++;
		return 0;
	case 3:
		quisk_multirx_count = quisk_pc_to_hermes[3] >> 3 & 0x7;		// number of receivers
		for (i = 0; i < quisk_multirx_count; i++)
			if ( ! multirx_fft_data[i].samples)		// Check that buffer exists
				multirx_fft_data[i].samples = (fftw_complex *)malloc(multirx_fft_width * sizeof(fftw_complex));
		quisk_hermes_tx_send(0, NULL);
		quisk_multirx_state++;
		return 0;
	case 4:
	case 5:
	case 6:
	case 7:
		dummy = 999999;		// enable transmit
		quisk_hermes_tx_send(rx_udp_socket, &dummy);	// send packets with number of receivers
		quisk_multirx_state++;
		QuiskSleepMicrosec(2000);
		return 0;
	case 8:
		if (quisk_rx_udp_started) {
			quisk_multirx_state++;
		}
		else {
			// send our return address until we receive UDP blocks
			buf[0] = 0xEF;
			buf[1] = 0xFE;
			buf[2] = 0x04;
			if (enable_bandscope)
				buf[3] = 0x03;
			else
				buf[3] = 0x01;
			for (i = 4; i < 64; i++)
				buf[i] = 0;
			send(rx_udp_socket, (char *)buf, 64, 0);
			QuiskSleepMicrosec(2000);
		}
		return 1;
	case 9:		// running state; we have received UDP blocks
	default:
		return 1;
	case 23:	// we are in a temporary shutdown
		return 0;
	}
}

static int read_rx_udp10(complex double * samp)	// Read samples from UDP using the Hermes protocol.
{		// Size of complex sample array is SAMP_BUFFER_SIZE.  Called from the sound thread.
	ssize_t bytes;
	unsigned char buf[1500];
	unsigned int seq;
	unsigned int power;
	static unsigned int seq0;
	static int tx_records;
	static int max_multirx_count=0;
	int i, j, nSamples, xr, xi, index, start, want_samples, dindex, num_records;
	complex double c;
	struct timeval tm_wait;
	fd_set fds;

	if ( ! quisk_hermes_is_ready(rx_udp_socket)) {
		seq0 = 0;
		tx_records = 0;
		quisk_rx_udp_started = 0;
		multirx_fft_next_index = 0;
		multirx_fft_next_state = 0;
		for (i = 0; i < QUISK_MAX_SUB_RECEIVERS; i++)
			multirx_fft_data[i].index = 0;
		return 0;
	}
	nSamples = 0;
	want_samples = (int)(quisk_sound_state.data_poll_usec * 1e-6 * quisk_sound_state.sample_rate + 0.5);
	num_records = 504 / ((quisk_multirx_count + 1) * 6 + 2);	// number of samples in each of two blocks for each receiver
	if (quisk_multirx_count) {
		if (multirx_sample_size < want_samples + 2000) {
			multirx_sample_size = want_samples * 2 + 2000;
			for (i = 0; i < max_multirx_count; i++) {
				free(multirx_cSamples[i]);
				multirx_cSamples[i] = (complex double *)malloc(multirx_sample_size * sizeof(complex double));
			}
		}
		if (quisk_multirx_count > max_multirx_count) {
			for (i = max_multirx_count; i < quisk_multirx_count; i++)
				multirx_cSamples[i] = (complex double *)malloc(multirx_sample_size * sizeof(complex double));
			max_multirx_count = quisk_multirx_count;
		}
	}
	while (nSamples < want_samples) {		// read several UDP blocks
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 100000; // Linux seems to have problems with very small time intervals
		FD_ZERO (&fds);
		FD_SET (rx_udp_socket, &fds);
		i = select (rx_udp_socket + 1, &fds, NULL, NULL, &tm_wait);		// blocking wait
		if (i == 1)
			;
		else if (i == 0) {
#if DEBUG_IO
			QuiskPrintf("Udp socket timeout\n");
#endif
			return 0;
		}
		else {
#if DEBUG_IO
			QuiskPrintf("Udp select error %d\n", i);
#endif
			return 0;
		}
		bytes = recv(rx_udp_socket, (char *)buf, 1500,  0);			// blocking read
		if (bytes != 1032 || buf[0] != 0xEF || buf[1] != 0xFE || buf[2] != 0x01) {		// Known size of sample block
			quisk_sound_state.read_error++;
#if DEBUG_IO
			QuiskPrintf("read_rx_udp10: Bad block size %d or header\n", (int)bytes);
#endif
			return 0;
		}
		////   Bandscope data - reversed byte order ?????
		if (buf[3] == 0x04 && bandscopeSamples) {		// ADC samples for bandscope
			seq = buf[7];		// sequence number
			seq = seq & (bandscopeBlockCount - 1);	// 0, 1, 2, ...
			switch (bandscopeState) {
			case 0:	// Start - wait for the start of a block and record block one
				if (seq == 0) {
					for (i = 0, j = 8; i < 512; i++, j+= 2)
						bandscopeSamples[i] = ((double)(short)(buf[j + 1] << 8 | buf[j])) / bandscopeScale;
					bandscopeState = 1;
				}
				break;
			default:
			case 1:	// Record blocks
				if (seq == bandscopeState) {
					for (i = 0, j = 8; i < 512; i++, j+= 2)
						bandscopeSamples[i + 512 * seq] = ((double)(short)(buf[j + 1] << 8 | buf[j])) / bandscopeScale;
					if (++bandscopeState >= bandscopeBlockCount)
						bandscopeState = 99;
				}
				else {
					bandscopeState = 0;	// Error
				}
				break;
			case 99:	// wait until the complete block is used
				break;
			}
			continue;
		}
		////   ADC Rx samples
		if (buf[3] != 0x06)		// End point 6: I/Q and mic samples
			return 0;
		seq = buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7];	// sequence number
		quisk_rx_udp_started = 1;
		tx_records += num_records * 2;		// total samples for each receiver
		quisk_hermes_tx_send(rx_udp_socket, &tx_records);		// send Tx samples, decrement tx_records
		if (seq != seq0) {
#if DEBUG_IO
			QuiskPrintf("read_rx_udp10: Bad sequence want %d got %d\n", seq0, seq);
#endif
			quisk_sound_state.read_error++;
		}
		seq0 = seq + 1;		// Next expected sequence number
		for (start = 11; start < 1000; start += 512) {
			// check the sync bytes
			if (buf[start - 3] != 0x7F || buf[start - 2] != 0x7F || buf[start - 1] != 0x7F) {
#if DEBUG_IO
				QuiskPrintf("read_rx_udp10: Bad sync byte\n");
#endif
				quisk_sound_state.read_error++;
			}
			// read five bytes of control information.  start is the index of C0.
			// Changes for HermesLite v2 thanks to Steve, KF7O
			dindex = buf[start] >> 1;
			if (dindex & 0x40) {	// the ACK bit C0[7] is set
				if (quisk_hermeslite_writepointer > 0) {
					// Save response
					quisk_hermeslite_response[0] = buf[start];
					quisk_hermeslite_response[1] = buf[start+1];
					quisk_hermeslite_response[2] = buf[start+2];
					quisk_hermeslite_response[3] = buf[start+3];
					quisk_hermeslite_response[4] = buf[start+4];
					// Look for match
					if (dindex == 0x7f) {
						QuiskPrintf("ERROR: Hermes-Lite did not process ACK command. Send again.\n");
						quisk_hermeslite_writepointer = 1;
					} else if (dindex != (quisk_hermeslite_writequeue[0])) {
						QuiskPrintf("ERROR: Nonmatching Hermes-Lite ACK response 0x%X seen\n",dindex);
					} else {
						//QuiskPrintf("Response received                       queue 0x%X 0x%X 0x%X 0x%X 0x%X\n", quisk_hermeslite_writequeue[0],
						//quisk_hermeslite_writequeue[1], quisk_hermeslite_writequeue[2], quisk_hermeslite_writequeue[3], quisk_hermeslite_writequeue[4]);
						quisk_hermeslite_writepointer = 0;
					} 
				} else {
					QuiskPrintf("ERROR: ACK response for 0x%X but no request outstanding\n",dindex);
				}
			} else {
				dindex = dindex >> 2;
			}
			// this does not save data for Hermes-Lite ACK
			if (dindex >= 0 && dindex <= 4) {		// Save the data returned by the hardware
				quisk_hermes_to_pc[dindex * 4    ] = buf[start + 1];	// C1 to C4
				quisk_hermes_to_pc[dindex * 4 + 1] = buf[start + 2];
				quisk_hermes_to_pc[dindex * 4 + 2] = buf[start + 3];
				quisk_hermes_to_pc[dindex * 4 + 3] = buf[start + 4];
			} 
			if (dindex == 0) {		// C0 is 0b00000xxx
//QuiskPrintTime("Poll key change", 0);
				//code_version = quisk_hermes_to_pc[3];
				if ((quisk_hermes_to_pc[0] & 0x01) != 0)	// C1
					quisk_sound_state.overrange++;
				quisk_tx_inhibit = (quisk_hermes_to_pc[0] & 0x02) == 0;	// should limit to Hermes Lite2 only
				hardware_ptt = buf[start] & 0x01;	// C0 bit zero is PTT
				quisk_hardware_cwkey = (buf[start] & 0x04) >> 2;	// C0 bit two is CW key state
				switch (hl2_txbuf_state) {
				case 0:			// hermes_mox_bit is zero.
				default:
					if (hermes_mox_bit) {
						hl2_txbuf_state = 1;
						//QuiskPrintf ("Change hermes_mox_bit %d\n", hermes_mox_bit);
					}
					break;
				case 1:			// hermes_mox_bit changed to 1
					if (hermes_mox_bit == 0) {
						//QuiskPrintf ("Change hermes_mox_bit %d\n", hermes_mox_bit);
						hl2_txbuf_state = 0;
					}
					else if (quisk_hermes_to_pc[2] & 0x7F) {	// check for samples in the HL2 Tx buffer
						hl2_txbuf_state = 2;
					}
					break;
				case 2:			// initial samples are in the buffer
					if (hermes_mox_bit == 0) {
						//QuiskPrintf ("Change hermes_mox_bit %d\n", hermes_mox_bit);
						hl2_txbuf_state = 0;
					}
					else if (quisk_hermes_to_pc[2] == 0x80 || quisk_hermes_to_pc[2] == 0xFF) {	// check for errors
						hl2_txbuf_errors++;
						//QuiskPrintf("FAULT quisk_hermes_to_pc[2] 0x%X\n", quisk_hermes_to_pc[2]);
						hl2_txbuf_state = 3;
					}
					break;
				case 3:			// the error bit was set; wait for it to clear
					if (hermes_mox_bit == 0) {
						//QuiskPrintf ("Change hermes_mox_bit %d\n", hermes_mox_bit);
						hl2_txbuf_state = 0;
					}
					else if ((quisk_hermes_to_pc[2] & 0x80) == 0) {
						hl2_txbuf_state = 2;
					}
					break;
				}
				if (quisk_hardware_cwkey != old_hardware_cwkey) {
//QuiskPrintTime("Udp10 change key", 0);
					old_hardware_cwkey = quisk_hardware_cwkey;
					quisk_set_play_state();
				}
			}
			else if(dindex == 1) {	// temperature and forward power
				hermes_temperature += quisk_hermes_to_pc[4] << 8 | quisk_hermes_to_pc[5];
				power = quisk_hermes_to_pc[6] << 8 | quisk_hermes_to_pc[7];
				hermes_fwd_power += power;
				hermes_fwd_peak = fmax(hermes_fwd_peak, (double)power);
				hermes_count_temperature++;
			}
			else if (dindex == 2) {	// reverse power and current
				power = quisk_hermes_to_pc[8] << 8 | quisk_hermes_to_pc[9];
				hermes_rev_power += power;
				hermes_rev_peak = fmax(hermes_rev_peak, (double)power);
				hermes_pa_current += quisk_hermes_to_pc[10] << 8 | quisk_hermes_to_pc[11];
				hermes_count_current++;
			}
			// convert 24-bit samples to 32-bit samples; int must be 32 bits.
			index = start + 5;
			for (i = 0; i < num_records; i++) {		// read records
				xi = buf[index    ] << 24 | buf[index + 1] << 16 | buf[index + 2] << 8;
				xr = buf[index + 3] << 24 | buf[index + 4] << 16 | buf[index + 5] << 8;
				samp[nSamples] = xr + xi * I;		// first receiver
				index += 6;
				for (j = 0; j < quisk_multirx_count; j++) {		// multirx receivers
					xi = buf[index    ] << 24 | buf[index + 1] << 16 | buf[index + 2] << 8;
					xr = buf[index + 3] << 24 | buf[index + 4] << 16 | buf[index + 5] << 8;
					c = xr + xi * I;
					multirx_cSamples[j][nSamples] = c;
					if (multirx_fft_data[j].index < multirx_fft_width)
						multirx_fft_data[j].samples[multirx_fft_data[j].index++] = c;
					index += 6;
				}
				nSamples++;
				index += 2;
			}
		}
	}
	if ((quisk_pc_to_hermes[3] >> 3 & 0x7) != quisk_multirx_count &&	// change in number of receivers
		( ! quisk_multirx_count || multirx_fft_next_state == 2)) {		// wait until the current FFT is finished
			quisk_multirx_state = 0;		// Do not change receiver count without stopping Hermes and restarting
	}
	if (multirx_fft_next_state == 2) {		// previous FFT is done
		if (++multirx_fft_next_index >= quisk_multirx_count)
			multirx_fft_next_index = 0;
		multirx_fft_next_state = 0;
	}
	if (quisk_multirx_count && multirx_fft_next_state == 0 && multirx_fft_data[multirx_fft_next_index].index >= multirx_fft_width) {		// FFT is read to run
		memcpy(multirx_fft_next_samples, multirx_fft_data[multirx_fft_next_index].samples, multirx_fft_width * sizeof(fftw_complex));
		multirx_fft_data[multirx_fft_next_index].index = 0;
		multirx_fft_next_time = 1.0 / graph_refresh / quisk_multirx_count;
		multirx_fft_next_state = 1;			// this FFT is ready to run
	}
	return nSamples;
}

static int read_rx_udp17(complex double * cSamples0)	// Read samples from UDP
{		// Size of complex sample array is SAMP_BUFFER_SIZE
	ssize_t bytes;
	unsigned char buf[1500];	// Maximum Ethernet is 1500 bytes.
	static unsigned char seq0;	// must be 8 bits
	int i, n, nSamples0, xr, xi, index, want_samples, key_down;
	complex double sample;
	unsigned char * ptxr, * ptxi;
	struct timeval tm_wait;
	fft_data * ptFFT;
	fd_set fds;
	static int block_number=0;
	// Data from the receiver is little-endian

	if ( ! rx_udp_gain_correct) {	// correct for second stage CIC decimation JIM JIM
		int dec;
		dec = (int)(rx_udp_clock / 30.0 / fft_sample_rate + 0.5);
		if ((dec / 3) * 3 == dec)		// Decimation by a factor of 3
			rx_udp_gain_correct = 1.053497942;
		else						// Decimation by factors of two
			rx_udp_gain_correct = 1.0;
		//QuiskPrintf ("Gain %d %.8lf\n", dec, rx_udp_gain_correct);
	}
	if ( ! quisk_rx_udp_started) {	// we never received any data
		// send our return address until we receive UDP blocks
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 5000;
		FD_ZERO (&fds);
		FD_SET (rx_udp_socket, &fds);
		if (select (rx_udp_socket + 1, &fds, NULL, NULL, &tm_wait) == 1) {	// see if data is available
			bytes = recv(rx_udp_socket, (char *)buf, 1500,  0);	// throw away the first block
			seq0 = buf[0] + 1;	// Next expected sequence number
			quisk_rx_udp_started = 1;
#if DEBUG_IO || DEBUG
			QuiskPrintf("Udp data started\n");
#endif
		}
		else {		// send our return address to the sample source
			buf[0] = buf[1] = 0x72;	// UDP command "register return address"
			send(rx_udp_socket, (char *)buf, 2, 0);
			return 0;
		}
	}
	nSamples0 = 0;
	want_samples = (int)(quisk_sound_state.data_poll_usec * 1e-6 * quisk_sound_state.sample_rate + 0.5);
	key_down = quisk_is_key_down();
	while (nSamples0 < want_samples) {		// read several UDP blocks
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 100000; // Linux seems to have problems with very small time intervals
		FD_ZERO (&fds);
		FD_SET (rx_udp_socket, &fds);
		i = select (rx_udp_socket + 1, &fds, NULL, NULL, &tm_wait);
		if (i == 1)
			;
		else if (i == 0) {
#if DEBUG_IO || DEBUG
			QuiskPrintf("Udp socket timeout\n");
#endif
			return 0;
		}
		else {
#if DEBUG_IO || DEBUG
			QuiskPrintf("Udp select error %d\n", i);
#endif
			return 0;
		}
		bytes = recv(rx_udp_socket, (char *)buf, 1500,  0);	// blocking read
		if (bytes != RX_UDP_SIZE) {		// Known size of sample block
			quisk_sound_state.read_error++;
#if DEBUG_IO || DEBUG
			QuiskPrintf("read_rx_udp: Bad block size\n");
#endif
			continue;
		}
		// buf[0] is the sequence number
		// buf[1] is the status:
		//		bit 0:  key up/down state
		//		bit 1:	set for ADC overrange (clip)
		if (buf[0] != seq0) {
#if DEBUG_IO || DEBUG
			QuiskPrintf("read_rx_udp: Bad sequence want %3d got %3d\n",
					(unsigned int)seq0, (unsigned int)buf[0]);
#endif
			quisk_sound_state.read_error++;
		}
		seq0 = buf[0] + 1;		// Next expected sequence number
		if (buf[1] & 0x02)					// bit one is ADC overrange
			quisk_sound_state.overrange++;
		index = 2;
		ptxr = (unsigned char *)&xr;
		ptxi = (unsigned char *)&xi;
		// convert 24-bit samples to 32-bit samples; int must be 32 bits.
		while (index < bytes) {
			if (is_little_endian) {
				xr = xi = 0;
				memcpy (ptxr + 1, buf + index, 3);
				index += 3;
				memcpy (ptxi + 1, buf + index, 3);
				index += 3;
				sample = (xr + xi * I) * rx_udp_gain_correct;
			}
			else {		// big-endian
				*(ptxr    ) = buf[index + 2];
				*(ptxr + 1) = buf[index + 1];
				*(ptxr + 2) = buf[index    ];
				*(ptxr + 3) = 0;
				index += 3;
				*(ptxi    ) = buf[index + 2];
				*(ptxi + 1) = buf[index + 1];
				*(ptxi + 2) = buf[index    ];
				*(ptxi + 3) = 0;
				index += 3;
				sample = (xr + xi * I) * rx_udp_gain_correct;
			}
			if (xr & 0x100) {		// channel 1
				if (quisk_invert_spectrum)		// Invert spectrum
					sample = conj(sample);
				// Put samples into the fft input array.
				ptFFT = fft_data_array + fft_data_index;
				if ( ! (xi & 0x100)) {		// zero marker for start of first block
					if (ptFFT->index != 0) {
						//QuiskPrintf("Resync block\n");
						fft_error++;
						ptFFT->index = 0;
					}
					ptFFT->block = block_number = 0;
				}
				else if (ptFFT->index == 0) {
					if (scan_blocks) {
						if (++block_number < scan_blocks)
							ptFFT->block = block_number;
						else
							ptFFT->block = block_number = 0;
					}
					else {
						ptFFT->block = block_number = 0;
					}
					if (scan_blocks && block_number >= scan_blocks)
						QuiskPrintf("Bad block_number %d\n", block_number);
				}
				ptFFT->samples[ptFFT->index] = sample;
				if ((quisk_isFDX || ! key_down) && ++(ptFFT->index) >= fft_size) {		// check sample count
					n = fft_data_index + 1;				// next FFT data location
					if (n >= FFT_ARRAY_SIZE)
						n = 0;
					if (fft_data_array[n].filled == 0) {				// Is the next buffer empty?
						fft_data_array[n].index = 0;
						fft_data_array[n].block = 0;
						fft_data_array[fft_data_index].filled = 1;	// Mark the previous buffer ready.
						fft_data_index = n;							// Write samples into the new buffer.
						ptFFT = fft_data_array + fft_data_index;
					}
					else {				// no place to write samples
						ptFFT->index = 0;
						ptFFT->block = 0;
						fft_error++;
					}
				}
			}
			else {					// channel 0
				cSamples0[nSamples0++] = sample;
			}
		}
	}
	return nSamples0;
}

static PyObject * open_rx_udp(PyObject * self, PyObject * args)
{
	const char * ip;
	int port;
	char buf[128];
	struct sockaddr_in Addr;
	int recvsize;

#if DEBUG_IO
	int intbuf;
#ifdef MS_WINDOWS
	int bufsize = sizeof(int);
#else
	socklen_t bufsize = sizeof(int);
#endif
#endif

#ifdef MS_WINDOWS
	WORD wVersionRequested;
	WSADATA wsaData;
#endif

	if (!PyArg_ParseTuple (args, "si", &ip, &port))
		return NULL;
#ifdef MS_WINDOWS
	wVersionRequested = MAKEWORD(2, 2);
	if (WSAStartup(wVersionRequested, &wsaData) != 0) {
		sprintf(buf, "Failed to initialize Winsock (WSAStartup)");
		return PyString_FromString(buf);
	}
	else {
		cleanupWSA = 1;
	}
#endif
#if DEBUG_IO
    QuiskPrintf("open_rx_udp to IP %s port 0x%X\n", ip, port);
#endif
	quisk_using_udp = 1;
	rx_udp_socket = socket(PF_INET, SOCK_DGRAM, 0);
	if (rx_udp_socket != INVALID_SOCKET) {
		recvsize = 256000;
		setsockopt(rx_udp_socket, SOL_SOCKET, SO_RCVBUF, (char *)&recvsize, sizeof(recvsize));
		memset(&Addr, 0, sizeof(Addr)); 
		Addr.sin_family = AF_INET;
		Addr.sin_port = htons(port);
#ifdef MS_WINDOWS
		Addr.sin_addr.S_un.S_addr = inet_addr(ip);
#else
		inet_aton(ip, &Addr.sin_addr);
#endif
		if (connect(rx_udp_socket, (const struct sockaddr *)&Addr, sizeof(Addr)) != 0) {
			shutdown(rx_udp_socket, QUISK_SHUT_BOTH);
			close(rx_udp_socket);
			rx_udp_socket = INVALID_SOCKET;
			sprintf(buf, "Failed to connect to UDP %s port 0x%X", ip, port);
		}
		else {
			sprintf(buf, "Capture from UDP %s port 0x%X", ip, port);
			if (quisk_use_rx_udp == 17)
				quisk_sample_source(NULL, close_udp, read_rx_udp17);
			else if (quisk_use_rx_udp == 10) {
				quisk_sample_source(NULL, close_udp10, read_rx_udp10);
				init_bandscope();
			}
			else
				quisk_sample_source(NULL, close_udp, quisk_read_rx_udp);
#if DEBUG_IO
			if (getsockopt(rx_udp_socket, SOL_SOCKET, SO_RCVBUF, (char *)&intbuf, &bufsize) == 0)
				QuiskPrintf("UDP socket receive buffer size %d\n", intbuf);
			else
				QuiskPrintf ("Failure SO_RCVBUF\n");
#endif
		}
	}
	else {
		sprintf(buf, "Failed to open socket");
	}
	return PyString_FromString(buf);
}

static PyObject * open_sound(PyObject * self, PyObject * args)
{
    int rate;
	char * mip;

	if (!PyArg_ParseTuple (args, "iiisiiiidi",
				&rate,
				&quisk_sound_state.data_poll_usec,
				&quisk_sound_state.latency_millisecs,
				&mip,
				&quisk_sound_state.tx_audio_port,
				&quisk_sound_state.mic_sample_rate,
				&quisk_sound_state.mic_channel_I,
				&quisk_sound_state.mic_channel_Q,
				&quisk_sound_state.mic_out_volume,
				&quisk_sound_state.mic_playback_rate
				))
		return NULL;

#if SAMPLES_FROM_FILE == 1
    QuiskWavWriteOpen(&hWav, "band.wav", 3, 2, 4, 48000, 1E3 / CLIP32);
#elif SAMPLES_FROM_FILE == 2
    QuiskWavReadOpen(&hWav, "band.wav", 3, 2, 4, 48000, CLIP32 / 1E6);
#endif
	quisk_sound_state.playback_rate = QuiskGetConfigInt("playback_rate", 48000);
	quisk_mic_preemphasis = QuiskGetConfigDouble("mic_preemphasis", 0.6);
	//if (quisk_mic_preemphasis < 0.0 || quisk_mic_preemphasis > 1.0)
	//	quisk_mic_preemphasis = 1.0;
	quisk_mic_clip = QuiskGetConfigDouble("mic_clip", 3.0);
	agc_release_time = QuiskGetConfigDouble("agc_release_time", 1.0);
	strMcpy(quisk_sound_state.mic_ip, mip, IP_SIZE);
	strMcpy(quisk_sound_state.IQ_server, QuiskGetConfigString("IQ_Server_IP", ""), IP_SIZE);
	quisk_sound_state.verbose_sound = quisk_sound_state.verbose_pulse = QuiskGetConfigInt("pulse_audio_verbose_output", 0);
	fft_error = 0;
	quisk_open_sound();
	quisk_open_mic();
	return get_state(NULL, NULL);
}

static void configure_sound_thread(int job)   // called from the sound thread except for job == 1
{
#ifdef MS_WINDOWS
	DWORD taskIndex;
	TIMECAPS tcaps;
	static UINT timer_msec;   // timer resolution in milliseconds;

	switch (job) {
        case 0:         // start sound thread
#if 0
		if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) == 0)
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("Failed to set class priority\n");
		if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) == 0)
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("Failed to set thread priority\n");
#endif
		taskIndex = 0;
		if (AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex) == 0 && quisk_sound_state.verbose_sound)
			QuiskPrintf("Failed to set sound thread to Pro Audio\n");
		timer_msec = 5;
		if (timeGetDevCaps(&tcaps, sizeof(TIMECAPS)) == MMSYSERR_NOERROR) {
			if (timer_msec < tcaps.wPeriodMin)
				timer_msec = tcaps.wPeriodMin;
			else if (timer_msec > tcaps.wPeriodMax)
				timer_msec = tcaps.wPeriodMax;
		}
		if (quisk_sound_state.verbose_sound)
			QuiskPrintf("Set Windows timer resolution to %u milliseconds\n", timer_msec);
		if (timeBeginPeriod(timer_msec) != TIMERR_NOERROR && quisk_sound_state.verbose_sound)
			QuiskPrintf ("Failed to set timer resolution to %u\n", timer_msec);
                break;
        case 1:		// change rxMode
                break;
        case 2:		// stop sound thread
		if (timeEndPeriod(timer_msec) != TIMERR_NOERROR && quisk_sound_state.verbose_sound)
			QuiskPrintf ("Failed to clear timer resolution\n");
                break;
	}
#endif
}

static PyObject * AppStatus(PyObject * self, PyObject * args)
{
        int status;

	if (!PyArg_ParseTuple (args, "i", &status))
		return NULL;
#ifdef MS_WINDOWS
        if (status == 1) {      // App is starting
                // Initialize the critical section one time only.
                InitializeCriticalSectionAndSpinCount(&QuiskCriticalSection, 0x00000400);
        }
        else if (status == 0) {
                // Release resources used by the critical section object.
                DeleteCriticalSection(&QuiskCriticalSection);
        
        }
#endif
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * GetQuiskPrintf(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
#ifdef MS_WINDOWS
        return QuiskPrintf(NULL);
#else
	Py_INCREF (Py_None);
	return Py_None;
#endif
}

static PyObject * close_sound(PyObject * self, PyObject * args)         // called from the sound thread
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	quisk_close_mic();
	quisk_close_sound();
#if SAMPLES_FROM_FILE
    QuiskWavClose(&hWav);
#endif
        configure_sound_thread(2);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * change_scan(PyObject * self, PyObject * args)	// Called from GUI thread
{	// Change to a new FFT rate

	if (!PyArg_ParseTuple (args, "iidii", &scan_blocks, &scan_sample_rate, &scan_valid, &scan_vfo0, &scan_deltaf))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * change_rates(PyObject * self, PyObject * args)	// Called from GUI thread
{	// Change to new sample rates

	multiple_sample_rates = 1;
	if (!PyArg_ParseTuple (args, "iiii", &quisk_sound_state.sample_rate, &vfo_audio, &fft_sample_rate, &vfo_screen))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * change_rate(PyObject * self, PyObject * args)	// Called from GUI thread
{	// Change to a new sample rate
	int rate, avg;

	if (!PyArg_ParseTuple (args, "ii", &rate, &avg))
		return NULL;
	if (multiple_sample_rates) {
		fft_sample_rate = rate;
	}
	else {
		quisk_sound_state.sample_rate = rate;
		fft_sample_rate = rate;
	}
	rx_udp_gain_correct = 0;	// re-calculate JIM
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * read_sound(PyObject * self, PyObject * args)
{
	int n;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
Py_BEGIN_ALLOW_THREADS
	if (quisk_close_file_play) {
		quisk_close_file_play = 0;
		wav_files_close();
	}
	n = quisk_read_sound();
Py_END_ALLOW_THREADS
	return PyInt_FromLong(n);
}

static PyObject * start_sound(PyObject * self, PyObject * args)         // called from the sound thread
{

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	configure_sound_thread(0);
	configure_sound_thread(1);
	quisk_start_sound();
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * mixer_set(PyObject * self, PyObject * args)
{
	char * card_name;
	int numid;
	PyObject * value;
	char err_msg[QUISK_SC_SIZE];

	if (!PyArg_ParseTuple (args, "siO", &card_name, &numid, &value))
		return NULL;

	quisk_alsa_mixer_set(card_name, numid, value, err_msg, QUISK_SC_SIZE);
	return PyString_FromString(err_msg);
}

static PyObject * pc_to_hermes(PyObject * self, PyObject * args)
{
	PyObject * byteArray;

	if (!PyArg_ParseTuple (args, "O", &byteArray))
		return NULL;
	if ( ! PyByteArray_Check(byteArray)) {
		PyErr_SetString (QuiskError, "Object is not a bytearray.");
		return NULL;
	}
	if (PyByteArray_Size(byteArray) != 17 * 4) {
		PyErr_SetString (QuiskError, "Bytearray size must be 17 * 4.");
		return NULL;
	}
	memmove(quisk_pc_to_hermes, PyByteArray_AsString(byteArray), 17 * 4);
	Py_INCREF (Py_None);
	return Py_None;
}

// Changes for HermesLite v2 thanks to Steve, KF7O
static PyObject * pc_to_hermeslite_writequeue(PyObject * self, PyObject * args)
{
	PyObject * byteArray;

	if (!PyArg_ParseTuple (args, "O", &byteArray))
		return NULL;
	if ( ! PyByteArray_Check(byteArray)) {
		PyErr_SetString (QuiskError, "Object is not a bytearray.");
		return NULL;
	}
	if (PyByteArray_Size(byteArray) != 5) {
		PyErr_SetString (QuiskError, "Bytearray size must be 5.");
		return NULL;
	}
	memmove(quisk_hermeslite_writequeue, PyByteArray_AsString(byteArray), 5);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_hermeslite_writepointer(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "I", &quisk_hermeslite_writepointer))
		return NULL;
	if (quisk_hermeslite_writepointer > 4 || quisk_hermeslite_writepointer < 0) {
		PyErr_SetString (QuiskError, "Hermeslite writepointer must be >=0 and <=4.");
		return NULL;		
	}
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * get_hermeslite_writepointer(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return Py_BuildValue("I",quisk_hermeslite_writepointer);
}

static PyObject * get_hermeslite_response(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return PyByteArray_FromStringAndSize((char *)quisk_hermeslite_response, 5);
}

static PyObject * clear_hermeslite_response(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	memset(quisk_hermeslite_response, 0, 5*sizeof(char));
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * hermes_to_pc(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	return PyByteArray_FromStringAndSize((char *)quisk_hermes_to_pc, 5 * 4);
}

static PyObject * set_hermes_id(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "ii", &quisk_hermes_code_version, &quisk_hermes_board_id))
		return NULL;
	switch(quisk_hermes_board_id) {
	default:
	case 3:		// Angelia and Odyssey-2
		bandscopeBlockCount = 32;
		break;
	case 6:		// Hermes Lite
		bandscopeBlockCount = 4;
		break;
	}
	bandscope_size = bandscopeBlockCount * 512;
	Py_INCREF (Py_None);
	return Py_None;
}

#ifdef MS_WINDOWS
static const char * Win_NtoA(unsigned long addr)
{
	static char buf32[32];

        if (addr > 0)
                snprintf(buf32, 32, "%li.%li.%li.%li", (addr>>24)&0xFF, (addr>>16)&0xFF, (addr>>8)&0xFF, (addr>>0)&0xFF);
        else
                buf32[0] = 0;
        return buf32;
}

#else
static const char * Lin_NtoA(struct sockaddr * a)
{
	static char buf32[32];
	unsigned long addr;

	if (a && (addr = ntohl(((struct sockaddr_in *)a)->sin_addr.s_addr)) > 0)
		snprintf(buf32, 32, "%li.%li.%li.%li", (addr>>24)&0xFF, (addr>>16)&0xFF, (addr>>8)&0xFF, (addr>>0)&0xFF);
	else
		buf32[0] = 0;
	return buf32;
}
#endif

static PyObject * ip_interfaces(PyObject * self, PyObject * args)
{
#ifdef MS_WINDOWS
        int i;
        MIB_IPADDRTABLE * ipTable = NULL;
        IP_ADAPTER_INFO * pAdapterInfo;
	PyObject * pylist, * tup;
        MIB_IPADDRROW row;
        ULONG bufLen;
        DWORD ipRet, apRet;
        const char * name;
        unsigned long ipAddr, netmask, baddr;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	pylist = PyList_New(0);
	bufLen = 0;
	for (i=0; i<5; i++) {
		ipRet = GetIpAddrTable(ipTable, &bufLen, 0);
		if (ipRet == ERROR_INSUFFICIENT_BUFFER) {
			free(ipTable);  // in case we had previously allocated it
			ipTable = (MIB_IPADDRTABLE *) malloc(bufLen);
		}
		else if (ipRet == NO_ERROR)
			break;
		else {
			free(ipTable);
			ipTable = NULL;
			break;
		}
	}

	if (ipTable) {
		pAdapterInfo = NULL;
		bufLen = 0;
		for (i=0; i<5; i++) {
			apRet = GetAdaptersInfo(pAdapterInfo, &bufLen);
			if (apRet == ERROR_BUFFER_OVERFLOW) {
				free(pAdapterInfo);  // in case we had previously allocated it
				pAdapterInfo = (IP_ADAPTER_INFO *) malloc(bufLen);
			}
			else if (apRet == ERROR_SUCCESS)
				break;
			else {
				free(pAdapterInfo);
				pAdapterInfo = NULL;
				break;
			}
		}

		for (i=0; i<ipTable->dwNumEntries; i++) {
			row = ipTable->table[i];
			// Now lookup the appropriate adaptor-name in the pAdaptorInfos, if we can find it
			name = NULL;
			if (pAdapterInfo) {
				IP_ADAPTER_INFO * next = pAdapterInfo;
				while((next)&&(name==NULL)) {
					IP_ADDR_STRING * ipAddr = &next->IpAddressList;
					while(ipAddr) {
						if (inet_addr(ipAddr->IpAddress.String) == row.dwAddr) {
							name = next->AdapterName;
							break;
						}
						ipAddr = ipAddr->Next;
					}
					next = next->Next;
				}
			}
			ipAddr  = ntohl(row.dwAddr);
			netmask = ntohl(row.dwMask);
			baddr = ipAddr & netmask;
			if (row.dwBCastAddr)
				baddr |= ~netmask;
			tup = PyTuple_New(4);
			if (name == NULL)
				PyTuple_SetItem(tup, 0, PyString_FromString("unnamed"));
			else
				PyTuple_SetItem(tup, 0, PyString_FromString(name));
			PyTuple_SetItem(tup, 1, PyString_FromString(Win_NtoA(ipAddr)));
			PyTuple_SetItem(tup, 2, PyString_FromString(Win_NtoA(netmask)));
			PyTuple_SetItem(tup, 3, PyString_FromString(Win_NtoA(baddr)));
			PyList_Append(pylist, tup);
			Py_DECREF(tup);
		}
		free(pAdapterInfo);
		free(ipTable);
	}
#else
	PyObject * pylist, * tup;
	struct ifaddrs * ifap, * p;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	pylist = PyList_New(0);
	if (getifaddrs(&ifap) == 0) {
		p = ifap;
		while(p) {
			if ((p->ifa_addr) && p->ifa_addr->sa_family == AF_INET) {
				tup = PyTuple_New(4);
				PyTuple_SetItem(tup, 0, PyString_FromString(p->ifa_name));
				PyTuple_SetItem(tup, 1, PyString_FromString(Lin_NtoA(p->ifa_addr)));
				PyTuple_SetItem(tup, 2, PyString_FromString(Lin_NtoA(p->ifa_netmask)));
				PyTuple_SetItem(tup, 3, PyString_FromString(Lin_NtoA(p->ifa_broadaddr)));
				PyList_Append(pylist, tup);
				Py_DECREF(tup);
			}
			p = p->ifa_next;
		}
		freeifaddrs(ifap);
	}
#endif
	return pylist;
}

static PyObject * invert_spectrum(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quisk_invert_spectrum))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_agc(PyObject * self, PyObject * args)
{  /* Change the AGC level */
	if (!PyArg_ParseTuple (args, "d", &agcReleaseGain))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_filters(PyObject * self, PyObject * args)
{  // Enter the coefficients of the I and Q digital filters.  The storage for
   // filters is not malloc'd because filters may be changed while being used.
   // Multiple filters are available at nFilter.
	PyObject * filterI, * filterQ;
	int i, size, nFilter, bw, start_offset;
	PyObject * obj;
	char buf98[98];

	if (!PyArg_ParseTuple (args, "OOiii", &filterI, &filterQ, &bw, &start_offset, &nFilter))
		return NULL;
	if (PySequence_Check(filterI) != 1) {
		PyErr_SetString (QuiskError, "Filter I is not a sequence");
		return NULL;
	}
	if (PySequence_Check(filterQ) != 1) {
		PyErr_SetString (QuiskError, "Filter Q is not a sequence");
		return NULL;
	}
	size = PySequence_Size(filterI);
	if (size != PySequence_Size(filterQ)) {
		PyErr_SetString (QuiskError, "The size of filters I and Q must be equal");
		return NULL;
	}
	if (size >= MAX_FILTER_SIZE) {
		snprintf(buf98, 98, "Filter size must be less than %d", MAX_FILTER_SIZE);
		PyErr_SetString (QuiskError, buf98);
		return NULL;
	}
	filter_bandwidth[nFilter] = bw;
	if (nFilter == 0)
		filter_start_offset = start_offset;
	for (i = 0; i < size; i++) {
		obj = PySequence_GetItem(filterI, i);
		cFilterI[nFilter][i] = PyFloat_AsDouble(obj);
		Py_XDECREF(obj);
		obj = PySequence_GetItem(filterQ, i);
		cFilterQ[nFilter][i] = PyFloat_AsDouble(obj);
		Py_XDECREF(obj);
	}
	sizeFilter = size;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_auto_notch(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quisk_auto_notch))
		return NULL;
	dAutoNotch(NULL, 0, 0, 0);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_noise_blanker(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quisk_noise_blanker))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_enable_bandscope(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &enable_bandscope))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_rx_mode(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &rxMode))
		return NULL;
	quisk_set_tx_mode();
	quisk_set_play_state();
	configure_sound_thread(1);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_spot_level(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quiskSpotLevel))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_imd_level(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quiskImdLevel))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_mic_out_volume(PyObject * self, PyObject * args)
{
	int level;

	if (!PyArg_ParseTuple (args, "i", &level))
		return NULL;
	quisk_sound_state.mic_out_volume = level / 100.0;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * ImmediateChange(PyObject * self, PyObject * args)	// called from the GUI thread
{
	char * name;

	if (!PyArg_ParseTuple (args, "s", &name))
		return NULL;
	if ( ! strcmp(name, "keyupDelay")) {
		quisk_sound_state.quiskKeyupDelay = QuiskGetConfigInt(name,  23);
	}
	else if ( ! strcmp(name, "cwTone")) {
		quisk_sidetoneFreq = QuiskGetConfigInt(name,  700);
	}
	else if ( ! strcmp(name, "pulse_audio_verbose_output")) {
		quisk_sound_state.verbose_sound = quisk_sound_state.verbose_pulse = QuiskGetConfigInt(name,  0);
	}
	else if ( ! strcmp(name, "start_cw_delay")) {
		quisk_start_cw_delay = QuiskGetConfigInt(name, 15);
		if (quisk_start_cw_delay < 0)
			quisk_start_cw_delay = 0;
		else if (quisk_start_cw_delay > START_CW_DELAY_MAX)
			quisk_start_cw_delay = START_CW_DELAY_MAX;
	}
	else if ( ! strcmp(name, "start_ssb_delay")) {
		quisk_start_ssb_delay = QuiskGetConfigInt(name, 100);
	}
	else if ( ! strcmp(name, "maximum_tx_secs")) {
		maximum_tx_secs = QuiskGetConfigInt(name, 0);
	}
	else if ( ! strcmp(name, "TxRxSilenceMsec")) {
		TxRxSilenceMsec = QuiskGetConfigInt(name, 50);
	}
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_split_rxtx(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &split_rxtx))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_tune(PyObject * self, PyObject * args)
{  /* Change the tuning frequency */
	if (!PyArg_ParseTuple (args, "ii", &rx_tune_freq, &quisk_tx_tune_freq))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_sidetone(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "idii", &quisk_sidetoneCtrl, &quisk_sidetoneVolume, &rit_freq, &quisk_sound_state.quiskKeyupDelay))
		return NULL;
	sidetonePhase = cexp((I * 2.0 * M_PI * abs(rit_freq)) / quisk_sound_state.playback_rate);
	if (rxMode == CWL || rxMode == CWU)
		dAutoNotch(NULL, 0, 0, 0);		// for CW, changing the RIT affects autonotch
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_squelch(PyObject * self, PyObject * args)		// Set level for FM squelch
{
	if (!PyArg_ParseTuple (args, "d", &squelch_level))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_ssb_squelch(PyObject * self, PyObject * args)	// Set level for SSB squelch
{
	if (!PyArg_ParseTuple (args, "ii", &ssb_squelch_enabled, &ssb_squelch_level))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_kill_audio(PyObject * self, PyObject * args)
{	/* replace radio sound with silence */
	if (!PyArg_ParseTuple (args, "i", &kill_audio))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * tx_hold_state(PyObject * self, PyObject * args)
{	// Query or set the transmit hold state
	int i;

	if (!PyArg_ParseTuple (args, "i", &i))
		return NULL;
	if (i >= 0)		// arg < 0 is a Query for the current value
		quiskTxHoldState = i;
	return PyInt_FromLong(quiskTxHoldState);
}

static PyObject * set_transmit_mode(PyObject * self, PyObject * args)
{	/* Set the radio to transmit mode */
	if (!PyArg_ParseTuple (args, "i", &quisk_transmit_mode))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_volume(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "d", &quisk_audioVolume))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_ctcss(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "d", &quisk_ctcss_freq))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_key_down(PyObject * self, PyObject * args)
{
	int down;

	if (!PyArg_ParseTuple (args, "i", &down))
		return NULL;
	quisk_set_key_down(down);
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_hardware_cwkey(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quisk_hardware_cwkey))
		return NULL;
	if (quisk_hardware_cwkey != old_hardware_cwkey) {
		old_hardware_cwkey = quisk_hardware_cwkey;
		quisk_set_play_state();
		//if (quisk_hardware_cwkey) QuiskPrintTime("set hardware cwkey", 0);
	}
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_remote_cwkey(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quisk_remote_cwkey))
		return NULL;
	if (quisk_remote_cwkey != old_remote_cwkey) {
		old_remote_cwkey = quisk_remote_cwkey;
		quisk_set_play_state();
		//if (quisk_remote_cwkey) QuiskPrintTime("set remote cwkey", 0);
	}
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_PTT(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &is_PTT_down))
		return NULL;
	quisk_set_play_state();
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_multirx_mode(PyObject * self, PyObject * args)
{
	int index, mode;

	if (!PyArg_ParseTuple (args, "ii", &index, &mode))
		return NULL;
	if (index < QUISK_MAX_SUB_RECEIVERS)
		multirx_mode[index] = mode;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_multirx_freq(PyObject * self, PyObject * args)
{
	int index, freq;

	if (!PyArg_ParseTuple (args, "ii", &index, &freq))
		return NULL;
	if (index < QUISK_MAX_SUB_RECEIVERS)
		multirx_freq[index] = freq;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_multirx_play_method(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &multirx_play_method))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_multirx_play_channel(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &multirx_play_channel))
		return NULL;
	if (multirx_play_channel >= QUISK_MAX_SUB_RECEIVERS)
		multirx_play_channel = -1;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * get_multirx_graph(PyObject * self, PyObject * args)	// Called by the GUI thread
{
	int i, j, k;
	double d1, d2, scale;
	static double * fft_window=NULL;		// Window for FFT data
	PyObject * retrn, * data;
	static double time0=0;			// time of last graph

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	if ( ! fft_window) {
		// Create the fft window
		fft_window = (double *) malloc(sizeof(double) * multirx_fft_width);
		for (i = 0, j = -multirx_fft_width / 2; i < multirx_fft_width; i++, j++)
			fft_window[i] = 0.5 + 0.5 * cos(2. * M_PI * j / multirx_fft_width);	// Hanning
	}
	retrn = PyTuple_New(2);
	if (multirx_fft_next_state == 1 && QuiskTimeSec() - time0 >= multirx_fft_next_time) {
		time0 = QuiskTimeSec();
		// The FFT is ready to run.  Calculate FFT.
		for (i = 0; i < multirx_fft_width; i++)		// multiply by window
			multirx_fft_next_samples[i] *= fft_window[i];
		fftw_execute(multirx_fft_next_plan);
		// Average the fft data into the graph in order of frequency
		data = PyTuple_New(multirx_data_width);
		scale = log10(multirx_fft_width) + 31.0 * log10(2.0);
		scale *= 20.0;
		j = MULTIRX_FFT_MULT;
		k = 0;
		d1 = 0;
		for (i = multirx_fft_width / 2; i < multirx_fft_width; i++) {			// Negative frequencies
			d1 += cabs(multirx_fft_next_samples[i]);
			if (--j == 0) {
				d2 = 20.0 * log10(d1) - scale;
				if (d2 < -200)
					d2 = -200;
				PyTuple_SetItem(data, k++, PyFloat_FromDouble(d2));
				d1 = 0;
				j = MULTIRX_FFT_MULT;
			}
		}
		for (i = 0; i < multirx_fft_width / 2; i++) {					// Positive frequencies
			d1 += cabs(multirx_fft_next_samples[i]);
			if (--j == 0) {
				d2 = 20.0 * log10(d1) - scale;
				if (d2 < -200)
					d2 = -200;
				PyTuple_SetItem(data, k++, PyFloat_FromDouble(d2));
				d1 = 0;
				j = MULTIRX_FFT_MULT;
			}
		}
		PyTuple_SetItem(retrn, 0, data);
		PyTuple_SetItem(retrn, 1, PyInt_FromLong(multirx_fft_next_index));
		multirx_fft_next_state = 2;			// This FFT is done.
	}
	else {
		data = PyTuple_New(0);
		PyTuple_SetItem(retrn, 0, data);
		PyTuple_SetItem(retrn, 1, PyInt_FromLong(-1));
	}
	return retrn;
}

void copy2pixels(double * pixels, int n_pixels, double * fft, int fft_size, double zoom, double deltaf, double rate)
{
	int i, j, j1, j2;
	double f1, d1, d2, sample;

	f1 = deltaf + rate / 2.0 * (1.0 - zoom);	// frequency at left of graph
	for (i = 0; i < n_pixels; i++) {	// for each pixel
		// freq = f1 + pixel / n_pixels * zoom * rate = rate * fft_index / fft_size
		d1 = fft_size / rate * (f1 + (double)i / n_pixels * zoom * rate);
		d2 = fft_size / rate * (f1 + (double)(i + 1) / n_pixels * zoom * rate);
		j1 = floor(d1);
		j2 = floor(d2);
		if (j1 == j2) {
			sample = (d2 - d1) * fft[j1];
		}
		else {
			sample = (j1 + 1 - d1) * fft[j1];
			for (j = j1 + 1; j < j2; j++)
				sample += fft[j];
			sample += (d2 - j2) * fft[j2];
		}
		pixels[i] = sample;
	}
}

static PyObject * get_bandscope(PyObject * self, PyObject * args)	// Called by the GUI thread
{
	int i, L, clock;
	double zoom, deltaf, rate;
	static int fft_count = 0;
	static double the_max = 0;
	static double time0=0;			// time of last graph
	double d1, sample, frac, scale;
	PyObject * tuple2;

	if (!PyArg_ParseTuple (args, "idd", &clock, &zoom, &deltaf))
		return NULL;

	if (bandscopeState == 99 && bandscopePlan) {	// bandscope samples are ready
		for (i = 0; i < bandscope_size; i++) {
			d1 = fabs(bandscopeSamples[i]);
			if (d1 > the_max)
				the_max = d1;
			bandscopeSamples[i] *= bandscopeWindow[i];	// multiply by window
		}
		fftw_execute(bandscopePlan);		// Calculate forward FFT
		// The return FFT has length bandscope_size / 2 + 1
		L = bandscope_size / 2 + 1;
		for (i = 0; i < L; i++)
			bandscopeAverage[i] += cabs(bandscopeFFT[i]);
		bandscopeState = 0;
		fft_count++;
		if (QuiskTimeSec() - time0 >= 1.0 / graph_refresh) {	// return FFT data
			bandscopeAverage[L] = 0.0;	// in case we run off the end
			// Average the return FFT into the data width
			tuple2 = PyTuple_New(graph_width);
			frac = (double)L / graph_width;
			scale = 1.0 / frac / fft_count / bandscope_size;
			rate = clock / 2.0;
			copy2pixels(bandscopePixels, graph_width, bandscopeAverage, L, zoom, deltaf, rate);
			for (i = 0; i < graph_width; i++) {	// for each pixel
				sample = bandscopePixels[i] * scale;
				if (sample <= 1E-10)
					sample = -200.0;
				else
					sample = 20.0 * log10(sample);
				PyTuple_SetItem(tuple2, i, PyFloat_FromDouble(sample));
			}
			fft_count = 0;
			time0 = QuiskTimeSec();
			hermes_adc_level = the_max;
			the_max = 0;
			for (i = 0; i < L; i++)
				bandscopeAverage[i] = 0;
			return tuple2;
		}
	}
	Py_INCREF(Py_None);	// No data yet
	return Py_None;
}

static PyObject * get_graph(PyObject * self, PyObject * args)	// Called by the GUI thread
{
	int i, j, k, m, n, index, ffts, ii, mm, m0, deltam;
	fft_data * ptFft;
	PyObject * tuple2;
	double d1, d2, scale, smeter_scale, zoom, deltaf;
	complex double c;
	static double meter = 0;	// RMS s-meter
	static int job = 1;		// job==0 return raw data ; 1 return FFT ; 2 delete FFT data
	static double * fft_avg=NULL;	// Array to average the FFT
	static double * fft_tmp;
	static int count_fft=0;			// how many fft's have occurred (for average)
	static double time0=0;			// time of last graph
	static double time_send_graph;		// time of the last send_graph_data()

	if (!PyArg_ParseTuple (args, "idd", &k, &zoom, &deltaf))
		return NULL;
	if (k != job) {		// change in data return type; re-initialize
		job = k;
		count_fft = 0;
	}
	if ( ! fft_avg) {
		fft_avg = (double *) malloc(sizeof(double) * fft_size);
		fft_tmp = (double *) malloc(sizeof(double) * fft_size);
		for (i = 0; i < fft_size; i++)
			fft_avg[i] = 0;
	}
	if (remote_control_head) {
		n = receive_graph_data(fft_avg);
		if (n == data_width) {
			tuple2 = PyTuple_New(data_width);
			for (i = 0; i < data_width; i++)
				PyTuple_SetItem(tuple2, i, PyFloat_FromDouble(fft_avg[i]));
			return tuple2;
		}
		job = 2;
	}
	if (remote_control_slave) {
		if (QuiskTimeSec() - time_send_graph > 1.0) {
			time_send_graph = QuiskTimeSec();
			send_graph_data(NULL, 0, 0.0, 0.0, 0, 0.0);
		}
	}
	// Process all FFTs that are ready to run.
	index = fft_data_index;		// oldest data first - FIFO
	for (ffts = 0; ffts < FFT_ARRAY_SIZE; ffts++) {
		if (++index >= FFT_ARRAY_SIZE)
			index = 0;
		if (fft_data_array[index].filled)
			ptFft = fft_data_array + index;
		else
			continue;
		if (scan_blocks && ptFft->block >= scan_blocks) {
			//QuiskPrintf("Reject block %d\n", ptFft->block);
			ptFft->filled = 0;
			continue;
		}
		if (job == 0) {		// return raw data, not FFT
			tuple2 = PyTuple_New(data_width);
			for (i = 0; i < data_width; i++)
				PyTuple_SetItem(tuple2, i,
					PyComplex_FromDoubles(creal(ptFft->samples[i]), cimag(ptFft->samples[i])));
			ptFft->filled = 0;
			return tuple2;
		}
		if (job == 2) {		// delete data
			ptFft->filled = 0;
			continue;
		}
		// Continue with FFT calculation
		for (i = 0; i < fft_size; i++)		// multiply by window
			ptFft->samples[i] *= fft_window[i];
		fftw_execute_dft(quisk_fft_plan, ptFft->samples, ptFft->samples);	// Calculate FFT
		// Create RMS s-meter value at known bandwidth
		// The pass band is (rx_tune_freq + filter_start_offset) to += bandwidth
		// d1 is the tune frequency
		// d2 is the number of FFT bins required for the bandwidth
		// i is the starting bin number from  - sample_rate / 2 to + sample_rate / 2
		d2 = (double)filter_bandwidth[0] * fft_size / fft_sample_rate;
		if (scan_blocks) {    // Use tx, not rx?? ERROR:
			d1 = ((double)quisk_tx_tune_freq + vfo_screen - scan_vfo0 - scan_deltaf * ptFft->block) * fft_size / scan_sample_rate;
			i = (int)(d1 - d2 / 2 + 0.5);
		}
		else
			i = (int)((double)(rx_tune_freq + filter_start_offset) * fft_size / fft_sample_rate + 0.5);
		n = (int)(floor(d2) + 0.01);		// number of whole bins to add
		if (i > - fft_size / 2 && i + n + 1 < fft_size / 2) {	// too close to edge?
			for (j = 0; j < n; i++, j++) {
				if (i < 0)
					c = ptFft->samples[fft_size + i];	// negative frequencies
				else
					c = ptFft->samples[i];				// positive frequencies
				meter = meter + c * conj(c);		// add square of amplitude
			}
			if (i < 0)			// add fractional next bin
				c = ptFft->samples[fft_size + i];
			else
				c = ptFft->samples[i];
			meter = meter + c * conj(c) * (d2 - n);	// fractional part of next bin
		}
		// Average the fft data into the graph in order of frequency
		if (scan_blocks) {
			if (ptFft->block == (scan_blocks - 1))
				count_fft++;
			k = 0;
			for (i = fft_size / 2; i < fft_size; i++)			// Negative frequencies
				fft_tmp[k++] = cabs(ptFft->samples[i]);
			for (i = 0; i < fft_size / 2; i++)					// Positive frequencies
				fft_tmp[k++] = cabs(ptFft->samples[i]);
			// Average this block into its correct position
			m0 = (int)(fft_size * ((1.0 - scan_valid) / 2.0));
			deltam = (int)(fft_size * scan_valid / scan_blocks);
			m = mm = m0 + ptFft->block * deltam;						// target position
			i = ii = (int)(fft_size * ((1.0 - scan_valid) / 2.0));	// start of valid data
			for (j = 0; j < deltam; j++) {
				d2 = 0;
				for (n = 0; n < scan_blocks; n++)
					d2 += fft_tmp[i++];
				fft_avg[m++] = d2;
			}
			//QuiskPrintf(" %d %.4lf At %5d to %5d place %5d to %5d for block %d\n", fft_size, scan_valid, mm, m, ii, i, ptFft->block);
		}
		else {
			count_fft++;
			k = 0;
			for (i = fft_size / 2; i < fft_size; i++)			// Negative frequencies
				fft_avg[k++] += cabs(ptFft->samples[i]);
			for (i = 0; i < fft_size / 2; i++)					// Positive frequencies
				fft_avg[k++] += cabs(ptFft->samples[i]);
		}
		ptFft->filled = 0;
		if (count_fft > 0 && QuiskTimeSec() - time0 >= 1.0 / graph_refresh) {
			// We have averaged enough fft's to return the graph data.
			scale = 1.0 / 2147483647.0 / fft_size;
			// scale = 1.0 / count_fft / fft_size;	// Divide by sample count
			// scale /= pow(2.0, 31);			// Normalize to max == 1
			scale = log10(count_fft) + log10(fft_size) + 31.0 * log10(2.0);
			scale *= 20.0;
			if (remote_control_slave)	// Send graph data to the control head
				send_graph_data(fft_avg, fft_size, zoom, deltaf, fft_sample_rate, scale);
			// Average the fft data of size fft_size into the size of data_width.
			n = (int)(zoom * (double)fft_size / data_width + 0.5);
			if (n < 1)
				n = 1;
			for (i = 0; i < data_width; i++) {	// For each graph pixel
				// find k, the starting index into the FFT data
				k = (int)(fft_size * (
					deltaf / fft_sample_rate + zoom * ((double)i / data_width - 0.5) + 0.5) + 0.1);
				d2 = 0.0;
				for (j = 0; j < n; j++, k++)
					if (k >= 0 && k < fft_size)
						d2 += fft_avg[k];
				fft_avg[i] = d2;
			}
			smeter_scale = 1.0 / 2147483647.0 / fft_size;
			Smeter = meter * smeter_scale * smeter_scale / count_fft;		// record the new s-meter value
			meter = 0;
			if (Smeter > 1E-16)
				Smeter = 10.0 * log10(Smeter);
			else
				Smeter = -160.0;
			// This correction is for a -40 dB strong signal, and is caused by FFT leakage
			// into adjacent bins. It is the amplitude that is spread out, not the squared amplitude.
			Smeter += 4.25969;
			tuple2 = PyTuple_New(data_width);
			for (i = 0; i < data_width; i++) {
				d2 = 20.0 * log10(fft_avg[i]) - scale;
				if (d2 < -200)
					d2 = -200;
				else if (d2 > 0)
					d2 = 0;
				current_graph[i] = d2;	// graph values are -200.0 to 0.0
				PyTuple_SetItem(tuple2, i, PyFloat_FromDouble(d2));
			}
			for (i = 0; i < fft_size; i++)
				fft_avg[i] = 0;
			count_fft = 0;
			time0 = time_send_graph = QuiskTimeSec();
			return tuple2;
		}
	}
	Py_INCREF(Py_None);	// No data yet
	return Py_None;
}

// These functions are used for the Waterfall display.
static PyObject * watfall_RgbData(PyObject * self, PyObject * args)	// Called by the GUI thread
{
	int i, width, max_height, size;
	Py_buffer red, green, blue;
	PyObject * bytes;
	struct watfall_t watfall;
	struct watfall_row_t * row, * next;

	if (!PyArg_ParseTuple (args, "w*w*w*ii", &red, &green, &blue, &width, &max_height))
		return NULL;

	memcpy(watfall.red, red.buf, 256);
	memcpy(watfall.green, green.buf, 256);
	memcpy(watfall.blue, blue.buf, 256);
	PyBuffer_Release(&red);
	PyBuffer_Release(&green);
	PyBuffer_Release(&blue);
	watfall.width = width;
	watfall.max_height = max_height;
	// malloc space for the maximum number of rows
	size = sizeof(struct watfall_row_t) + width * 3;		// struct plus pixel data for the row
	row = malloc(size);
	memset(row, 0, size);
	row->prior_row = NULL;
	watfall.current_row = row;
	next = NULL;
	for (i = 1; i < max_height; i++) {
		next = malloc(size);
		memset(next, 0, size);
		next->prior_row = row;
		row->next_row = next;
		row = next;
	}
	next->next_row = watfall.current_row;
	watfall.current_row->prior_row = next;
	bytes = PyByteArray_FromStringAndSize((const char *)&watfall, sizeof(watfall));
	return bytes;
}

static PyObject * watfall_OnGraphData(PyObject * self, PyObject * args)	// Called by the GUI thread
{
	int i, l, y_zero, y_scale, x_origin, size;
	double yz, dB, gain;
	uint8_t * pPixels;
	Py_buffer rgb_data;
	PyObject * db_list, * obj;
	struct watfall_t * pWatfall;
	struct watfall_row_t * pRow;

	if (!PyArg_ParseTuple (args, "w*Oiidi", &rgb_data, &db_list, &y_zero, &y_scale, &gain, &x_origin))
		return NULL;
	if (PySequence_Check(db_list) != 1) {
		PyErr_SetString (QuiskError, "List of dB data is not a sequence");
		return NULL;
	}

	pWatfall = (struct watfall_t *)rgb_data.buf;
	pWatfall->current_row = pWatfall->current_row->prior_row;
	pRow = pWatfall->current_row;
	// replace data in oldest row
	pRow->x_origin = x_origin;
	pPixels = pRow->pixels;
	size = PySequence_Size(db_list);
	if (size > pWatfall->width)
		size = pWatfall->width;
	yz = 40.0 + y_zero * 0.69;		// -yz is the color center in dB
	for (i = 0; i < size; i++) {
		obj = PySequence_GetItem(db_list, i);
		dB = PyFloat_AsDouble(obj);	// x is -130 to 0, or so (dB)
		Py_DECREF(obj);
		l = (int)((dB - gain + yz) * (y_scale + 10) * 0.10 + 128);
		if (l < 0)
			l = 0;
		else if(l > 255)
			l = 255;
		*pPixels++ = pWatfall->red[l];
		*pPixels++ = pWatfall->green[l];
		*pPixels++ = pWatfall->blue[l];
	}
	for ( ; i < pWatfall->width; i++) {
		*pPixels++ = 0;
		*pPixels++ = 0;
		*pPixels++ = 0;
	}
	PyBuffer_Release(&rgb_data);
	Py_INCREF(Py_None);
	return Py_None;
}

static uint8_t * watfall_copy(uint8_t * dest, uint8_t * source, int x, int width)
{ // Copy the source to location x in the dest. The source and dest have size width.
	if (x == 0) {
		memcpy(dest, source, width);
	}
	else if (x >= width || x + width <= 0) {	// no overlap
		memset(dest, 0, width);
	}
	else if (x > 0) {
		memset(dest, 0, x);
		memcpy(dest + x, source, width - x);
	}
	else {
		x = -x;
		memcpy(dest, source + x, width - x);
		memset(dest + width - x, 0, x);
	}
	return dest + width;
}

static PyObject * watfall_GetPixels(PyObject * self, PyObject * args)	// Called by the GUI thread
{
	int i, j, x, x_origin, width, width3, height;
	Py_buffer rgb_data, pixels;
	struct watfall_t * pWatfall;
	struct watfall_row_t * pRow;
	uint8_t * pDest;

	if (!PyArg_ParseTuple (args, "w*w*iii", &rgb_data, &pixels, &x_origin, &width, &height))
		return NULL;

	width3 = width * 3;
	pDest = pixels.buf;
	pWatfall = (struct watfall_t *)rgb_data.buf;
	pRow = pWatfall->current_row;
	if (waterfall_scroll_mode) {
		for (j = 8; j > 1; j--) {
			x = pRow->x_origin - x_origin;
			x *= 3;
			for (i = 0; i < j; i++) {		// Copy the first rows multiple times
				pDest = watfall_copy(pDest, pRow->pixels, x, width3);
				height--;
			}
			pRow = pRow->next_row;
		}
	}
	for (i = 0; i < height; i++) {
		x = pRow->x_origin - x_origin;
		x *= 3;
		pDest = watfall_copy(pDest, pRow->pixels, x, width3);
		pRow = pRow->next_row;
	}
	PyBuffer_Release(&rgb_data);
	PyBuffer_Release(&pixels);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject * get_filter(PyObject * self, PyObject * args)
{
	int i, j, k, n;
	int freq, time;
	PyObject * tuple2;
	complex double cx;
	double d2, scale, accI, accQ;
	double * average, * bufI, * bufQ;
	double phase, delta;
	static fftw_complex * samples;
	static fftw_plan plan;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;

	// Create space for the fft of size data_width
	samples = (fftw_complex *) fftw_malloc(sizeof(fftw_complex) * data_width);
	plan = fftw_plan_dft_1d(data_width, samples, samples, FFTW_FORWARD, FFTW_MEASURE);
	average = (double *) malloc(sizeof(double) * (data_width + sizeFilter));
	bufI = (double *) malloc(sizeof(double) * sizeFilter);
	bufQ = (double *) malloc(sizeof(double) * sizeFilter);

	for (i = 0; i < data_width + sizeFilter; i++)
		average[i] = 0.5;	// Value for freq == 0
	for (freq = 1; freq < data_width / 2.0 - 10.0; freq++) {
		delta = 2 * M_PI / data_width * freq;
		phase = 0;
		// generate some initial samples to fill the filter pipeline
		for (time = 0; time < data_width + sizeFilter; time++) {
			average[time] += cos(phase);	// current sample
			phase += delta;
			if (phase > 2 * M_PI)
				phase -= 2 * M_PI;
		}
	}
	// now filter the signal
	n = 0;
	for (time = 0; time < data_width + sizeFilter; time++) {
		d2 = average[time];
		bufI[n] = d2;
		bufQ[n] = d2;
		accI = accQ = 0;
		j = n;
		for (k = 0; k < sizeFilter; k++) {
			accI += bufI[j] * cFilterI[0][k];
			accQ += bufQ[j] * cFilterQ[0][k];
			if (++j >= sizeFilter)
				j = 0;
		}
		cx = accI + I * accQ;	// Filter output
		if (++n >= sizeFilter)
			n = 0;
		if (time >= sizeFilter)
			samples[time - sizeFilter] = cx;
	}

	for (i = 0; i < data_width; i++)	// multiply by window
		samples[i] *= fft_window[i];
	fftw_execute(plan);		// Calculate FFT
	// Normalize and convert to log10
	scale = 1. / data_width;
	for (k = 0; k < data_width; k++) {
		cx = samples[k];
		average[k] = cabs(cx) * scale;
		if (average[k] <= 1e-7)		// limit to -140 dB
			average[k] = -7;
		else
			average[k] = log10(average[k]);
	}
	// Return the graph data
	tuple2 = PyTuple_New(data_width);
	i = 0;
	// Negative frequencies:
	for (k = data_width / 2; k < data_width; k++, i++)
		PyTuple_SetItem(tuple2, i, PyFloat_FromDouble(20.0 * average[k]));

	// Positive frequencies:
	for (k = 0; k < data_width / 2; k++, i++)
		PyTuple_SetItem(tuple2, i, PyFloat_FromDouble(20.0 * average[k]));

	free(bufQ);
	free(bufI);
	free(average);
	fftw_destroy_plan(plan);
	fftw_free(samples);

	return tuple2;
}

static PyObject * quisk_control_midi(PyObject * self, PyObject * args, PyObject * keywds)
{
#ifdef QUISK_HAVE_ALSA
	return quisk_alsa_control_midi(self, args, keywds);
#else
	return quisk_wasapi_control_midi(self, args, keywds);
#endif
}

static void measure_freq(complex double * cSamples, int nSamples, int srate)
{
	int i, k, center, ipeak;
	double dmax, c3, freq;
	complex double cBuffer[SAMP_BUFFER_SIZE];
	static int index = 0;				// current index of samples
	static int fft_size=12000;			// size of fft data
	static int fft_count=0;				// number of ffts for the average
	static fftw_complex * samples;		// complex data for fft
	static fftw_plan planA;				// fft plan for fft
	static double * fft_window;			// window function
	static double * fft_average;		// average amplitudes
	static struct quisk_cHB45Filter HalfBand1 = {NULL, 0, 0};
	static struct quisk_cHB45Filter HalfBand2 = {NULL, 0, 0};
	static struct quisk_cHB45Filter HalfBand3 = {NULL, 0, 0};

	if ( ! cSamples) {		// malloc new space and initialize
		samples = (fftw_complex *) fftw_malloc(sizeof(fftw_complex) * fft_size);
		planA = quisk_create_or_cache_fftw_plan_dft_1d(fft_size, samples, samples, FFTW_FORWARD, FFTW_MEASURE);
		fft_window = (double *) malloc(sizeof(double) * (fft_size + 1));
		fft_average = (double *) malloc(sizeof(double) * fft_size);
		memset(fft_average, 0, sizeof(double) * fft_size);
		for (i = 0; i < fft_size; i++)	// Hanning
			fft_window[i] = 0.50 - 0.50 * cos(2. * M_PI * i / (fft_size - 1));
		return;
	}
	memcpy(cBuffer, cSamples, nSamples * sizeof(complex double));	// do not destroy cSamples
	nSamples = quisk_cDecim2HB45(cBuffer, nSamples, &HalfBand1);
	nSamples = quisk_cDecim2HB45(cBuffer, nSamples, &HalfBand2);
	nSamples = quisk_cDecim2HB45(cBuffer, nSamples, &HalfBand3);
	srate /= 8;		// sample rate as decimated
	for (i = 0; i < nSamples && index < fft_size; i++, index++)
		samples[index] = cBuffer[i];
	if (index < fft_size)
		return;		// wait for a full array of samples
	for (i = 0; i < fft_size; i++)	// multiply by window
		samples[i] *= fft_window[i];
	fftw_execute(planA);		// Calculate FFT
	index = 0;
	fft_count++;
	// Average the fft data into the graph in order of frequency
	k = 0;
	for (i = fft_size / 2; i < fft_size; i++)			// Negative frequencies
		fft_average[k++] += cabs(samples[i]);
	for (i = 0; i < fft_size / 2; i++)					// Positive frequencies
		fft_average[k++] += cabs(samples[i]);
	if (fft_count < measure_freq_mode / 2)
		return;		// continue with average
	// time for a calculation
	fft_count = 0;
	dmax = 1.e-20;
	ipeak = 0;
	center = fft_size / 2 - rit_freq * fft_size / srate;
	k = 500;	// desired +/- half-bandwidth to search for a peak
	k = k * fft_size / srate;		// convert to index
	for (i = center - k; i <= center + k; i++) {	// search for a peak near the RX freq
		if (fft_average[i] > dmax) {
			dmax = fft_average[i];
			ipeak = i;
		}
	}
	c3 = 1.36 * (fft_average[ipeak+1] - fft_average[ipeak - 1]) / (fft_average[ipeak-1] + fft_average[ipeak] + fft_average[ipeak+1]);
	freq = srate * (2 * (ipeak + c3) - fft_size) / 2 / fft_size;
	freq += rx_tune_freq;
	//QuiskPrintf("freq %.0f rx_tune_freq %d vfo_screen %d vfo_audio %d\n", freq, rx_tune_freq, vfo_screen, vfo_audio);
	// QuiskPrintf("\n%5d %.4lf %.2lf k=%d\n", ipeak, c3, freq, k);
	measured_frequency = freq;
	//for (i = ipeak - 10; i <= ipeak + 10 && i >= 0 && i < fft_size; i++)
	//	QuiskPrintf("%4d %12.5f\n", i, fft_average[i] / dmax);
	memset(fft_average, 0, sizeof(double) * fft_size);
}

static PyObject * Xdft(PyObject * pyseq, int inverse, int window)
{  // Native spectral order is 0 Hz to (Fs - 1).  Change this to
   // - (Fs - 1)/2 to + Fs/2.  For even Fs==32, there are 15 negative
   // frequencies, a zero, and 16 positive frequencies.  For odd Fs==31,
   // there are 15 negative and positive frequencies plus zero frequency.
   // Note that zero frequency is always index (Fs - 1) / 2.
	PyObject * obj;
	int i, j, size;
	static int fft_size = -1;			// size of fft data
	static fftw_complex * samples;		// complex data for fft
	static fftw_plan planF, planB;		// fft plan for fftW
	static double * fft_window;			// window function
	Py_complex pycx;					// Python C complex value

	if (PySequence_Check(pyseq) != 1) {
		PyErr_SetString (QuiskError, "DFT input data is not a sequence");
		return NULL;
	}
	size = PySequence_Size(pyseq);
	if (size <= 0)
		return PyTuple_New(0);
	if (size != fft_size) {		// Change in previous size; malloc new space
		if (fft_size > 0) {
			fftw_destroy_plan(planF);
			fftw_destroy_plan(planB);
			fftw_free(samples);
			free (fft_window);
		}
		fft_size = size;	// Create space for one fft
		samples = (fftw_complex *) fftw_malloc(sizeof(fftw_complex) * fft_size);
		planF = fftw_plan_dft_1d(fft_size, samples, samples, FFTW_FORWARD, FFTW_MEASURE);
		planB = fftw_plan_dft_1d(fft_size, samples, samples, FFTW_BACKWARD, FFTW_MEASURE);
		fft_window = (double *) malloc(sizeof(double) * (fft_size + 1));
		for (i = 0; i <= size/2; i++) {
			if (1)	// Blackman window
				fft_window[i] = fft_window[size - i] = 0.42 + 0.50 * cos(2. * M_PI * i / size) + 
					0.08 * cos(4. * M_PI * i / size);
			else if (1)	// Hamming
				fft_window[i] = fft_window[size - i] = 0.54 + 0.46 * cos(2. * M_PI * i / size);
			else	// Hanning
				fft_window[i] = fft_window[size - i] = 0.50 + 0.50 * cos(2. * M_PI * i / size);
		}
	}
	j = (size - 1) / 2;		// zero frequency in input
	for (i = 0; i < size; i++) {
		obj = PySequence_GetItem(pyseq, j);
		if (PyComplex_Check(obj)) {
			pycx = PyComplex_AsCComplex(obj);
		}
		else if (PyFloat_Check(obj)) {
			pycx.real = PyFloat_AsDouble(obj);
			pycx.imag = 0;
		}
		else if (PyInt_Check(obj)) {
			pycx.real = PyInt_AsLong(obj);
			pycx.imag = 0;
		}
		else {
			Py_XDECREF(obj);
			PyErr_SetString (QuiskError, "DFT input data is not a complex/float/int number");
			return NULL;
		}
		samples[i] = pycx.real + I * pycx.imag;
		if (++j >= size)
			j = 0;
		Py_XDECREF(obj);
	}
	if (inverse) {		// Normalize using 1/N
		fftw_execute(planB);		// Calculate inverse FFT / N
		if (window) {
			for (i = 0; i < fft_size; i++)	// multiply by window / N
				samples[i] *= fft_window[i] / size;
		}
		else {
			for (i = 0; i < fft_size; i++)	// divide by N
				samples[i] /= size;
		}
	}
	else {
		if (window) {
			for (i = 0; i < fft_size; i++)	// multiply by window
				samples[i] *= fft_window[i];
	   }
		fftw_execute(planF);		// Calculate FFT
	}
	pyseq = PyList_New(fft_size);
	j = (size - 1) / 2;		// zero frequency in input
	for (i = 0; i < fft_size; i++) {
		pycx.real = creal(samples[i]);
		pycx.imag = cimag(samples[i]);
		PyList_SetItem(pyseq, j, PyComplex_FromCComplex(pycx));
		if (++j >= size)
			j = 0;
	}
	return pyseq;
}

static PyObject * dft(PyObject * self, PyObject * args)
{
	PyObject * tuple2;
	int window;

	window = 0;
	if (!PyArg_ParseTuple (args, "O|i", &tuple2, &window))
		return NULL;
	return Xdft(tuple2, 0, window);
}

static PyObject * is_cwkey_down(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	if (QUISK_CWKEY_DOWN)
		return PyInt_FromLong(1);
	else
		return PyInt_FromLong(0);
}

static PyObject * is_key_down(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	if (quisk_is_key_down())
		return PyInt_FromLong(1);
	else
		return PyInt_FromLong(0);
}

static PyObject * idft(PyObject * self, PyObject * args)
{
	PyObject * tuple2;
	int window;

	window = 0;
	if (!PyArg_ParseTuple (args, "O|i", &tuple2, &window))
		return NULL;
	return Xdft(tuple2, 1, window);
}

void quisk_set_key_down(int state)
{		// Set the key state internally. Treat as a software PTT except that it works in CW mode too.
	if (state)
		key_is_down = 1;
	else
		key_is_down = 0;
	quisk_set_play_state();
}

int quisk_is_key_down(void)
{
	return quisk_transmit_mode || quisk_play_state > RECEIVE;
}

static void set_stone(void)
{
	if ( ! quisk_use_sidetone || quisk_isFDX)
		quisk_active_sidetone = 0;		// No sidetone
	else if (quisk_Playback.driver == DEV_DRIVER_ALSA)
		quisk_active_sidetone = 3;		// Sidetone from alsa
	else if (quisk_Playback.driver == DEV_DRIVER_PULSEAUDIO)
		quisk_active_sidetone = 4;		// Sidetone from pulseaudio
	else if (quisk_Playback.driver == DEV_DRIVER_WASAPI2)
		quisk_active_sidetone = 1;		// Sidetone from wasapi
	else
		quisk_active_sidetone = 2;		// Sidetone from old logic
}

#define IS_HW_CWKEY	quisk_hardware_cwkey
#define IS_SW_CWKEY	(quisk_serial_key_down || quisk_midi_cwkey || quisk_remote_cwkey)
#define IS_HW_PTT	hardware_ptt
#define IS_SW_PTT	(quisk_serial_ptt || key_is_down || is_PTT_down)

void quisk_set_play_state(void)
{
	static double Time0;			// Timer to change to RX after the key goes up. CW hang time and PTT.
	static double TimeoutTimer = 1E30;	// Timer for maximum Tx time limit.
	//static int change = 0;
	//int i;

	if (quisk_use_rx_udp == 10 && IS_HW_CWKEY && quisk_play_state != HARDWARE_CWKEY) {
		// work around HL2 gateware bug that uses CWX when key goes down
		Time0 = TimeoutTimer = QuiskTimeSec();
		quisk_play_state = HARDWARE_CWKEY;
		set_stone();
		quisk_play_sidetone(&quisk_Playback);
		hermes_mox_bit = 0;
	}
	switch (quisk_play_state) {
	case SHUTDOWN:
		Time0 = QuiskTimeSec();
		quisk_active_sidetone = 0;
		hermes_mox_bit = 0;
		break;
	case STARTING:
		quisk_active_sidetone = 0;
		if (QuiskTimeSec() - Time0 > 0.500) {
			quisk_play_state = RECEIVE;
	                if (quisk_sound_state.verbose_sound)
                                QuiskPrintf("Change from state Starting to Receive\n");
		}
		hermes_mox_bit = 0;
		break;
	case RECEIVE:
		quisk_active_sidetone = 0;
		if (rxMode == CWU || rxMode == CWL) {
			if (key_is_down) {
				Time0 = TimeoutTimer = QuiskTimeSec();
				quisk_play_state = SOFTWARE_PTT;
				hermes_mox_bit = 1;
			}
			else if (IS_HW_CWKEY) {
				Time0 = TimeoutTimer = QuiskTimeSec();
				quisk_play_state = HARDWARE_CWKEY;
				set_stone();
				quisk_play_sidetone(&quisk_Playback);
				hermes_mox_bit = 1;
			}
			else if (IS_SW_CWKEY) {
				Time0 = TimeoutTimer = QuiskTimeSec();
				quisk_play_state = SOFTWARE_CWKEY;
				set_stone();
				quisk_play_sidetone(&quisk_Playback);
				hermes_mox_bit = 1;
			}
		}
		else {
			if (IS_HW_PTT) {
				Time0 = TimeoutTimer = QuiskTimeSec();
				quisk_play_state = HARDWARE_PTT;
				hermes_mox_bit = 1;
			}
			else if (IS_SW_PTT) {
				Time0 = TimeoutTimer = QuiskTimeSec();
				quisk_play_state = SOFTWARE_PTT;
				hermes_mox_bit = 1;
			}
		}
		break;
	case HARDWARE_CWKEY:
		if (IS_HW_CWKEY)
			Time0 = QuiskTimeSec();
		else if (QuiskTimeSec() - Time0 >= quisk_sound_state.quiskKeyupDelay * 1E-3) {
			quisk_play_state = RECEIVE;
			quisk_play_sidetone(&quisk_Playback);
			quisk_active_sidetone = 0;
			hermes_mox_bit = 0;
		}
		break;
	case SOFTWARE_CWKEY:
		if (IS_SW_CWKEY)
			Time0 = QuiskTimeSec();
		else if (QuiskTimeSec() - Time0 >= quisk_sound_state.quiskKeyupDelay * 1E-3) {
			quisk_play_state = RECEIVE;
			quisk_play_sidetone(&quisk_Playback);
			quisk_active_sidetone = 0;
			hermes_mox_bit = 0;
		}
		break;
	case HARDWARE_PTT:
		quisk_active_sidetone = 0;
		if (IS_HW_PTT)
			Time0 = QuiskTimeSec();
		else if (QuiskTimeSec() - Time0 >= 50E-3) {
			quisk_play_state = RECEIVE;
			hermes_mox_bit = 0;
		}
		break;
	case SOFTWARE_PTT:
		quisk_active_sidetone = 0;
		if (IS_SW_PTT)
			Time0 = QuiskTimeSec();
		else if (QuiskTimeSec() - Time0 >= 50E-3) {
			quisk_play_state = RECEIVE;
			hermes_mox_bit = 0;
		}
		break;
	}
	if (maximum_tx_secs && quisk_play_state > RECEIVE && QuiskTimeSec() - TimeoutTimer >= maximum_tx_secs) {
		quisk_hardware_cwkey = 0;
		quisk_serial_key_down = 0;
		quisk_serial_ptt = 0;
		quisk_midi_cwkey = 0;
		hardware_ptt = 0;
		key_is_down = 0;
		is_PTT_down = 0;
		hermes_mox_bit = 0;
	}
	//i = quisk_play_state + hermes_mox_bit * 19 + IS_HW_PTT * 100 + IS_HW_CWKEY * 1000 + IS_SW_PTT * 10000 + IS_SW_CWKEY * 100000;
	//if (i != change) {
	//	change = i;
	//	QuiskPrintf("quisk_play_state %d hermes_mox_bit %d IS_HW_PTT %d IS_HW_CWKEY %d IS_SW_PTT %d IS_SW_CWKEY %d\n",
	//		quisk_play_state, hermes_mox_bit, IS_HW_PTT, IS_HW_CWKEY, IS_SW_PTT, IS_SW_CWKEY);
	//}
}

static PyObject * record_app(PyObject * self, PyObject * args)
{  // Record the Python object for the application instance, malloc space for fft's.
	int i, j, rate;
	unsigned long handle;
	fftw_complex * pt;
	char * name;
        const char * utf8 = "utf-8";
	Py_ssize_t l1;

	name = malloc(QUISK_SC_SIZE);
	l1 = QUISK_SC_SIZE;
	if (!PyArg_ParseTuple (args, "OOiiiiikes#", &pyApp, &quisk_pyConfig, &data_width, &graph_width,
		&fft_size, &multirx_data_width, &rate, &handle, utf8, &name, &l1))
		return NULL;
	strMcpy(fftw_wisdom_name, name, QUISK_SC_SIZE);
        free(name);

	Py_INCREF(quisk_pyConfig);

#ifdef MS_WINDOWS
#ifdef _WIN64
        quisk_mainwin_handle = (HWND)(unsigned long long)handle;
#else
        quisk_mainwin_handle = (HWND)handle;
#endif
#endif
	fftw_import_wisdom_from_filename(fftw_wisdom_name);
	rx_udp_clock = QuiskGetConfigDouble("rx_udp_clock", 122.88e6);
	graph_refresh = QuiskGetConfigInt("graph_refresh", 7);
	quisk_use_rx_udp = QuiskGetConfigInt("use_rx_udp", 0);
	quisk_sidetoneFreq = QuiskGetConfigInt("cwTone", 700);
	waterfall_scroll_mode = QuiskGetConfigInt("waterfall_scroll_mode", 1);
	quisk_use_sidetone = QuiskGetConfigInt("use_sidetone", 0);
	quisk_start_cw_delay = QuiskGetConfigInt("start_cw_delay", 15);
	quisk_start_ssb_delay = QuiskGetConfigInt("start_ssb_delay", 100);
	maximum_tx_secs = QuiskGetConfigInt("maximum_tx_secs", 0);
	TxRxSilenceMsec = QuiskGetConfigInt("TxRxSilenceMsec", 50);
	quisk_sound_state.sample_rate = rate;
	fft_sample_rate = rate;
	is_little_endian = 1;	// Test machine byte order
	if (*(char *)&is_little_endian == 1)
		is_little_endian = 1;
	else
		is_little_endian = 0;
	strMcpy (quisk_sound_state.err_msg, CLOSED_TEXT, QUISK_SC_SIZE);
	// Initialize space for the FFTs
	for (i = 0; i < FFT_ARRAY_SIZE; i++) {
		fft_data_array[i].filled = 0;
		fft_data_array[i].index = 0;
		fft_data_array[i].block = 0;
		fft_data_array[i].samples = (fftw_complex *) fftw_malloc(sizeof(fftw_complex) * fft_size);
	}
	pt = fft_data_array[0].samples;
	quisk_fft_plan = quisk_create_or_cache_fftw_plan_dft_1d(fft_size, pt, pt, FFTW_FORWARD, FFTW_MEASURE);
	// Create space for the fft average and window
	if (fft_window)
		free(fft_window);
	fft_window = (double *) malloc(sizeof(double) * fft_size);
	for (i = 0, j = -fft_size / 2; i < fft_size; i++, j++) {
		if (0)	// Hamming
			fft_window[i] = 0.54 + 0.46 * cos(2. * M_PI * j / fft_size);
		else	// Hanning
			fft_window[i] = 0.5 + 0.5 * cos(2. * M_PI * j / fft_size);
	}
	// Initialize plan for multirx FFT
	multirx_fft_width = multirx_data_width * MULTIRX_FFT_MULT;		// Use larger FFT than graph size
	multirx_fft_next_samples = (fftw_complex *)malloc(multirx_fft_width * sizeof(fftw_complex));
	multirx_fft_next_plan = quisk_create_or_cache_fftw_plan_dft_1d(multirx_fft_width, multirx_fft_next_samples, multirx_fft_next_samples, FFTW_FORWARD, FFTW_MEASURE);
	if (current_graph)
		free(current_graph);
	current_graph = (double *) malloc(sizeof(double) * data_width);
	measure_freq(NULL, 0, 0);
	dAutoNotch(NULL, 0, 0, 0);
	quisk_process_decimate(NULL, 0, 0, 0);
	quisk_process_demodulate(NULL, NULL, 0, 0, 0, 0);
#if DEBUG_IO
	QuiskPrintTime(NULL, 0);
#endif
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * record_graph(PyObject * self, PyObject * args)
{  /* record the Python object for the application instance */
	if (!PyArg_ParseTuple (args, "iid", &graphX, &graphY, &graphScale))
		return NULL;
	graphScale *= 2;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * test_1(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * test_2(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * test_3(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_fdx(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &quisk_isFDX))
		return NULL;
	quisk_set_play_state();
	Py_INCREF (Py_None);
	return Py_None;
}

static PyObject * set_sample_bytes(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, "i", &sample_bytes))
		return NULL;
	Py_INCREF (Py_None);
	return Py_None;
}

static PyMethodDef QuiskMethods[] = {
	{"add_tone", add_tone, METH_VARARGS, "Add a test tone to the data."},
	{"dft", dft, METH_VARARGS, "Calculate the discrete Fourier transform."},
	{"idft", idft, METH_VARARGS, "Calculate the inverse discrete Fourier transform."},
	{"is_key_down", is_key_down, METH_VARARGS, "Check whether the key is down; return 0 or 1."},
	{"is_cwkey_down", is_cwkey_down, METH_VARARGS, "Check whether the CW key is down; return 0 or 1."},
	{"get_state", get_state, METH_VARARGS, "Return a count of read and write errors."},
	{"get_graph", get_graph, METH_VARARGS, "Return a tuple of graph data."},
	{"get_bandscope", get_bandscope, METH_VARARGS, "Return a tuple of bandscope data."},
	{"set_multirx_mode", set_multirx_mode, METH_VARARGS, "Select demodulation mode for sub-receivers."},
	{"set_multirx_freq", set_multirx_freq, METH_VARARGS, "Select how to play audio from sub-receivers."},
	{"set_multirx_play_method", set_multirx_play_method, METH_VARARGS, "Select how to play audio from sub-receivers."},
	{"set_multirx_play_channel", set_multirx_play_channel, METH_VARARGS, "Select which sub-receiver to play audio."},
	{"get_multirx_graph", get_multirx_graph, METH_VARARGS, "Return a tuple of sub-receiver graph data."},
	{"get_filter", get_filter, METH_VARARGS, "Return the frequency response of the receive filter."},
	{"get_filter_rate", get_filter_rate, METH_VARARGS, "Return the sample rate used for the filters."},
	{"get_tx_filter", quisk_get_tx_filter, METH_VARARGS, "Return the frequency response of the transmit filter."},
	{"get_audio_graph", get_audio_graph, METH_VARARGS, "Return a tuple of the audio graph data."},
	{"measure_frequency", measure_frequency, METH_VARARGS, "Set the method, return the measured frequency."},
	{"measure_audio", measure_audio, METH_VARARGS, "Set the method, return the measured audio voltage."},
	{"get_overrange", get_overrange, METH_VARARGS, "Return the count of overrange (clip) for the ADC."},
	{"get_smeter", get_smeter, METH_VARARGS, "Return the S meter reading."},
	{"get_hermes_adc", get_hermes_adc, METH_VARARGS, "Return the ADC peak level."},
	{"get_hermes_TFRC", get_hermes_TFRC, METH_VARARGS, "Return the temperature, forward and reverse power and PA current."},
	{"set_hermes_id", set_hermes_id, METH_VARARGS, "Set the Hermes hardware code version and board ID."},
	{"set_hermes_filters", quisk_set_hermes_filter, METH_VARARGS, "Set the Hermes filter to use for Rx and Tx."},
	{"invert_spectrum", invert_spectrum, METH_VARARGS, "Invert the input RF spectrum"},
	{"ip_interfaces", ip_interfaces, METH_VARARGS, "Return a list of interface data"},
	{"pc_to_hermes", pc_to_hermes, METH_VARARGS, "Send this block of control data to the Hermes device"},
	{"pc_to_hermeslite_writequeue", pc_to_hermeslite_writequeue, METH_VARARGS, "Fill Hermes-Lite write queue"},	
	{"set_hermeslite_writepointer", set_hermeslite_writepointer, METH_VARARGS, "Set Hermes-Lite write pointer"},	
	{"get_hermeslite_writepointer", get_hermeslite_writepointer, METH_VARARGS, "Return Hermes-Lite write pointer"},
	{"clear_hermeslite_response", clear_hermeslite_response, METH_VARARGS, "Clear the Hermes-Lite response array"},
	{"get_hermeslite_response", get_hermeslite_response, METH_VARARGS, "Get the Hermes-Lite response array"},
	{"hermes_to_pc", hermes_to_pc, METH_VARARGS, "Get the block of control data from the Hermes device"},
	{"record_app", record_app, METH_VARARGS, "Save the App instance."},
	{"record_graph", record_graph, METH_VARARGS, "Record graph parameters."},
	{"ImmediateChange", ImmediateChange, METH_VARARGS, "Call this to notify the program of changes."},
	{"set_ampl_phase", quisk_set_ampl_phase, METH_VARARGS, "Set the sound card amplitude and phase corrections."},
	{"set_udp_tx_correct", quisk_set_udp_tx_correct, METH_VARARGS, "Set the UDP transmit corrections."},
	{"set_agc", set_agc, METH_VARARGS, "Set the AGC parameters."},
	{"set_squelch", set_squelch, METH_VARARGS, "Set the FM squelch parameter."},
	{"get_squelch", get_squelch, METH_VARARGS, "Get the FM squelch state, 0 or 1."},
	{"set_ssb_squelch", set_ssb_squelch, METH_VARARGS, "Set the SSB squelch parameters."},
	{"set_ctcss", set_ctcss, METH_VARARGS, "Set the frequency of the repeater access tone."},
	{"set_file_name", (PyCFunction)quisk_set_file_name, METH_VARARGS|METH_KEYWORDS, "Set the names and state of the recording and playback files."},
	{"get_params", get_params, METH_VARARGS, "Return parameters from quisk."},
	{"set_params", (PyCFunction)set_params, METH_VARARGS|METH_KEYWORDS, "Set miscellaneous parameters in quisk.c."},
	{"set_sparams", (PyCFunction)quisk_set_sparams, METH_VARARGS|METH_KEYWORDS, "Set miscellaneous parameters in sound.c."},
	{"set_filters", set_filters, METH_VARARGS, "Set the receive audio I and Q channel filters."},
	{"set_auto_notch", set_auto_notch, METH_VARARGS, "Set the auto notch on or off."},
	{"set_kill_audio", set_kill_audio, METH_VARARGS, "Replace radio sound with silence."},
	{"set_enable_bandscope", set_enable_bandscope, METH_VARARGS, "Enable or disable sending bandscope data."},
	{"set_noise_blanker", set_noise_blanker, METH_VARARGS, "Set the noise blanker level."},
	{"set_record_state", set_record_state, METH_VARARGS, "Set the temp buffer record and playback state."},
	{"set_rx_mode", set_rx_mode, METH_VARARGS, "Set the receive mode: CWL, USB, AM, etc."},
	{"set_mic_out_volume", set_mic_out_volume, METH_VARARGS, "Set the level of the mic output for SoftRock transmit"},
	{"set_spot_level", set_spot_level, METH_VARARGS, "Set the spot level, or -1 for no spot"},
	{"set_imd_level", set_imd_level, METH_VARARGS, "Set the imd level 0 to 1000."},
	{"set_sidetone", set_sidetone, METH_VARARGS, "Set the sidetone volume and frequency."},
	{"set_sample_bytes", set_sample_bytes, METH_VARARGS, "Set the number of bytes for each I or Q sample."},
	{"XXset_transmit_mode", set_transmit_mode, METH_VARARGS, "Change the radio to transmit mode independent of key_down."},
	{"set_volume", set_volume, METH_VARARGS, "Set the audio output volume."},
	{"set_tx_audio", (PyCFunction)quisk_set_tx_audio, METH_VARARGS|METH_KEYWORDS, "Set the transmit audio parameters."},
	{"is_vox", quisk_is_vox, METH_VARARGS, "return the VOX state zero or one."},
	{"set_split_rxtx", set_split_rxtx, METH_VARARGS, "Set split for rx/tx."},
	{"set_tune", set_tune, METH_VARARGS, "Set the tuning frequency."},
	{"test_1", test_1, METH_VARARGS, "Test 1 function."},
	{"test_2", test_2, METH_VARARGS, "Test 2 function."},
	{"test_3", test_3, METH_VARARGS, "Test 3 function."},
	{"tx_hold_state", tx_hold_state, METH_VARARGS, "Query or set the transmit hold state."},
	{"set_fdx", set_fdx, METH_VARARGS, "Set full duplex mode; ignore the key status."},
	{"directx_sound_devices", quisk_directx_sound_devices, METH_VARARGS, "Return a list of available DirectX sound device names."},
	{"wasapi_sound_devices", quisk_wasapi_sound_devices, METH_VARARGS, "Return a list of available WASAPI sound device names."},
	{"portaudio_sound_devices", quisk_portaudio_sound_devices, METH_VARARGS, "Return a list of available PortAudio sound device names."},
	{"pulseaudio_sound_devices", quisk_pulseaudio_sound_devices, METH_VARARGS, "Return a list of available PulseAudio sound device names."},
	{"alsa_sound_devices", quisk_alsa_sound_devices, METH_VARARGS, "Return a list of available Alsa sound device names."},
	{"GetQuiskPrintf", GetQuiskPrintf, METH_VARARGS, "Return the output of our printf() replacement from Windows C."},
	{"AppStatus", AppStatus, METH_VARARGS, "Perform application initialization."},
	{"sound_errors", quisk_sound_errors, METH_VARARGS, "Return a list of text strings with sound devices and error counts"},
	{"set_sound_name", quisk_set_sound_name, METH_VARARGS, "Set the name of the soundcard device."},
	{"open_sound", open_sound, METH_VARARGS, "Open the soundcard device."},
	{"control_midi", (PyCFunction)quisk_control_midi, METH_VARARGS|METH_KEYWORDS, "Set the MIDI parameters."},
	{"open_wav_file_play", open_wav_file_play, METH_VARARGS, "Open a WAV file to play instead of the microphone."},
	{"close_sound", close_sound, METH_VARARGS, "Stop the soundcard and release resources."},
	{"capt_channels", quisk_capt_channels, METH_VARARGS, "Set the I and Q capture channel numbers"},
	{"play_channels", quisk_play_channels, METH_VARARGS, "Set the I and Q playback channel numbers"},
	{"micplay_channels", quisk_micplay_channels, METH_VARARGS, "Set the I and Q microphone playback channel numbers"},
	{"change_scan", change_scan, METH_VARARGS, "Change to a new FFT rate and multiplier"},
	{"change_rate", change_rate, METH_VARARGS, "Change to a new sample rate"},
	{"change_rates", change_rates, METH_VARARGS, "Change to multiple new sample rates"},
	{"read_sound", read_sound, METH_VARARGS, "Read from the soundcard."},
	{"start_sound", start_sound, METH_VARARGS, "Start the soundcard."},
	{"mixer_set", mixer_set, METH_VARARGS, "Set microphone mixer parameters such as volume."},
	{"open_key", (PyCFunction)quisk_open_key, METH_VARARGS|METH_KEYWORDS, "Open access to the state of the key (CW or PTT)."},
	{"close_key", quisk_close_key, METH_VARARGS, "Close the key."},
	{"open_rx_udp", open_rx_udp, METH_VARARGS, "Open a UDP port for capture."},
	{"close_rx_udp", close_rx_udp, METH_VARARGS, "Close the UDP port used for capture."},
	{"add_rx_samples", add_rx_samples, METH_VARARGS, "Record the Rx samples received by Python code."},
	{"add_bscope_samples", add_bscope_samples, METH_VARARGS, "Record the bandscope samples received by Python code."},
	{"set_key_down", set_key_down, METH_VARARGS, "Change the key up/down state."},
	{"set_cwkey", set_hardware_cwkey, METH_VARARGS, "Change the CW key up/down state."},
	{"set_remote_cwkey", set_remote_cwkey, METH_VARARGS, "Change the remote control CW key up/down state."},
	{"set_PTT", set_PTT, METH_VARARGS, "Record the PTT button state."},
	{"freedv_open", quisk_freedv_open, METH_VARARGS, "Open FreeDV."},
	{"freedv_close", quisk_freedv_close, METH_VARARGS, "Close FreeDV."},
	{"freedv_get_snr", quisk_freedv_get_snr, METH_VARARGS, "Return the signal to noise ratio in dB."},
	{"freedv_get_version", quisk_freedv_get_version, METH_VARARGS, "Return the codec2 API version."},
	{"freedv_get_rx_char", quisk_freedv_get_rx_char, METH_VARARGS, "Get text characters received from freedv."},
	{"freedv_set_options", (PyCFunction)quisk_freedv_set_options, METH_VARARGS|METH_KEYWORDS, "Set the freedv parameters."},
	{"freedv_set_squelch_en", quisk_freedv_set_squelch_en, METH_VARARGS, "Enable or disable FreeDV squelch."},
	{"wdsp_set_parameter", (PyCFunction)quisk_wdsp_set_parameter, METH_VARARGS|METH_KEYWORDS, "Set parameters for the WDSP SDR library."},
	{"tmp_record_save", tmp_record_save, METH_VARARGS, "Save the temporary recording in a WAV file."},
	{"watfall_RgbData", watfall_RgbData, METH_VARARGS, "Return a cookie for the Waterfall pixel data."},
	{"watfall_OnGraphData", watfall_OnGraphData, METH_VARARGS, "Record a row of Waterfall FFT dB data."},
	{"watfall_GetPixels", watfall_GetPixels, METH_VARARGS, "Write the Waterfall image to be displayed."},
	{"write_fftw_wisdom", write_fftw_wisdom, METH_VARARGS, "Write the current fftw wisdom to the wisdom file."},
	{"read_fftw_wisdom", read_fftw_wisdom, METH_VARARGS, "Return the current fftw wisdom as a byte array."},
// Remote Quisk control head and slave by Ben, AC2YD
	{"start_control_head_remote_sound", quisk_start_control_head_remote_sound, METH_VARARGS, "Start running UDP remote sound on control_head."},
	{"stop_control_head_remote_sound", quisk_stop_control_head_remote_sound, METH_VARARGS, "Stop running UDP remote sound on control_head."},
	{"start_remote_radio_remote_sound", quisk_start_remote_radio_remote_sound, METH_VARARGS, "Start running UDP remote sound on remote_radio."},
	{"stop_remote_radio_remote_sound", quisk_stop_remote_radio_remote_sound, METH_VARARGS, "Stop running UDP remote sound on remote_radio."},
	{NULL, NULL, 0, NULL}		/* Sentinel */
};

#if PY_MAJOR_VERSION < 3
// Python 2.7:
PyMODINIT_FUNC init_quisk (void)
{
	PyObject * m;
	PyObject * c_api_object;
	static void * Quisk_API[] = QUISK_API_INIT;

	m = Py_InitModule ("_quisk", QuiskMethods);
	if (m == NULL) {
		QuiskPrintf("Py_InitModule of _quisk failed!\n");
		return;
	}

	QuiskError = PyErr_NewException ("quisk.error", NULL, NULL);
	Py_INCREF (QuiskError);
	PyModule_AddObject (m, "error", QuiskError);

       /* Create Capsules for handing _quisk symbols to C extensions in other Python modules. */
       c_api_object = PyCapsule_New(Quisk_API, "_quisk.QUISK_C_API", NULL);
       if (c_api_object != NULL)
         PyModule_AddObject(m, "QUISK_C_API", c_api_object);
}

// Python 3:
#else
static struct PyModuleDef _quiskmodule = {
	PyModuleDef_HEAD_INIT,
	"_quisk",
	NULL,
	-1,
	QuiskMethods
} ;

PyMODINIT_FUNC PyInit__quisk(void)
{
	PyObject * m;
	PyObject * c_api_object;
	static void * Quisk_API[] = QUISK_API_INIT;

	m = PyModule_Create(&_quiskmodule);
	if (m == NULL)
		return NULL;
	QuiskError = PyErr_NewException("_quisk.error", NULL, NULL);
	if (QuiskError == NULL) {
		Py_DECREF(m);
		return NULL;
	}
	Py_INCREF (QuiskError);
	PyModule_AddObject (m, "error", QuiskError);

       /* Create Capsules for handing _quisk symbols to C extensions in other Python modules. */
       c_api_object = PyCapsule_New(Quisk_API, "_quisk.QUISK_C_API", NULL);
       if (c_api_object != NULL)
         PyModule_AddObject(m, "QUISK_C_API", c_api_object);
	return m;
}
#endif
