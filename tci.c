#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "ws.h"
#include <pthread.h>
#include <complex.h>
#include <time.h>
#include "quisk.h"

#define TCI_STREAM_DATA_BYTES	16384
#define TCI_RX_BUF_SIZE		1024
#define TCI_COMMAND_SIZE	32

static int verbose;	// non-zero for verbose log messages
static int tci_port;

// Implement the ExpertSDR2 Version 1.4 protocol:
// The only sample rate is 48000. The Tx audio parameters are copied from the Rx audio stream.
// The only sample type is FLOAT32 type 3.
// WSJT-X RX_AUDIO_STREAM must be two channels and Stream.length is the number of floats (not samples).
// WSJT-X TX_AUDIO_STREAM always returns two channels and Stream.length is the number of floats (not samples).
// WSJT-X TX_AUDIO_STREAM is twice as long, but the data is in the first half and garbage is in the second half.
// WSJT-X TX_CHRONO Stream.length is the number of floats, and the returned TX_AUDIO_STREAM length is equal to it.
// It is unclear whether Stream.length is the number of floats, or half that for two channels. And version 1.4 lacks Stream.channels.
// The protocol name for TCI version 1.4 is "protocol:ESDR,1.4;" and the port is 40001.
// The protocol name for TCI version 2.0 is "protocol:ExpertSDR3,2.0;" and the port is 50001.
// The TCI modulations are: am, sam, dsb, lsb, usb, cw, nfm, wfm, spec, digl, digu, drm.

// These are TCI parameters:
//static int64_t quisk_dds;
//static int quisk_if;
static long long quisk_vfo;
static long long client_vfo=-1;
static int quisk_trx;
static int client_trx=-1;
static int quisk_split_enable;
static int client_split_enable=-1;
static char   quisk_modulation[TCI_COMMAND_SIZE];
static char   client_modulation[TCI_COMMAND_SIZE]={0};

static ws_cli_conn_t tci_clients_list[MAX_CLIENTS];	// list of TCI clients
static int           tci_clients_count;
static int tci_started;		// Did we start the TCI server?

// Tx Audio
ws_cli_conn_t	tci_tx_audio_client;	// single client providing Tx audio
double		tci_tx_audio_time;	// seconds since the start of Tx audio
uint64_t	tci_tx_audio_samples;	// number of samples requested since the start of Tx audio
int		tci_tx_audio_rate;	// samples per second
int		tci_tx_audio_request;	// the Stream.length to request in TX_CHRONO

static pthread_mutex_t clients_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tx_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

enum StreamType		// ExpertSDR3
{
IQ_STREAM = 0,		// Receiver IQ signal stream
RX_AUDIO_STREAM = 1,	// Receiver audio stream
TX_AUDIO_STREAM = 2,	// Audio stream for transmitter
TX_CHRONO = 3,		// Time marker flow for audio signal transmission
LINEOUT_STREAM = 4	// Linear audio output audio stream
} ;

enum SampleType		// ExpertSDR3
{
TCI_INT16 = 0,	// 16-bit integral character type
TCI_INT24 = 1,	// 24-bit integral character type
TCI_INT32 = 2,	// 32-bit integral character type
TCI_FLOAT32 = 3,	// 32-bit type with a floating point
} ;

struct ClientData {	// data for each client
	char msg_buf[TCI_RX_BUF_SIZE];
	int msg_length;
	int send_Rx_audio_stream;
	int audio_stream_samplerate;
	int audio_stream_sample_type;
	int audio_stream_channels;
	int audio_stream_samples;
	int audio_stream_bytes_per_sample;
} ;

struct _Stream {
	uint32_t receiver;	// receiver number
	uint32_t sample_rate;	// sampling rate
	uint32_t format;	// sample type determined in SampleType
	uint32_t codec;		// compression algorithm (not implemented), always 0
	uint32_t crc;		// checksum (not implemented), always 0
	int32_t  length;	// number of samples
	uint32_t type;		// stream type determined in StreamType
	uint32_t channels;	// number of channels for ExpertSDR3
	uint32_t reserv[8];	// reserved
	uint8_t  data[TCI_STREAM_DATA_BYTES];	// samples
} ;

struct _TxChrono {
	uint32_t receiver;	// receiver number
	uint32_t sample_rate;	// sampling rate
	uint32_t format;	// sample type determined in SampleType
	uint32_t codec;		// compression algorithm (not implemented), always 0
	uint32_t crc;		// checksum (not implemented), always 0
	int32_t  length;	// number of samples
	uint32_t type;		// stream type determined in StreamType
	uint32_t channels;	// number of channels for ExpertSDR3
	uint32_t reserv[8];	// reserved
} ;

static struct _TxChrono TxChrono = {
	.receiver = 0,
	.sample_rate = 48000,
	.format = TCI_FLOAT32,
	.codec = 0,
	.crc = 0,
	.length = 0,
	.type = TX_CHRONO,
	.channels = 2,
	.reserv = {0, 0, 0, 0, 0, 0, 0, 0}
} ;

struct _StreamTypes {
	char * name;
	time_t when;
} ;

#define NUM_STREAM_TYPES	5
struct _StreamTypes StreamTypes[NUM_STREAM_TYPES] = {
	{"iq       ", 0},
	{"rx_audio ", 0},
	{"tx_audio ", 0},
	{"tx_chrono", 0},
	{"lineout  ", 0}
} ;

static void PrintStream(const char * msg, size_t size)
{
	struct _Stream * pt = (struct _Stream *)msg;

	if (time(NULL) - StreamTypes[pt->type].when > 5) {
		StreamTypes[pt->type].when = time(NULL);
		if (pt->type == TX_AUDIO_STREAM)
			QuiskPrintf("TCI     Receiving %slength %5d:", StreamTypes[pt->type].name, (int)size);
		else
			QuiskPrintf("TCI Sending %s      length %5d:", StreamTypes[pt->type].name, (int)size);
		QuiskPrintf("%d %5d %d %d %d %5d %d %d\n", (int)(pt->receiver), (int)(pt->sample_rate), (int)(pt->format), (int)(pt->codec),
			(int)(pt->crc), (int)(pt->length), (int)(pt->type), (int)(pt->channels));
	}
}

static int sendframe_txt(ws_cli_conn_t client, const char * msg)
{
	if (verbose)
		QuiskPrintf("TCI Send text              %s\n", msg);
	return ws_sendframe_txt(client, msg);
}

static int sendframe_txt_bcast(uint16_t port, const char * msg)
{
	if (verbose)
		QuiskPrintf("TCI Broadcast text         %s\n", msg);
	return ws_sendframe_txt_bcast(port, msg);
}

static size_t sendframe_bin(ws_cli_conn_t client, const char * msg, size_t size)
{
	if (verbose)
		PrintStream(msg, size);
	return ws_sendframe_bin(client, msg, size);
}

static int text_message(ws_cli_conn_t client, struct ClientData * ctx)
{  // This function alters the message ctx->msg_buf.
	int i;
	char char_buf[TCI_COMMAND_SIZE];
	char * saveptr, * command;
	char * arg1, * arg2, * arg3;

	if (verbose)
		QuiskPrintf("TCI     Received text      %s\n", ctx->msg_buf);
	for (i = 0; ctx->msg_buf[i]; i++)
		ctx->msg_buf[i] = tolower(ctx->msg_buf[i]);
	command = strtok_r(ctx->msg_buf, ":", &saveptr);
	if (command == NULL)
		return 0;
	arg1 = strtok_r(NULL, ",;", &saveptr);
	arg2 = strtok_r(NULL, ",;", &saveptr);
	arg3 = strtok_r(NULL, ",;", &saveptr);
	// The command will be broadcast to all clients unless the return value is zero.
	switch (command[0]) {
	case 'a':
		if (strcmp(command, "audio_start") == 0) {
			pthread_mutex_lock(&clients_list_mutex);
			for (i = 0; i < tci_clients_count; i++) {
				if (tci_clients_list[i] == client) {
					ctx->send_Rx_audio_stream = 1;
					break;
				}
			}
			pthread_mutex_unlock(&clients_list_mutex);
		}
		else if (strcmp(command, "audio_stop") == 0) {
			pthread_mutex_lock(&clients_list_mutex);
			for (i = 0; i < tci_clients_count; i++) {
				if (tci_clients_list[i] == client) {
					ctx->send_Rx_audio_stream = 0;
					break;
				}
			}
			pthread_mutex_unlock(&clients_list_mutex);
		}
		else if (strcmp(command, "audio_stream_sample_type") == 0) {
			if (strcmp(arg1, "float32") == 0) {
				ctx->audio_stream_sample_type = TCI_FLOAT32;
				ctx->audio_stream_bytes_per_sample = 4;
			}
			else {
				return 0;
			}
		}
		else if (strcmp(command, "audio_samplerate") == 0) {
			if (atoi(arg1) != 48000)
				return 0;
		}
		else if (strcmp(command, "audio_stream_channels") == 0) {
			if (strcmp(arg1, "1") == 0)
				ctx->audio_stream_channels = 1;
			else if (strcmp(arg1, "2") == 0)
				ctx->audio_stream_channels = 2;
			else
				return 0;
		}
		else if (strcmp(command, "audio_stream_samples") == 0) {
			return 0;	// do not send
		}
		break;
	case 'i':
		if (strcmp(command, "iq_start") == 0)
			return 0;	// do not send
		else if (strcmp(command, "iq_stop") == 0)
			return 0;	// do not send
		else if (strcmp(command, "iq_samplerate") == 0)
			return 0;	// do not send
		break;
	case 'm':
		if (strcmp(command, "modulation") == 0) {
			if (arg2) {
				strncpy(client_modulation, arg2, TCI_COMMAND_SIZE - 1);
			}
			else {
				snprintf(char_buf, TCI_COMMAND_SIZE, "modulation:0,%.9s;", quisk_modulation);
				sendframe_txt(client, char_buf);
			}
			return 0;
		}
		break;
	case 's':
		if (strcmp(command, "split_enable") == 0) {
			if (arg2) {
				if (strcmp(arg2, "true") == 0)
					client_split_enable = 1;
				else
					client_split_enable = 0;
			}
			else {
				if (quisk_split_enable)
					sendframe_txt(client, "split_enable:0,true;");
				else
					sendframe_txt(client, "split_enable:0,false;");
			}
			return 0;
		}
		break;
	case 't':
		if (strcmp(command, "trx") == 0) {
			if (arg2) {
				if (strcmp(arg2, "true") == 0) {
					if (quisk_trx == 0) {
						pthread_mutex_lock(&tx_buffer_mutex);
						client_trx = 1;
						tci_tx_audio_request = TCI_STREAM_DATA_BYTES /
							ctx->audio_stream_bytes_per_sample / ctx->audio_stream_channels;
						CircularBuffer(0, NULL, tci_tx_audio_request * 2, 0);
						tci_tx_audio_client = client;
						tci_tx_audio_time = QuiskTimeSec();
						tci_tx_audio_samples = 0;
						tci_tx_audio_rate = ctx->audio_stream_samplerate;
						pthread_mutex_unlock(&tx_buffer_mutex);
					}
				}
				else if (client == tci_tx_audio_client) {
					client_trx = 0;
					tci_tx_audio_client = 0;
				}
			}
			else {
				if (quisk_trx)
					sendframe_txt(client, "trx:0,true;");
				else
					sendframe_txt(client, "trx:0,false;");
			}
			return 0;
		}
		else if (strcmp(command, "tx_stream_audio_buffering") == 0) {
			return 0;	// do not send
		}
		break;
	case 'v':
		if (strcmp(command, "vfo") == 0) {
			if (arg3) {
				client_vfo = atoll(arg3);
			}
			else {
				snprintf(char_buf, TCI_COMMAND_SIZE, "vfo:0,0,%lld;", quisk_vfo);
				sendframe_txt(client, char_buf);
			}
			return 0;
		}
		break;
	}
	return 1;	// send to all clients
}

static void onopen(ws_cli_conn_t client)
{
	char *cli;
	char command[TCI_COMMAND_SIZE];
	struct ClientData * ctx;

	// Make parameters for each client:
	ctx = malloc(sizeof(struct ClientData));
	ctx->msg_length = 0;
	ctx->send_Rx_audio_stream = 0;
	ctx->audio_stream_samplerate = 48000;
	ctx->audio_stream_sample_type = TCI_FLOAT32;
	ctx->audio_stream_channels = 2;
	ctx->audio_stream_samples = 0;
	ctx->audio_stream_bytes_per_sample = 4;
	ws_set_connection_context(client, ctx);

	pthread_mutex_lock(&clients_list_mutex);
	tci_clients_list[tci_clients_count++] = client;
	pthread_mutex_unlock(&clients_list_mutex);

	if (verbose) {
		cli = ws_getaddress(client);
		QuiskPrintf("TCI *Connection opened, addr: %s\n", cli);
	}

	sendframe_txt(client, "protocol:ESDR,1.4;");
	//sendframe_txt(client, "vfo_limits:30000,30000000;");  Not known due to transverters
	//sendframe_txt(client, "if_limits:-48000,48000;");
	sendframe_txt(client, "trx_count:1;");
	sendframe_txt(client, "channel_count:1;");
	sendframe_txt(client, "device:QuiskSDR;");
	sendframe_txt(client, "receive_only:false;");
	sendframe_txt(client, "modulations_list:usb,lsb,cw,am,fm,digl,digu;");
	sendframe_txt(client, "ready;");

	sendframe_txt(client, "start;");
	sendframe_txt(client, "tx_enable:0,true;");
	//snprintf(command, TCI_COMMAND_SIZE, "dds:0,%d;", quisk_dds);
	//sendframe_txt(client, command);
	//snprintf(command, TCI_COMMAND_SIZE, "if:0,0,%d;", quisk_if);
	//sendframe_txt(client, command);
	snprintf(command, TCI_COMMAND_SIZE, "modulation:0,%.9s;", quisk_modulation);
	sendframe_txt(client, command);
	snprintf(command, TCI_COMMAND_SIZE, "vfo:0,0,%lld;", quisk_vfo);
	sendframe_txt(client, command);
	if (quisk_trx)
		sendframe_txt(client, "trx:0,true;");
	else
		sendframe_txt(client, "trx:0,false;");
	if (quisk_split_enable)
		sendframe_txt(client, "split_enable:0,true;");
	else
		sendframe_txt(client, "split_enable:0,false;");
}

static void onclose(ws_cli_conn_t client)
{
	int i, count;

	pthread_mutex_lock(&clients_list_mutex);
	count = tci_clients_count;
	tci_clients_count = 0;
	for (i = 0; i < count; i++)
		if (tci_clients_list[i] != client)
			tci_clients_list[tci_clients_count++] = tci_clients_list[i];
	pthread_mutex_unlock(&clients_list_mutex);

	free(ws_get_connection_context(client));
	if (verbose) {
		char * cli = ws_getaddress(client);
		QuiskPrintf("TCI *Connection closed, addr: %s\n", cli);
	}
}

static void onmessage(ws_cli_conn_t client, const unsigned char *msg, uint64_t size, int type)
{
	char echo_copy[TCI_RX_BUF_SIZE];
	complex double sample;

	if (type == WS_FR_OP_TXT) {
		if (size > TCI_RX_BUF_SIZE / 2)
			return;		// prevent overflow
		struct ClientData * ctx = ws_get_connection_context(client);
		if (ctx->msg_length + size >= TCI_RX_BUF_SIZE - 2)
			ctx->msg_length = 0;	// prevent overflow
		memcpy(ctx->msg_buf + ctx->msg_length, msg, size);	// append msg to buffer
		ctx->msg_length += size;
		ctx->msg_buf[ctx->msg_length] = '\0';
		char * semicolon;
		char save;
		while ((semicolon = strchr(ctx->msg_buf, ';')) != NULL) {
			save = *(semicolon + 1);
			*(semicolon + 1) = '\0';
			strncpy(echo_copy, ctx->msg_buf, TCI_RX_BUF_SIZE);
			if (text_message(client, ctx))		// should we send to all clients?
				sendframe_txt_bcast(tci_port, echo_copy);
			*(semicolon + 1) = save;
			size_t processed_len = (semicolon - ctx->msg_buf) + 1;
			size_t remaining_len = ctx->msg_length - processed_len;
			memmove(ctx->msg_buf, ctx->msg_buf + processed_len, remaining_len);
			ctx->msg_length = remaining_len;
			ctx->msg_buf[ctx->msg_length] = '\0';
		}
	}
	else if (type == WS_FR_OP_BIN) {
		struct _Stream * pt = (struct _Stream *)msg;
		int count = 0;
		float re, im;
		int channels = TxChrono.channels;
		int two_channels = channels == 2;
		int header = 16 * sizeof(uint32_t);
		const unsigned char * vpt = msg + header;
		const unsigned char * vpt_end = vpt + (size - header);
		const unsigned char * vpt2;
		const unsigned char * vpt2_end;
		int count2;
		int buf_start=0, buf_end;

		if (verbose >= 3) {
			QuiskPrintf("\n\nMSG size %ld length %d\n   0 ", size, pt->length);
			vpt2 = msg + header;
			vpt2_end = vpt2 + (size - header);
			count2 = 0;
			while (vpt2 < vpt2_end) {
				re = *(float *)vpt2;
				vpt2 += sizeof(float);
				im = *(float *)vpt2;
				vpt2 += sizeof(float);
				if (fabsf(re) > 1.1)
					re = 9;
				if (fabsf(im) > 1.1)
					im = 9;
				count2++;
				QuiskPrintf("%8.4f %8.4f   ", re, im);
				if (count2 % 5 == 0)
					QuiskPrintf("\n%4d ", count2);
			}
			QuiskPrintf("\n");
		}

		if (pt->type == TX_AUDIO_STREAM) {
			if (pt->length > 0 && client == tci_tx_audio_client) {
				pthread_mutex_lock(&tx_buffer_mutex);
				if (verbose >= 2)
					buf_start = CircularBuffer(0, &sample, 0, 0);
				switch (TxChrono.format) {
				case TCI_INT16:
					break;
				case TCI_FLOAT32:
					// Version 1.4 does not return the number of channels. We assume two channels.
					// pt->length is the number of floats in WSJT-X.
					while (vpt < vpt_end && count < pt->length) {
						re = *(float *)vpt;
						vpt += sizeof(float);
						count++;
						if (two_channels) {
							im = *(float *)vpt;
							vpt += sizeof(float);
							count++;
						}
						else {
							im = re;
						}
						sample = (re + I * im) * (CLIP32 / 2);
						CircularBuffer(0, &sample, 0, 1);
					}
					if (verbose && count != pt->length)
						QuiskPrintf("TCI *TX count %ld %d %d\n", size, count, pt->length);
					break;
				}
				if (verbose >= 2) {
					buf_end = CircularBuffer(0, &sample, 0, 0);
					QuiskPrintf("TCI Tx buffer add %4d start %5d end %5d\n", count, buf_start, buf_end);
				}
				pthread_mutex_unlock(&tx_buffer_mutex);
			}
		}
		if (verbose)
			PrintStream((const char *)msg, size);

	}

}

static void tci_startup(void)
{
	struct ws_server tci = {
		.host = NULL,
		.port = 0,
		.thread_loop = 1,
		.timeout_ms = 1000,
		.evs.onopen = &onopen,
		.evs.onclose = &onclose,
		.evs.onmessage = &onmessage
	};
	char host32[32];

	tci_clients_count = 0;
	tci_port = QuiskGetConfigInt("tci_port", 40001);
	strncpy(host32, QuiskGetConfigString("tci_ip", "127.0.0.1"), 31);
	tci.host = host32;
	tci.port = tci_port;
	ws_socket(&tci);
	if (verbose)
		QuiskPrintf("TCI Start server on host %s port %d\n", tci.host, tci.port);
	return;
}

void tci_send_audio(complex double * cSamples, int nSamples)	// called from the sound thread
{
	int i, n;
	struct _Stream stream;
	struct ClientData * ctx;

	if (tci_clients_count <= 0)
		return;

	if (nSamples <= 0)
		return;

	pthread_mutex_lock(&clients_list_mutex);
	for (n = 0; n < tci_clients_count; n++) {
		ctx = ws_get_connection_context(tci_clients_list[n]);
		if (ctx->send_Rx_audio_stream == 0)
			continue;
		memset(&stream, 0, 16 * sizeof(uint32_t));
		stream.sample_rate = ctx->audio_stream_samplerate;
		stream.format = ctx->audio_stream_sample_type;
		stream.type = RX_AUDIO_STREAM;
		stream.channels = ctx->audio_stream_channels;
		int two_channels = stream.channels == 2;
		int bytes_per_sample = ctx->audio_stream_bytes_per_sample;
		void * vpt = &stream.data;
		void * vpt_end = vpt + TCI_STREAM_DATA_BYTES;
		switch (ctx->audio_stream_sample_type) {
		case TCI_FLOAT32:
			stream.length = 0;	// make a frame
			for (i = 0; i < nSamples; i++) {
				*(float *)vpt = (float)(creal(cSamples[i]) * (1.0 / 2147483648.0 / 2));
				vpt += bytes_per_sample;
				stream.length++;
				if (two_channels) {
					*(float *)vpt = (float)(cimag(cSamples[i]) * (1.0 / 2147483648.0 / 2));
					vpt += bytes_per_sample;
					stream.length++;
				}
				if (vpt >= vpt_end || i == nSamples - 1) {
					sendframe_bin(tci_clients_list[n], (const char *)&stream,
						(16 * sizeof(uint32_t) + stream.length * bytes_per_sample));
					stream.length = 0;
				}
			}
			break;
		}
	}
	pthread_mutex_unlock(&clients_list_mutex);
}

int tci_get_mic(complex double * cSamples, int mic_count)	// called from the sound thread
{
	if (tci_tx_audio_client) {
		pthread_mutex_lock(&tx_buffer_mutex);	// Copy samples from the Tx buffer
		int count = CircularBuffer(0, cSamples, mic_count, 0);
		pthread_mutex_unlock(&tx_buffer_mutex);
		if (count < mic_count) {
			if (verbose >= 2)
				QuiskPrintf("TCI *TX replace mic %d %d\n", count, mic_count);
			while (count < mic_count)
				cSamples[count++] = 0;
		}
		// Ask for more samples
		double dtime = QuiskTimeSec();
		if (tci_tx_audio_samples < (dtime - tci_tx_audio_time) * tci_tx_audio_rate) {
			TxChrono.length = tci_tx_audio_request;
			sendframe_bin(tci_tx_audio_client, (const char *)&TxChrono, 16 * sizeof(uint32_t));
			//tci_tx_audio_samples += tci_tx_audio_request;
			tci_tx_audio_samples += tci_tx_audio_request / 2;	// Stream.length is floats, not samples
			if (verbose >= 3)
				QuiskPrintf("TCI Request more Tx samples\n");
		}
	}
	return mic_count;
}

PyObject * quisk_tci_set_params(PyObject * self, PyObject * args, PyObject * keywds)	// called from the GUI thread
{  /* Call with keyword arguments ONLY.
      Sent from Quisk when parameters change. Broadcast the change to clients.*/
	static char * kwlist[] = {"start", "verbose", "tci_dds", "tci_if", "tci_vfo", "tci_trx", "tci_split_enable",
		"tci_modulation", NULL} ;
	long long new_vfo=-1;
	long long new_dds=-1;
	int start=-1, new_if=-1, new_trx=-1, new_split_enable=-1;
	char * mode=NULL;
	char char_buf[TCI_COMMAND_SIZE];

	if (!PyArg_ParseTupleAndKeywords (args, keywds, "|iiLiLiis", kwlist,
			&start, &verbose, &new_dds, &new_if, &new_vfo, &new_trx, &new_split_enable,
			&mode))
		return NULL;
	if (start == 1) {
		tci_started = 1;
		tci_startup();	// start the TCP server for TCI
	}
	if (tci_started == 0) {
		Py_INCREF (Py_None);
		return Py_None;
	}
	if (new_vfo != -1) {
		quisk_vfo = new_vfo;
		snprintf(char_buf, TCI_COMMAND_SIZE, "vfo:0,0,%lld;", quisk_vfo);
		sendframe_txt_bcast(tci_port, char_buf);
	}
	if (new_split_enable != -1) {
		quisk_split_enable = new_split_enable;
		if (quisk_split_enable)
			sendframe_txt_bcast(tci_port, "split_enable:0,true;");
		else
			sendframe_txt_bcast(tci_port, "split_enable:0,false;");
	}
	if (new_trx != -1) {
		quisk_trx = new_trx;
		if (quisk_trx) {
			sendframe_txt_bcast(tci_port, "trx:0,true;");
		}
		else {
			tci_tx_audio_client = 0;
			sendframe_txt_bcast(tci_port, "trx:0,false;");
		}
	}
	if (mode) {
		if (strcmp(mode, "DGT-U") == 0)
			strcpy(quisk_modulation, "digu");
		else if (strcmp(mode, "DGT-L") == 0)
			strcpy(quisk_modulation, "digl");
		else if (strcmp(mode, "CWL") == 0 || strcmp(mode, "CWU") == 0)
			strcpy(quisk_modulation, "cw");
		else if (strcmp(mode, "LSB") == 0)
			strcpy(quisk_modulation, "lsb");
		else if (strcmp(mode, "USB") == 0)
			strcpy(quisk_modulation, "usb");
		else if (strcmp(mode, "AM") == 0)
			strcpy(quisk_modulation, "am");
		else if (strcmp(mode, "FM") == 0)
			strcpy(quisk_modulation, "fm");
		else
			strncpy(quisk_modulation, mode, TCI_COMMAND_SIZE - 1);
		snprintf(char_buf, TCI_COMMAND_SIZE, "modulation:0,%.9s", quisk_modulation);
		sendframe_txt_bcast(tci_port, char_buf);
	}
	Py_INCREF (Py_None);
	return Py_None;
}

PyObject * quisk_tci_get_params(PyObject * self, PyObject * args)	// called from the GUI thread
{  /* Return client request for a parameter change */
	const char * name;
	int ii;
	long long ll;
	char char_buf[TCI_COMMAND_SIZE];

	if (!PyArg_ParseTuple (args, "s", &name))
		return NULL;
	if (tci_started == 0) {
	}
	else if (strcmp(name, "tci_vfo") == 0 && client_vfo >= 0) {
		ll = client_vfo;
		client_vfo = -1;
		return PyLong_FromLongLong(ll);
	}
	else if (strcmp(name, "tci_split_enable") == 0 && client_split_enable >= 0) {
		ii = client_split_enable;
		client_split_enable = -1;
		return PyLong_FromLong(ii);
	}
	else if (strcmp(name, "tci_trx") == 0 && client_trx >= 0) {
		ii = client_trx;
		client_trx = -1;
		return PyLong_FromLong(ii);
	}
	else if (strcmp(name, "tci_modulation") == 0 && client_modulation[0] != 0) {
		char_buf[0] = 0;
		if (strcmp(client_modulation, "digu") == 0)
			strcpy(char_buf, "DGT-U");
		else if (strcmp(client_modulation, "digl") == 0)
			strcpy(char_buf, "DGT-L");
		else if (strcmp(client_modulation, "cw") == 0)
			strcpy(char_buf, "CWU");
		else if (strcmp(client_modulation, "lsb") == 0)
			strcpy(char_buf, "LSB");
		else if (strcmp(client_modulation, "usb") == 0)
			strcpy(char_buf, "USB");
		else if (strcmp(client_modulation, "am") == 0)
			strcpy(char_buf, "AM");
		else if (strcmp(client_modulation, "fm") == 0)
			strcpy(char_buf, "FM");
		client_modulation[0] = 0;
		if (char_buf[0])
			return PyString_FromString(char_buf);
	}
	Py_INCREF (Py_None);
	return Py_None;
}
