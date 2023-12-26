/*
  This software is Copyright (C) 2021-2022 by Ben Cahill and 2006-2022 by James C. Ahlstrom,
  and is licensed for use under the GNU General Public License (GPL).
  See http://www.opensource.org.
  Note that there is NO WARRANTY AT ALL.  USE AT YOUR OWN RISK!!
*/

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <complex.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#ifdef MS_WINDOWS
#include <winsock2.h>
#include <stdint.h>
#else

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/param.h>
#if defined(BSD)
#include <netinet/in.h>
#endif
#endif

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <fcntl.h>
#endif

#include "../quisk.h"
#include "../filter.h"

#define GRAPH_DATA_SCALE	163
#define MAX_UDP_INT16_T		600
#define UDP_SEND_INT16		200

#define REMOTE_DEBUG 0  //BMC TODO:  Make this a configuration option

static SOCKET remote_radio_sound_socket = INVALID_SOCKET;		// send radio sound to control_head, receive mic samples
static SOCKET control_head_sound_socket = INVALID_SOCKET;		// receive radio sound from remote_radio, send mic samples
static SOCKET remote_radio_graph_socket = INVALID_SOCKET;		// send graph data to control_head
static SOCKET control_head_graph_socket = INVALID_SOCKET;		// receive graph data from remote_radio
static int control_head_sound_socket_started = 0;			// sound stream started on the control head
static int remote_radio_sound_socket_started = 0;			// sound stream started on the remote radio
static int control_head_graph_socket_started = 0;			// graph data stream started on the control head
static int remote_radio_graph_socket_started = 0;			// graph data stream started on the remote radio
static int control_head_data_width;					// app.data_width of control head for graph data
static int packets_sent;
static int packets_recd;

// Receive stereo 16-bit pcm radio speaker sound on the control head via UDP
int read_remote_radio_sound_socket(complex double * cSamples)
{
	int i, bytes, nInt16, nSamples;
	int16_t buf[UDP_SEND_INT16];
	double samp_r, samp_l;
	struct timeval tm_wait;
	fd_set fds;
	static struct quisk_cHB45Filter HalfBand;
	static struct quisk_cFilter cFiltInterp3;
	static int init_filters=1;

	if (control_head_sound_socket == INVALID_SOCKET)
		return 0;
	if (init_filters) {
		init_filters = 0;
		memset(&HalfBand, 0, sizeof(struct quisk_cHB45Filter));
		quisk_filt_cInit(&cFiltInterp3, quiskAudio24p3Coefs, sizeof(quiskAudio24p3Coefs)/sizeof(double));
	}
	// Signal far end (server) that we're ready (this sends our address/port to far end)
	if (!control_head_sound_socket_started) {
		QuiskPrintf("read_remote_radio_sound_socket() sending 'rr'\n");
		bytes = send(control_head_sound_socket, "rr\n", 3, 0);
		if (bytes != 3)
			QuiskPrintf("read_remote_radio_sound_socket(), sendto(): %s\n", strerror(errno));
	}

	// read all available packets, one per loop
	nSamples = 0;
	while (1) {
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 0;
		FD_ZERO (&fds);
		FD_SET (control_head_sound_socket, &fds);
		if (select(control_head_sound_socket + 1, &fds, NULL, NULL, &tm_wait) != 1) {
			//BMC QuiskPrintf("read_remote_radio_sound_socket(): select returned %i\n", retval);
			break;
		}
		bytes = recv(control_head_sound_socket, (char *)buf, UDP_SEND_INT16 * 2, 0);
		if (bytes < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				QuiskPrintf("read_remote_radio_sound_socket(), recv(): %s\n", strerror(errno));
			break;
		}
		if (bytes > 0) {
			control_head_sound_socket_started = 1;
			nInt16 = bytes / 2;
			for (i = 0; i < nInt16; i += 2) {
				samp_r = buf[i];
				samp_l = buf[i + 1];
				cSamples[nSamples++] = (samp_r + I * samp_l) / CLIP16 * CLIP32;
			}
		}
	} // while(1)

	nSamples = quisk_cInterpolate(cSamples, nSamples, &cFiltInterp3, 3);
	nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand);

	return nSamples;
}

// Receive stereo 16-bit pcm microphone samples at the remote radio via UDP
int read_remote_mic_sound_socket(complex double * cSamples)
{
	int i, bytes, nInt16, nSamples;
	int16_t buf[UDP_SEND_INT16];
	double samp_r, samp_l;
	struct timeval tm_wait;
	fd_set fds;
	static struct quisk_cHB45Filter HalfBand;
	static struct quisk_cFilter cFiltInterp3;
	static int init_filters=1;

	if (remote_radio_sound_socket == INVALID_SOCKET)
		return 0;
	if (init_filters) {
		init_filters = 0;
		memset(&HalfBand, 0, sizeof(struct quisk_cHB45Filter));
		quisk_filt_cInit(&cFiltInterp3, quiskAudio24p3Coefs, sizeof(quiskAudio24p3Coefs)/sizeof(double));
	}

	// read all available packets, one per loop
	nSamples = 0;
	while (1) {
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 0;
		FD_ZERO (&fds);
		FD_SET (remote_radio_sound_socket, &fds);
		if (select(remote_radio_sound_socket + 1, &fds, NULL, NULL, &tm_wait) != 1) {
			//BMC QuiskPrintf("read_remote_mic_sound_socket(): select returned %i\n", retval);
			break;
		}
		bytes = recv(remote_radio_sound_socket, (char *)buf, UDP_SEND_INT16 * 2, 0);
		if (bytes < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				QuiskPrintf("read_remote_mic_sound_socket(), recv(): %s\n", strerror(errno));
			break;
		}
		if (bytes > 0) {
			nInt16 = bytes / 2;
			for (i = 0; i < nInt16; i += 2) {
				samp_r = buf[i];
				samp_l = buf[i + 1];
				cSamples[nSamples++] = (samp_r + I * samp_l) / CLIP16 * CLIP32;
			}
		}
	} // while(1)

	nSamples = quisk_cInterpolate(cSamples, nSamples, &cFiltInterp3, 3);
	nSamples = quisk_cInterp2HB45(cSamples, nSamples, &HalfBand);

	return nSamples;
}
// Send stereo 16-bit pcm sound via UDP
// This code acts as UDP server for radio sound (on remote radio) or mic sound (on control head)
#define MAX_SAMPLES_FOR_REMOTE_SOUND 15000
#define RX_BUFFER_SIZE 64

// Send microphone samples from the control head to the remote radio
void send_remote_mic_sound_socket(complex double * cSamples, int nSamples)
{
	int i;
	ssize_t sent;
	static int buffer_index=0;
	static int16_t buffer[UDP_SEND_INT16];
	static struct quisk_cHB45Filter HalfBand;
	static struct quisk_cFilter cFiltDecim3;
	static int init_filters=1, size_cBuf=0;
	static complex double * cBuf=NULL;

	if (init_filters) {
		init_filters = 0;
		memset(&HalfBand, 0, sizeof(struct quisk_cHB45Filter));
		quisk_filt_cInit(&cFiltDecim3, quiskAudio24p3Coefs, sizeof(quiskAudio24p3Coefs)/sizeof(double));
	}
	if (nSamples > size_cBuf) {
		size_cBuf = nSamples;
		cBuf = (complex double *)realloc(cBuf, size_cBuf * sizeof(complex double));
	}
	if (control_head_sound_socket == INVALID_SOCKET)
		return;

	if (!control_head_sound_socket_started)
		return;
	memcpy(cBuf, cSamples, nSamples * sizeof(complex double));	// Do not alter cSamples
	// Reduce sample rate from 48 to 8 ksps
	nSamples = quisk_cDecim2HB45(cBuf, nSamples, &HalfBand);
	nSamples = quisk_cDecimate(cBuf, nSamples, &cFiltDecim3, 3);
	// Convert format from complex double to stereo pairs of 16-bit PCM samples.
	// Buffer samples until UDP_SEND_INT16 are available, and then send the block.
	for (i = 0; i < nSamples; i++) {
		buffer[buffer_index++] = (int16_t)(creal(cBuf[i]) * (double)CLIP16 / CLIP32);
		buffer[buffer_index++] = (int16_t)(cimag(cBuf[i]) * (double)CLIP16 / CLIP32);
		if (buffer_index >= UDP_SEND_INT16) {
			sent = send(control_head_sound_socket, (const char *)buffer, buffer_index * 2, 0);
			if (sent != buffer_index * 2)
				QuiskPrintf("send_remote_mic_sound_socket(), send(): %s\n", strerror(errno));
			buffer_index = 0;
		}
	}
}

// Send radio speaker sound from the remote radio to the control head
void send_remote_radio_sound_socket(complex double * cSamples, int nSamples)
{	// Send nSamples samples.  Each sample is sent as two shorts (4 bytes) of L/R audio data.
	int i, N, sent;
	static int16_t sound_lr[UDP_SEND_INT16];
	static int udp_size = 0;	// Keep track of UDP payload size, in shorts
	char buf[RX_BUFFER_SIZE];	// For startup message
	int recv_len;
	int retval;
	SOCKET * sock = &remote_radio_sound_socket;
	static struct quisk_cHB45Filter HalfBand;
	static struct quisk_cFilter cFiltDecim3;
	static int init_filters=1, size_cBuf=0;
	static complex double * cBuf=NULL;

#if REMOTE_DEBUG > 0 //BMC debug
	// measure/monitor tools:
	static float callcount = 0;
	static float sampcount = 0;
	static uint64_t bunchcount = 0;
	static float callcount_total = 0;
	static float sampcount_total = 0;
	static double prior_ts = 0;
	static double delta_total = 0;
#if REMOTE_DEBUG > 1 //BMC debug
	static double prior_packet_ts = 0.0;
	double now;
#endif
#endif

	if (*sock == INVALID_SOCKET)
		return;

	if (init_filters) {
		init_filters = 0;
		memset(&HalfBand, 0, sizeof(struct quisk_cHB45Filter));
		quisk_filt_cInit(&cFiltDecim3, quiskAudio24p3Coefs, sizeof(quiskAudio24p3Coefs)/sizeof(double));
	}
	if (nSamples > size_cBuf) {
		size_cBuf = nSamples;
		cBuf = (complex double *)realloc(cBuf, size_cBuf * sizeof(complex double));
	}

#if REMOTE_DEBUG > 0 //BMC debug
	callcount++;
#endif
	// Wait for far end (client) to send its opening greetings, so we can grab its network address/port
	if (!remote_radio_sound_socket_started) {
		struct sockaddr_in far_addr;
#ifdef MS_WINDOWS
		int addr_len = sizeof(struct sockaddr_in);
#else
		socklen_t addr_len = sizeof(struct sockaddr_in);
#endif
		struct timeval tm_wait;
		fd_set fds;
		tm_wait.tv_sec = 0;
		tm_wait.tv_usec = 0;
		FD_ZERO (&fds);
		FD_SET (*sock, &fds);
		if ((retval = select(*sock + 1, &fds, NULL, NULL, &tm_wait)) != 1) {
			//BMC QuiskPrintf("send_remote_sound_socket(): select returned %i\n", retval);
			return;
		}
		// Receive short msg, grab far end address
		if ((recv_len = recvfrom(*sock, buf, RX_BUFFER_SIZE, 0, (struct sockaddr *) &far_addr, &addr_len)) == -1) {
			QuiskPrintf("send_remote_sound_socket(), recvfrom(): %s\n", strerror(errno));
			return;
		}
		else if(recv_len > 0) {
			if (recv_len >= RX_BUFFER_SIZE)
				buf[RX_BUFFER_SIZE - 1] = '\n';
			else
				buf[recv_len] = '\n';
			QuiskPrintf("send_remote_sound_socket(): recv_len = %i, %s", recv_len, buf);
			if (connect(*sock, (const struct sockaddr *)&far_addr, sizeof(far_addr)) != 0) {
				QuiskPrintf("send_remote_sound_socket), connect(): %s\n", strerror(errno));
				close(*sock);
				*sock = INVALID_SOCKET;
			}
			else
				remote_radio_sound_socket_started = 1;
		}
	}

	memcpy(cBuf, cSamples, nSamples * sizeof(complex double));	// Do not alter cSamples
	// Reduce sample rate from 48 to 8 ksps
	nSamples = quisk_cDecim2HB45(cBuf, nSamples, &HalfBand);
	nSamples = quisk_cDecimate(cBuf, nSamples, &cFiltDecim3, 3);
	// Convert format from complex double to stereo pairs of 16-bit PCM samples, send to client
	for (i = 0; i < nSamples; i++) {
		sound_lr[udp_size++] = (int16_t)(creal(cBuf[i]) * (double)CLIP16 / CLIP32);
		sound_lr[udp_size++] = (int16_t)(cimag(cBuf[i]) * (double)CLIP16 / CLIP32);
		if (udp_size >= UDP_SEND_INT16) {
			N = UDP_SEND_INT16;
			udp_size = 0;
			sent = send(*sock, (char *)sound_lr, N * 2, 0);
			if (sent != N * 2)
				QuiskPrintf("send_remote_sound_socket(), send(): %s\n", strerror(errno));
#if REMOTE_DEBUG > 0 //BMC debug
			else {
				sampcount += sent/4;
				packets_sent++;
#if REMOTE_DEBUG > 1 //BMC debug
				now = QuiskTimeSec();
				QuiskPrintf("%f, send_remote_sound_socket(): now - prior = %f, samples = %u, sampcount = %lu\n",
					now, now - prior_packet_ts, sent / 4, sampcount);
				prior_packet_ts = now;
#endif
			}
#endif
		}
#if REMOTE_DEBUG > 1 //BMC debug
		else {
			now = QuiskTimeSec();
			QuiskPrintf("%f, send_remote_sound_socket(): nSamples = %i\n", now, nSamples);
		}
#endif
	}
#if REMOTE_DEBUG > 0 //BMC debug
	if (callcount >= 200) {
		double new_ts = QuiskTimeSec();
		double delta = new_ts - prior_ts;
		prior_ts = new_ts;
#if REMOTE_DEBUG > 1 //BMC every 200
		QuiskPrintf("send_remote_sound_socket CURRENT calls: %lu, samples %lu, deltasec %f\n", callcount, sampcount, delta);
		QuiskPrintf("%f: send_remote_sound_socket CURRENT RATES (HZ): calls %f, samples %f\n", new_ts, callcount / delta, sampcount / delta);
#endif
		if (bunchcount > 0) {	// skip the initial bunch; prebuf may distort some numbers(?)
			callcount_total += callcount;
			sampcount_total += sampcount;
			delta_total += delta;
		}
		if (bunchcount % 10 == 0 && bunchcount > 0) {
			QuiskPrintf("%f: send_remote_sound_socket SUMMARY calls: %f, samples %f, deltasec %f\n",
				new_ts, callcount_total, sampcount_total, delta_total);
			QuiskPrintf("%f: send_remote_sound_socket SUMMARY RATES (HZ): calls %f, samples %f\n",
				new_ts, callcount_total / delta_total, sampcount_total / delta_total);
		}
		bunchcount++;
		callcount = 0;
		sampcount = 0;
	}
#endif
}

// Send graph data via UDP from the remote radio to the control head
void send_graph_data(double * fft_avg, int fft_size, double zoom, double deltaf, int fft_sample_rate, double scale)
{
	static double * pixels = NULL;
	static int n_pixels = 0;
	static uint8_t sequence = 0;
	uint8_t flags;
	int16_t buffer[MAX_UDP_INT16_T];
	int16_t block;
	int pixel_index, buffer_index;
	double d1, d2;
	ssize_t sent;
	char buf[RX_BUFFER_SIZE];	// For startup message
	int recv_len;
	struct sockaddr_in far_addr;
#ifdef MS_WINDOWS
	int addr_len = sizeof(struct sockaddr_in);
#else
	socklen_t addr_len = sizeof(struct sockaddr_in);
#endif

	if (remote_radio_graph_socket == INVALID_SOCKET)
		return;
	if ( !control_head_data_width)
		return;
	if ( !remote_radio_graph_socket_started) {
		// Receive short msg, grab far end address
		// Receive from control head is necessary to establish a path through NAT
		if ((recv_len = recvfrom(remote_radio_graph_socket, buf, RX_BUFFER_SIZE, 0, (struct sockaddr *) &far_addr, &addr_len)) < 2) {
			return;
		}
		else {
			if (connect(remote_radio_graph_socket, (const struct sockaddr *)&far_addr, sizeof(far_addr)) != 0) {
				QuiskPrintf("send_remote_graph_socket), connect(): %s\n", strerror(errno));
				close(remote_radio_graph_socket);
				remote_radio_graph_socket = INVALID_SOCKET;
				return;
			}
			else {
				remote_radio_graph_socket_started = 1;
			}
		}
	}
	if (control_head_data_width > n_pixels) {
		n_pixels = control_head_data_width;
		if (pixels)
			free(pixels);
		pixels = (double *)malloc(n_pixels * sizeof(double));
	}
	if ( ! fft_avg) {	// send dummy graph data
		send(remote_radio_graph_socket, "dum", 3, 0);
		return;
	}
	copy2pixels(pixels, control_head_data_width, fft_avg, fft_size, zoom, deltaf, fft_sample_rate);
	// Send multiple 16-bit data blocks: {flags, sequence}, block number, 16-bit graph data
	// 8-bit flags:
	//	bit 0: clip indicator
	// 8-bit sequence: 0, 1, 2, ..., 255
	block = 0;
	pixel_index = 0;
	while (pixel_index < control_head_data_width) {
		if (quisk_get_overrange())
			flags = 0x01;
		else
			flags = 0x00;
		buffer[0] = flags << 8 | sequence;
		buffer[1] = block;
		buffer_index = 2;
		while (buffer_index < MAX_UDP_INT16_T && pixel_index < control_head_data_width) {
			d1 = pixels[pixel_index++];
			if (fabs(d1) < 1e-40)	// avoid log10(0)
				d1 = 1E-40;
			d2 = 20.0 * log10(d1) - scale;
			if (d2 < -200)
				d2 = -200;
			else if (d2 > 0)
				d2 = 0;
			buffer[buffer_index++] = (int16_t)lround(d2 * GRAPH_DATA_SCALE);
		}
		sent = send(remote_radio_graph_socket, (const char *)buffer, buffer_index * 2, 0);
		if (sent != buffer_index * 2)
			QuiskPrintf("send_graph_data(), send(): %s\n", strerror(errno));
		block++;
	}
	sequence += 1;
}

// Receive graph data via UDP on the control head
int receive_graph_data(double * fft_avg)
{ 
	int i, i1, i2;
	ssize_t count;
	uint8_t seq, flags;
	int16_t buffer[MAX_UDP_INT16_T];
	int16_t block;
	static int16_t * pixels = NULL;
	static int n_pixels = 0;
	static int total = 0;
	static int16_t sequence = 0;

	if (control_head_graph_socket == INVALID_SOCKET)
		return 0;
	// Signal far end (server) that we're ready (this sends our address/port to far end)
	if ( !control_head_graph_socket_started) {
		i = send(control_head_graph_socket, "rr\n", 3, 0);
		if (i != 3)
			QuiskPrintf("receive_graph_data(), send(): %s\n", strerror(errno));
	}
	if (n_pixels < data_width) {
		n_pixels = data_width;
		if (pixels)
			free(pixels);
		pixels = (int16_t *)malloc(n_pixels * sizeof(int16_t));
	}
	count = recv(control_head_graph_socket, (char *)buffer, MAX_UDP_INT16_T * 2, 0);
	count /= 2;	// convert to int16_t
	if (count > 2) {
		control_head_graph_socket_started = 1;
		flags = buffer[0] >> 8;
		if (flags & 0x01)	// Clip
			quisk_sound_state.overrange++;
		seq = buffer[0] & 0xFF;
		if (seq != sequence) {	// new graph data
			sequence = seq;
			total = 0;
		}
		block = buffer[1];
		count -= 2;	// number of 16-bit graph data items
		i1 = block * (MAX_UDP_INT16_T - 2);
		i2 = i1 + count;
		if (i1 >= 0 && i2 <= data_width) {
			memcpy(pixels + i1, buffer + 2, count * 2);
			total += count;
			if (total == data_width) {
				for (i = 0; i < data_width; i++)
					fft_avg[i] = (double)pixels[i] / GRAPH_DATA_SCALE;
				return data_width;
			}
		}
	}
	return 0;
}


static int start_winsock()
{
#ifdef MS_WINDOWS
	WORD wVersionRequested = MAKEWORD(2, 2);
	WSADATA wsaData;

	if (WSAStartup(wVersionRequested, &wsaData) != 0) {
		QuiskPrintf("start_winsock(): %s\n", strerror(errno));
		return 0;		// failure to start winsock
	}
#endif
	return 1;
}





static void open_and_bind_socket(SOCKET * sock, char * ip, int port, int sndsize, char * name, int non_block)
{
	struct sockaddr_in bind_addr;
	const char enable = 1;	// for sockopt
#ifndef MS_WINDOWS
	int tos = 184;	// DSCP "Expedite" (46)
#endif

	if (!start_winsock()) {
		QuiskPrintf("open_and_bind_socket for %s: Failure to start WinSock\n", name);
		return;
	}

	*sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (*sock != INVALID_SOCKET) {
		setsockopt(*sock, SOL_SOCKET, SO_SNDBUF, (char *)&sndsize, sizeof(sndsize));
		setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#ifndef MS_WINDOWS
		setsockopt(*sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif

		// bind to this computer for receiving (and reading far-end address from client)
		memset((char *) &bind_addr, 0, sizeof(bind_addr));
		bind_addr.sin_family = AF_INET;
		bind_addr.sin_port = htons(port);
		bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(*sock, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
			QuiskPrintf("open_and_bind_socket(), bind(): %s\n", strerror(errno));
			close(*sock);
			*sock = INVALID_SOCKET;
		}
		else if (non_block) {
#ifdef MS_WINDOWS
       			unsigned long one = 1;
			ioctlsocket(*sock, FIONBIO, &one);	// set non-blocking
#else
			int flags;
			flags = fcntl(*sock, F_GETFL, 0);	// set non-blocking
			fcntl(*sock, F_SETFL, flags | O_NONBLOCK);
#endif
		}
	}
	if (*sock == INVALID_SOCKET) {
		QuiskPrintf("open server %s: Failure to open socket\n", name);
	}
	else {
		QuiskPrintf("open server %s: opened socket %s port %i\n", name, ip, port);
	}
}


static void open_and_connect_socket(SOCKET * sock, char * ip, int port, int sndsize, char * name, int non_block)
{
	struct sockaddr_in Addr;
	const char enable = 1;	// for sockopt

	if (!start_winsock()) {
		QuiskPrintf("open_and_connect_socket for %s: Failure to start WinSock\n", name);
		return;
	}

	*sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (*sock != INVALID_SOCKET) {
		setsockopt(*sock, SOL_SOCKET, SO_RCVBUF, (char *)&sndsize, sizeof(sndsize));
		setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

		// set far-end address structure to enable sending initial packet to get things started
		Addr.sin_family = AF_INET;
		Addr.sin_port = htons(port);
#ifdef MS_WINDOWS
		Addr.sin_addr.S_un.S_addr = inet_addr(ip);
#else
		inet_aton(ip, &Addr.sin_addr);
#endif
		if (connect(*sock, (const struct sockaddr *)&Addr, sizeof(Addr)) != 0) {
			close(*sock);
			*sock = INVALID_SOCKET;
		}
		else if (non_block) {
#ifdef MS_WINDOWS
       			unsigned long one = 1;
			ioctlsocket(*sock, FIONBIO, &one);	// set non-blocking
#else
			int flags;
			flags = fcntl(*sock, F_GETFL, 0);	// set non-blocking
			fcntl(*sock, F_SETFL, flags | O_NONBLOCK);
#endif
		}
	}
	if (*sock == INVALID_SOCKET) {
		QuiskPrintf("open client %s: Failure to open socket\n", name);
	}
	else {
		QuiskPrintf("open client %s: opened socket %s port %i\n", name, ip, port);
	}
}


static void close_socket(SOCKET * sock, char * name)
{
	if (*sock != INVALID_SOCKET) {
		close(*sock);
		*sock = INVALID_SOCKET;
#ifdef MS_WINDOWS
		WSACleanup();
#endif
		QuiskPrintf("%s: closed socket\n", name);
	}
	else {
		QuiskPrintf("%s: socket already closed\n", name);
	}
}

// start running UDP remote sound on control_head ...
// ... receive radio sound from remote_radio, send mic sound to remote_radio
PyObject * quisk_start_control_head_remote_sound(PyObject * self, PyObject * args)
{
	int radio_sound_port;
	int graph_data_port;
	int sndsize = 48000;
	char * remote_radio_ip;	// IP address of far end
	char * name;
	SOCKET * sock;

	if (!PyArg_ParseTuple (args, "sii", &remote_radio_ip, &radio_sound_port, &graph_data_port))
		return NULL;

	name = "radio sound from remote_radio";
	sock = &control_head_sound_socket;
	open_and_connect_socket(sock, remote_radio_ip, radio_sound_port, sndsize, name, 0);

	name = "graph data from remote_radio";
	sock = &control_head_graph_socket;
	open_and_connect_socket(sock, remote_radio_ip, graph_data_port, 1024 * 8, name, 1);

	packets_sent = 0;
	packets_recd = 0;

	return Py_None;
}

// stop running UDP remote sound on control_head
PyObject * quisk_stop_control_head_remote_sound(PyObject * self, PyObject * args)
{
	char * name;
	SOCKET * sock;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;

	name = "radio sound from remote_radio";
	sock = &control_head_sound_socket;
	close_socket(sock, name);

	name = "graph data from remote_radio";
	sock = &control_head_graph_socket;
	close_socket(sock, name);

	control_head_sound_socket_started = 0;	// reset for next time
	remote_radio_sound_socket_started = 0;
	control_head_graph_socket_started = 0;
	remote_radio_graph_socket_started = 0;

	QuiskPrintf("total packets sent = %i, recd = %i\n", packets_sent, packets_recd);

	return Py_None;
}

// start running UDP remote sound on remote_radio ...
// ... send radio sound to control_head, receive mic sound from control_head
PyObject * quisk_start_remote_radio_remote_sound(PyObject * self, PyObject * args)
{
	int radio_sound_port;
	int graph_data_port;
	int sndsize = 48000;
	char * control_head_ip;	// IP address of far end
	char * name;
	SOCKET * sock;

	if (!PyArg_ParseTuple (args, "siii", &control_head_ip, &radio_sound_port,
                     &graph_data_port, &control_head_data_width))
		return NULL;

	name = "radio sound to control_head";
	sock = &remote_radio_sound_socket;
	open_and_bind_socket(sock, control_head_ip, radio_sound_port, sndsize, name, 0);

	name = "graph data to control_head";
	sock = &remote_radio_graph_socket;
	open_and_bind_socket(sock, control_head_ip, graph_data_port, 1024 * 8, name, 1);

	packets_sent = 0;
	packets_recd = 0;

	return Py_None;
}

// stop running UDP remote sound on remote_radio
PyObject * quisk_stop_remote_radio_remote_sound(PyObject * self, PyObject * args)
{
	char * name;
	SOCKET * sock;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;

	name = "radio sound to control_head";
	sock = &remote_radio_sound_socket;
	close_socket(sock, name);

	name = "graph data to control_head";
	sock = &remote_radio_graph_socket;
	close_socket(sock, name);

	control_head_sound_socket_started = 0;	// reset for next time
	remote_radio_sound_socket_started = 0;
	control_head_graph_socket_started = 0;
	remote_radio_graph_socket_started = 0;
	control_head_data_width = 0;

	QuiskPrintf("total packets sent = %i, recd = %i\n", packets_sent, packets_recd);

	return Py_None;
}
