
#define DEBUG_IO	0
#define DEBUG_MIC	0

// Sound parameters
//
#define QUISK_SC_SIZE		256
#define QUISK_PATH_SIZE		256		// max file path length
#define IP_SIZE			32
#define MAX_FILTER_SIZE		10001
#define BIG_VOLUME		2.2e9
#define CLOSED_TEXT		"The sound device is closed."
#define CLIP32			2147483647
#define CLIP16			32767
#define SAMP_BUFFER_SIZE	66000		// size of arrays used to capture samples
#define IMD_TONE_1		1200		// frequency of IMD test tones
#define IMD_TONE_2		1600
#define INTERP_FILTER_TAPS	85		// interpolation filter
#define MIC_OUT_RATE		48000		// mic post-processing sample rate
#define PA_LIST_SIZE		16		// max number of pulseaudio devices
#define QUISK_MAX_SUB_RECEIVERS		9	// Maximum number of sub-receiver channels in addition to the main receiver
#define QUISK_INDEX_SUB_RX1		4	// Index of sub-receiver Rx1 in quiskPlaybackDevices
#define START_CW_DELAY_MAX	250		// Maximum delay for start_cw_delay

// Test the audio: 0 == No test; normal operation;
// 1 == Copy real data to the output; 2 == copy imaginary data to the output;
// 3 == Copy transmit audio to the output.
#define TEST_AUDIO	0

#define QUISK_CWKEY_DOWN	(quisk_hardware_cwkey || quisk_serial_key_down || quisk_midi_cwkey || quisk_remote_cwkey)

#ifdef MS_WINDOWS
#define QUISK_SHUT_RD	SD_RECEIVE
#define QUISK_SHUT_BOTH	SD_BOTH
#include <synchapi.h>
extern CRITICAL_SECTION QuiskCriticalSection;
extern PyObject * QuiskPrintf(char *, ...);
#else
#define SOCKET  int
#define INVALID_SOCKET	-1
#define QUISK_SHUT_RD	SHUT_RD
#define QUISK_SHUT_BOTH	SHUT_RDWR
#define QuiskPrintf     printf
#endif

#if PY_MAJOR_VERSION >= 3
#define PyInt_FromLong			PyLong_FromLong
#define PyInt_Check			PyLong_Check
#define PyInt_AsLong			PyLong_AsLong
#define PyInt_AsUnsignedLongMask	PyLong_AsUnsignedLongMask
#endif

#define PyString_FromString		PyUnicode_FromString

typedef enum _rx_mode {
	CWL =		0,
	CWU =		1,
	LSB =		2,
	USB =		3,
	AM =		4,
	FM =		5,
	EXT =		6,
	DGT_U =		7,
	DGT_L =		8,
	DGT_IQ =	9,
	IMD =		10,
	FDV_U =		11,
	FDV_L =		12,
	DGT_FM =	13
} rx_mode_type;

// Pulseaudio support added by Philip G. Lee.  Many thanks!
/*!
 * \brief Specifies which driver a \c sound_dev is opened with
 */

typedef enum {  // In order of preference.
	Int32,
	Int16,
	Float32,
	Int24
} sound_format_t;
// Keep this array of names up to date.
extern char * sound_format_names[4];     // = {"Int32", "Int16", "Float32", "Int24"} ;

typedef enum {
	t_Capture,
	t_Playback,
	t_MicCapture,
	t_MicPlayback,
	t_DigitalInput,
	t_DigitalOutput,
	t_RawSamplePlayback,
	t_DigitalRx1Output
} device_index_t;

typedef enum dev_driver{
   DEV_DRIVER_NONE = 0,
   DEV_DRIVER_PORTAUDIO,
   DEV_DRIVER_ALSA,
   DEV_DRIVER_PULSEAUDIO,
   DEV_DRIVER_DIRECTX,
   DEV_DRIVER_WASAPI,
   DEV_DRIVER_WASAPI2,
} dev_driver_t;

typedef enum {
	SHUTDOWN,
	STARTING,
	RECEIVE,
// All states greater than RECEIVE are assumed to be transmit states.
	HARDWARE_CWKEY,
	HARDWARE_PTT,
	SOFTWARE_CWKEY,
	SOFTWARE_PTT,
	} play_state_t;

struct sound_dev {				// data for sound capture or playback device
	char name[QUISK_SC_SIZE];		// the name of the device for display
	char stream_description[QUISK_SC_SIZE]; // Short description of device/stream
	char device_name[QUISK_SC_SIZE];	// hardware device name 
	void * handle;				// Handle of open device, or NULL
	dev_driver_t driver;		// Which audio driver the device is using
	void * buffer;				// Handle of buffer for device
	int portaudio_index;		// index of portaudio device, or -1
	int doAmplPhase;			// Amplitude and Phase corrections
	double AmPhAAAA;
	double AmPhCCCC;
	double AmPhDDDD;
	double portaudio_latency;	// Suggested latency for portaudio device
	int sample_rate;			// Sample rate such as 48000, 96000, 192000
	int sample_bytes;			// Size of one channel sample in bytes, either 2 or 3 or 4
	int num_channels;			// number of channels per frame: 1, 2, 3, ...
	int channel_I;				// Index of I and Q channels: 0, 1, ...
	int channel_Q;
	int channel_Delay;			// Delay this channel by one sample; -1 for no delay, else channel_I or _Q
	int overrange;				// Count for ADC overrange (clip) for device
   // Number of frames for a read request.
   // If 0, the read should be non-blocking and read all available
   // frames.
	int read_frames;
	int latency_frames;			// desired latency in audio play samples
	int play_buf_size;			// size of the sound card playback buffer in frames (?? or bytes)
	int play_buf_bytes;			// size of the sound card playback buffer in bytes
        int old_key;                            // previous key up/down state
	int use_float;				// DirectX: Use IEEE floating point
	int dataPos;				// DirectX: data position
	int play_delay;				// DirectX: bytes of sound available to play
	int old_play_delay;			// DirectX: previous bytes of sound available to play
	int started;				// DirectX: started flag or state
	int dev_error;				// read or write error
	int dev_underrun;			// lack of samples to play
	int dev_latency;			// latency frames
	unsigned int rate_min;		// min and max available sample rates
	unsigned int rate_max;
	unsigned int chan_min;		// min and max available number of channels
	unsigned int chan_max;
	complex double dc_remove;	// filter to remove DC from samples
	double save_sample;		// Used to delay the I or Q sample
	char msg1[QUISK_SC_SIZE];	// string for information message
	char dev_errmsg[QUISK_SC_SIZE];	// error message for device, or ""
	int stream_dir_record;		// 1 for recording, 0 for playback
	char server[IP_SIZE];		// server string for remote pulseaudio
	int stream_format;		// format of pulseaudio device
	int pulse_stream_state;		// state of the pulseaudio stream
	volatile int cork_status;	// 1 for corked, 0 for uncorked
	double average_square;		// average of squared sample magnitude
	sound_format_t sound_format;	// format of sound array for the sound device
	device_index_t dev_index;	// identify devices
	void * device_data;		// special data for each sound device
        double TimerTime0;              // Used to print debug messages
	// Variables used to correct differences in sample rates:
	int cr_correction;
	int cr_delay;
	double cr_average_fill;
	int cr_average_count;
	int cr_sample_time;
	int cr_correct_time;
} ;

extern struct sound_dev quisk_Playback;
extern struct sound_dev quisk_MicPlayback;

struct sound_conf {
	char dev_capt_name[QUISK_SC_SIZE];
	char dev_play_name[QUISK_SC_SIZE];
	int sample_rate;		// Input sample rate from the ADC
	int playback_rate;		// Output play rate to sound card
	int data_poll_usec;
	int latency_millisecs;
	unsigned int rate_min;
	unsigned int rate_max;
	unsigned int chan_min;
	unsigned int chan_max;
	int read_error;
	int write_error;
	int underrun_error;
	int overrange;		// count of ADC overrange (clip) for non-soundcard device
	int latencyCapt;
	int latencyPlay;
	int interrupts;
	char msg1[QUISK_SC_SIZE];
	char err_msg[QUISK_SC_SIZE];
	// These parameters are for the microphone:
	char mic_dev_name[QUISK_SC_SIZE];		// capture device
	char name_of_mic_play[QUISK_SC_SIZE];		// playback device
	char mic_ip[IP_SIZE];
	int mic_sample_rate;				// capture sample rate
	int mic_playback_rate;				// playback sample rate
	int tx_audio_port;
	int mic_read_error;
	int mic_channel_I;		// channel number for microphone: 0, 1, ...
	int mic_channel_Q;
	double mic_out_volume;
	char IQ_server[IP_SIZE];	//IP address of optional streaming IQ server (pulseaudio)
	int verbose_pulse;      // verbose output for pulse audio
	int verbose_sound;	// verbose output for other sound systems
	int quiskKeyupDelay;	// keup delay in milliseconds
} ;

enum quisk_rec_state {
	IDLE,
	TMP_RECORD_SPEAKERS,
	TMP_RECORD_MIC,
	TMP_PLAY_SPKR_MIC,
	FILE_PLAY_SPKR_MIC,
	FILE_PLAY_SAMPLES } ;
extern enum quisk_rec_state quisk_record_state;

struct wav_file {
	FILE * fp;
	char file_name[QUISK_PATH_SIZE];
	unsigned long samples;
};

struct QuiskWav {			// data to create a WAV or RAW audio file
    double scale;
    int sample_rate;
    short format;			// RAW is 0; PCM integer is 1; IEEE float is 3.
    short nChan;
    short bytes_per_sample;
    FILE * fp;
    unsigned int samples;
    int fpStart;
    int fpEnd;
    int fpPos;
} ;

// Remote Quisk control head and slave by Ben, AC2YD
extern int remote_control_head;
extern int remote_control_slave;
int read_remote_radio_sound_socket(complex double * cSamples);
int read_remote_mic_sound_socket(complex double * cSamples);
void send_remote_radio_sound_socket(complex double * cSamples, int nSamples);
void send_remote_mic_sound_socket(complex double * cSamples, int nSamples);
extern PyObject * quisk_start_control_head_remote_sound(PyObject * self, PyObject * args);
extern PyObject * quisk_stop_control_head_remote_sound(PyObject * self, PyObject * args);
extern PyObject * quisk_start_remote_radio_remote_sound(PyObject * self, PyObject * args);
extern PyObject * quisk_stop_remote_radio_remote_sound(PyObject * self, PyObject * args);
extern int receive_graph_data(double * fft_avg);
extern void send_graph_data(double * fft_avg, int fft_size, double zoom, double deltaf, int fft_sample_rate, double scale);

void QuiskWavClose(struct QuiskWav *);
int QuiskWavWriteOpen(struct QuiskWav *, char *, short, short, short, int, double);
void QuiskWavWriteC(struct QuiskWav *, complex double *, int);
void QuiskWavWriteD(struct QuiskWav *, double *, int);
void QuiskWavWriteF(struct QuiskWav *, float *, int);
int QuiskWavReadOpen(struct QuiskWav *, char *, short, short, short, int, double);
void QuiskWavReadC(struct QuiskWav *, complex double *, int);
void QuiskWavReadD(struct QuiskWav *, double *, int);
void QuiskMeasureRate(const char *, int, int, int);
void quisk_record_audio(struct wav_file *, complex double *, int);
void copy2pixels(double * pixels, int n_pixels, double * fft, int fft_size, double zoom, double deltaf, double rate);

extern struct sound_conf quisk_sound_state, * pt_quisk_sound_state;
extern int mic_max_display;		// display value of maximum microphone signal level
extern int quiskSpotLevel;		// 0 for no spotting; else the level 10 to 1000
extern int data_width;
extern int quisk_using_udp;	// is a UDP port used for capture (0 or 1)?
extern int quisk_rx_udp_started;		// have we received any data?
extern rx_mode_type rxMode;			// mode CWL, USB, etc.
extern int quisk_tx_tune_freq;	// Transmit tuning frequency as +/- sample_rate / 2
extern PyObject * quisk_pyConfig;		// Configuration module instance
extern double quisk_mic_preemphasis;	// Mic preemphasis 0.0 to 1.0; or -1.0
extern double quisk_mic_clip;			// Mic clipping; try 3.0 or 4.0
extern int quisk_noise_blanker;			// Noise blanker level, 0 for off
extern int quisk_sidetoneCtrl;			// sidetone control value 0 to 1000
extern double quisk_audioVolume;		// volume control for radio sound playback, 0.0 to 1.0
extern int quiskImdLevel;				// level for rxMode IMD
extern int quiskTxHoldState;			// state machine for Tx wait for repeater frequency shift
extern double quisk_ctcss_freq;			// frequency in Hertz
extern unsigned char quisk_pc_to_hermes[17 * 4];		// Data to send from the PC to the Hermes hardware
extern unsigned char quisk_hermeslite_writequeue[5];		// One-time writes to Hermes-Lite
extern unsigned int quisk_hermeslite_writepointer;		// 0==No request; 1=Send writequeue; 2==Wait for ACK; 3==0x3F error from HL2
extern unsigned int quisk_hermes_code_version;			// Hermes code version from Hermes to PC
extern unsigned int quisk_hermes_board_id;			// Hermes board ID from Hermes to PC
extern int hermes_mox_bit;					// Hermes mox bit from the PC to Hermes
extern int quisk_use_rx_udp;					// Method of access to UDP hardware
extern complex double cRxFilterOut(complex double, int, int);
extern int quisk_multirx_count;			// number of additional receivers zero or 1, 2, 3, ..
extern struct sound_dev quisk_DigitalRx1Output;		// Output sound device for sub-receiver 1
extern int quisk_is_vna;			// is this the VNA program?
extern int quisk_serial_key_errors;		// Error count for the Quisk internal serial key
extern double quisk_sidetoneVolume;		// Audio output level of the CW sidetone, 0.0 to 1.0
extern int quisk_serial_key_down;		// The state of the serial port CW key
extern int quisk_serial_ptt;			// The state of the serial port PTT
extern int quisk_hardware_cwkey;		// The state of the hardware CW key from UDP or USB
extern int quisk_midi_cwkey;			// The state of the MIDI CW key
extern int quisk_remote_cwkey;			// The state of the remote CW key
extern int quisk_sidetoneFreq;			// Frequency in hertz for the sidetone
extern int quisk_active_sidetone;		// Whether and how to generate a sidetone
extern int quisk_isFDX;				// Are we in full duplex mode?
extern int quisk_use_serial_port;		// Are we using the serial port for CW key or PTT?
extern play_state_t quisk_play_state;		// startup, receiving, sidetone
extern int freedv_current_mode;			// current FreeDV mode; 700D, @)@) etc.
extern int n_modem_sample_rate;			// Receive data, decimate to modem_sample_rate, FreeDV codec output data at speech_sample_rate
extern int n_speech_sample_rate;		// Microphone decimate to speech_sample_rate, Freedv codec output data at modem_sample_rate, interpolate to 48000
extern int n_max_modem_samples;			// maximum input to freedv_rx()
extern int quisk_start_cw_delay;		// milliseconds to delay output on serial or MIDI CW key down
extern int quisk_start_ssb_delay;		// milliseconds to discard output for all modes except CW
extern struct sound_dev * quiskPlaybackDevices[];	// array of Playback sound devices
extern int quisk_close_file_play;
extern double digital_output_level;

extern PyObject * quisk_set_spot_level(PyObject * , PyObject *);
extern PyObject * quisk_get_tx_filter(PyObject * , PyObject *);

extern PyObject * quisk_set_ampl_phase(PyObject * , PyObject *);
extern PyObject * quisk_capt_channels(PyObject * , PyObject *);
extern PyObject * quisk_play_channels(PyObject * , PyObject *);
extern PyObject * quisk_micplay_channels(PyObject * , PyObject *);
extern PyObject * quisk_alsa_sound_devices(PyObject * , PyObject *);
extern PyObject * quisk_directx_sound_devices(PyObject * , PyObject *);
extern PyObject * quisk_portaudio_sound_devices(PyObject * , PyObject *);
extern PyObject * quisk_pulseaudio_sound_devices(PyObject * , PyObject *);
extern PyObject * quisk_wasapi_sound_devices(PyObject * , PyObject *);
extern PyObject * quisk_dummy_sound_devices(PyObject * , PyObject *);
extern PyObject * quisk_sound_errors(PyObject *, PyObject *);
extern PyObject * quisk_set_file_record(PyObject *, PyObject *);
extern PyObject * quisk_set_file_name(PyObject *, PyObject *, PyObject *);
extern PyObject * quisk_set_tx_audio(PyObject *, PyObject *, PyObject *);
extern PyObject * quisk_is_vox(PyObject *, PyObject *);
extern PyObject * quisk_set_udp_tx_correct(PyObject *, PyObject *);
extern PyObject * quisk_set_hermes_filter(PyObject *, PyObject *);
extern PyObject * quisk_set_alex_hpf(PyObject *, PyObject *);
extern PyObject * quisk_set_alex_lpf(PyObject *, PyObject *);

extern PyObject * quisk_freedv_open(PyObject *, PyObject *);
extern PyObject * quisk_freedv_close(PyObject *, PyObject *);
extern PyObject * quisk_freedv_get_snr(PyObject *, PyObject *);
extern PyObject * quisk_freedv_get_version(PyObject *, PyObject *);
extern PyObject * quisk_freedv_get_rx_char(PyObject *, PyObject *);
extern PyObject * quisk_freedv_set_options(PyObject *, PyObject *, PyObject *);
extern PyObject * quisk_set_sparams(PyObject *, PyObject *, PyObject *);
extern PyObject * quisk_freedv_set_squelch_en(PyObject *, PyObject *);
extern PyObject * quisk_open_key(PyObject *, PyObject *, PyObject *);
extern PyObject * quisk_close_key(PyObject *, PyObject *);
extern PyObject * quisk_set_sound_name(PyObject *, PyObject *);
extern PyObject * quisk_alsa_control_midi(PyObject *, PyObject *, PyObject *);
extern PyObject * quisk_wasapi_control_midi(PyObject *, PyObject *, PyObject *);

// WDSP interface
#define QUISK_WDSP_RX	1
extern PyObject * quisk_wdsp_set_parameter(PyObject *, PyObject *, PyObject *);
extern int wdspFexchange0(int, double *, int);

// These function pointers are the Start/Stop/Read interface for
// the SDR-IQ and any other C-language extension modules that return
// radio data samples.
typedef void (* ty_sample_start)(void);
typedef void (* ty_sample_stop)(void);
typedef int  (* ty_sample_read)(complex double *);
typedef int  (* ty_sample_write)(complex double *, int);
extern ty_sample_write quisk_pt_sample_write;

void quisk_open_sound(void);
void quisk_close_sound(void);
int quisk_process_samples(complex double *, int);
void quisk_play_samples(complex double *, int);
void quisk_play_zeros(int);
void quisk_start_sound(void);
int quisk_get_overrange(void);
void quisk_alsa_mixer_set(char *, int, PyObject *, char *, int);
int quisk_read_sound(void);
int quisk_process_microphone(int, complex double *, int);
void quisk_open_mic(void);
void quisk_close_mic(void);
void quisk_set_key_down(int);
void quisk_set_tx_mode(void);
void ptimer(int);
int quisk_extern_demod(complex double *, int, double);
void quisk_tmp_microphone(complex double *, int);
void quisk_tmp_record(complex double * , int, double);
void quisk_file_microphone(complex double *, int);
void quisk_file_playback(complex double *, int, double);
void quisk_tmp_playback(complex double *, int, double);
void quisk_hermes_tx_send(int, int *);
void quisk_udp_mic_error(char *);
void quisk_check_freedv_mode(void);
void quisk_calc_audio_graph(double, complex double *, double *, int, int);
double QuiskDeltaSec(int);
void * quisk_make_sidetone(struct sound_dev *, int);
void * quisk_make_txIQ(struct sound_dev *, int);
int quisk_play_sidetone(struct sound_dev *);
void quisk_set_play_state(void);
void quisk_poll_hardware_key(void);

// Functions supporting digital voice codecs
typedef int  (* ty_dvoice_codec_rx)(short *, double *, int, int);
typedef int  (* ty_dvoice_codec_tx)(complex double *, double *, int, int);
extern ty_dvoice_codec_rx  pt_quisk_freedv_rx;
extern ty_dvoice_codec_tx  pt_quisk_freedv_tx;

// Driver function definitions=================================================
int  quisk_read_alsa(struct sound_dev *, complex double *);
void quisk_play_alsa(struct sound_dev *, int, complex double *, int, double);
void quisk_alsa_sidetone(struct sound_dev *);
void quisk_start_sound_alsa(struct sound_dev **, struct sound_dev **);
void quisk_close_sound_alsa(struct sound_dev **, struct sound_dev **);

int  quisk_read_portaudio(struct sound_dev *, complex double *);
void quisk_play_portaudio(struct sound_dev *, int, complex double *, int, double);
void quisk_pulseaudio_sidetone(struct sound_dev *);
void quisk_start_sound_portaudio(struct sound_dev **, struct sound_dev **);
void quisk_close_sound_portaudio(void);

void play_sound_interface(struct sound_dev * , int, complex double * , int, double);

int  quisk_read_pulseaudio(struct sound_dev *, complex double *);
void quisk_play_pulseaudio(struct sound_dev *, int, complex double *, int, double);
void quisk_start_sound_pulseaudio(struct sound_dev **, struct sound_dev **);
void quisk_close_sound_pulseaudio(void);
void quisk_cork_pulseaudio(struct sound_dev *, int);
void quisk_flush_pulseaudio(struct sound_dev *);

int  quisk_read_directx(struct sound_dev *, complex double *);
void quisk_play_directx(struct sound_dev *, int, complex double *, int, double);
void quisk_start_sound_directx(struct sound_dev **, struct sound_dev **);
void quisk_close_sound_directx(struct sound_dev **, struct sound_dev **);

int  quisk_read_wasapi(struct sound_dev *, complex double *);
void quisk_write_wasapi(struct sound_dev *, int, complex double *, double);
void quisk_play_wasapi(struct sound_dev *, int, complex double *, double);
void quisk_start_sound_wasapi(struct sound_dev **, struct sound_dev **);
void quisk_close_sound_wasapi(struct sound_dev **, struct sound_dev **);
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

/*
Functions defined below this point are available for export to other extension modules using the
standard Python CObject or Capsule interface.  See the documentation in import_quisk_api.c.  Note
that index zero is used for a structure pointer, not a function pointer.

To add a function, declare it twice, use the next array index, and add it to QUISK_API_INIT.
Be very careful; here be dragons!
*/

#ifdef IMPORT_QUISK_API
// For use by modules that import the _quisk symbols
extern void ** Quisk_API;	// array of pointers to functions and variables from module _quisk
int import_quisk_api(void);	// used to initialize Quisk_API

#define QuiskGetConfigInt	(*(	int	(*)	(const char *, int)	)Quisk_API[1])
#define QuiskGetConfigDouble	(*(	double	(*)	(const char *, double)	)Quisk_API[2])
#define QuiskGetConfigString	(*(	char *	(*)	(const char *, char *)	)Quisk_API[3])
#define QuiskTimeSec		(*(	double	(*)	(void)			)Quisk_API[4])
#define QuiskSleepMicrosec	(*(	void	(*)	(int)			)Quisk_API[5])
#define QuiskPrintTime		(*(	void	(*)	(const char *, int)	)Quisk_API[6])
#define quisk_sample_source	(*(	void	(*)	(ty_sample_start, ty_sample_stop, ty_sample_read)	)Quisk_API[7])
#define quisk_dvoice_freedv	(*(	void	(*)	(ty_dvoice_codec_rx, ty_dvoice_codec_tx)        	)Quisk_API[8])
#define quisk_is_key_down	(*(	int	(*)	(void)			                                )Quisk_API[9])
#define quisk_sample_source4	(*(	void	(*)	(ty_sample_start, ty_sample_stop, ty_sample_read, ty_sample_write)	)Quisk_API[10])
#define strMcpy                 (*(     char *  (*)     (char *, const char *, size_t)                          )Quisk_API[11])

#else
// Used to export symbols from _quisk in quisk.c

int	QuiskGetConfigInt(const char *, int);
double	QuiskGetConfigDouble(const char *, double);
char *	QuiskGetConfigString(const char *, char *);
double	QuiskTimeSec(void);
void	QuiskSleepMicrosec(int);
void	QuiskPrintTime(const char *, int);
void	quisk_sample_source(ty_sample_start, ty_sample_stop, ty_sample_read);
void	quisk_dvoice_freedv(ty_dvoice_codec_rx, ty_dvoice_codec_tx);
int	quisk_is_key_down(void);
void	quisk_sample_source4(ty_sample_start, ty_sample_stop, ty_sample_read, ty_sample_write);
char *  strMcpy(char *, const char *, size_t);

#define QUISK_API_INIT	{ \
 &quisk_sound_state, &QuiskGetConfigInt, &QuiskGetConfigDouble, &QuiskGetConfigString, &QuiskTimeSec, \
 &QuiskSleepMicrosec, &QuiskPrintTime, &quisk_sample_source, &quisk_dvoice_freedv, &quisk_is_key_down, \
 &quisk_sample_source4, &strMcpy \
 }

#endif

