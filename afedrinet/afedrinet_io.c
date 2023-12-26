#include <Python.h>

#ifdef MS_WINDOWS
#include <Winsock2.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#ifdef MS_WINDOWS
#define QUISK_SHUT_RD	SD_RECEIVE
#define QUISK_SHUT_BOTH	SD_BOTH
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#define SOCKET  int
#define INVALID_SOCKET	-1
#define QUISK_SHUT_RD	SHUT_RD
#define QUISK_SHUT_BOTH	SHUT_RDWR
#endif

#include <complex.h>

#define IMPORT_QUISK_API
#include "quisk.h"
//#include "sdriq.h"

static SOCKET rx_udp_socket = INVALID_SOCKET;		// Socket for receiving ADC samples from UDP
static int rx_udp_started = 0;		// Have we received any data yet?
static int rx_udp_read_blocks = 0;	// Number of blocks to read for each read call
static double rx_udp_gain_correct = 1;		// For decimation by 5, correct by 4096 / 5**5
static int use_remove_dc=0;		// Remove DC from samples
//static int quisk_using_udp = 0;
// This module provides access to the SDR-IQ by RfSpace.  It is the source
// for the Python extension module sdriq.  It can be used as a model for an
// extension module for other hardware.  Read the end of this file for more
// information.  This module was written by James Ahlstrom, N2ADR.

// This module uses the Python interface to import symbols from the parent _quisk
// extension module.  It must be linked with import_quisk_api.c.  See the documentation
// at the start of import_quisk_api.c.

// Start of SDR-IQ specific code:
//
#define DEBUG		0

// Type field for the message block header; upper 3 bits of byte
#define TYPE_HOST_SET	0
#define TYPE_HOST_GET	(1 << 5)
#define NAME_SIZE		16

//#define UDP_BROADCAST 
#ifdef UDP_BROADCAST
	#define FIRST_IQ_DATA_IDX 20 
	#define RX_UDP_SIZE		1044		// Expected size of UDP samples packet
#else
	#define RX_UDP_SIZE		1028		// Expected size of UDP samples packet
	#define FIRST_IQ_DATA_IDX 4 
#endif
#define BROADCAST_HEADER_SIZE 16
#define UDP_PROTCOL_ID1  0x04
#define UDP_PROTCOL_ID2  0x18

#ifdef DEBUG_IO
#undef DEBUG_IO
#define DEBUG_IO 1
#endif

static PyObject * open_rx_udp(const char * ip, int port)
{
//	const char * ip;
//	int port;
	char buf[128];
	struct sockaddr_in Addr;
	int recvsize;
	char optval;
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

//	if (!PyArg_ParseTuple (args, "si", &ip, &port))
//		return NULL;
//	port = 50000;
#ifdef MS_WINDOWS
	wVersionRequested = MAKEWORD(2, 2);
	if (WSAStartup(wVersionRequested, &wsaData) != 0) {
		sprintf(buf, "Failed to initialize Winsock (WSAStartup)");
		return PyString_FromString(buf);
	}
#endif
//	quisk_using_udp = 1;
	rx_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rx_udp_socket != INVALID_SOCKET) 
	{
		optval=1;
		 setsockopt( rx_udp_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval) );
		recvsize = 256000;
		setsockopt(rx_udp_socket, SOL_SOCKET, SO_RCVBUF, (char *)&recvsize, sizeof(recvsize));
		memset(&Addr, 0, sizeof(Addr)); 
		Addr.sin_family = AF_INET;
		Addr.sin_port = htons(port);
		Addr.sin_addr.s_addr = htonl(INADDR_ANY);//inet_addr("192.168.0.8");
//		Addr.sin_addr.S_un.S_addr = inet_addr(ip);
//		if (connect(rx_udp_socket, (const struct sockaddr *)&Addr, sizeof(Addr)) != 0)
		if (bind(rx_udp_socket, (const struct sockaddr *)&Addr, sizeof(Addr)) != 0)
		{
			shutdown(rx_udp_socket, QUISK_SHUT_BOTH);
			close(rx_udp_socket);
			rx_udp_socket = INVALID_SOCKET;
			sprintf(buf, "Failed to connect to UDP %s port %u", ip, port);
		}
		else {
			sprintf(buf, "Capture from UDP %s port %u", ip, port);
#if DEBUG_IO
			if (getsockopt(rx_udp_socket, SOL_SOCKET, SO_RCVBUF, (char *)&intbuf, &bufsize) == 0)
			{
				printf("UDP socket receive buffer size %d\n", intbuf);
				printf("address %s port %u\n", ip, port);
			}
			else
				printf ("Failure SO_RCVBUF\n");
#endif
		}
	}
	else {
		sprintf(buf, "Failed to open socket");
	}
	return PyString_FromString(buf);
}

static PyObject * close_rx_udp(PyObject * self, PyObject * args)
{
	short msg = 0x7373;		// shutdown

	if (!PyArg_ParseTuple (args, ""))
		return NULL;

	if (rx_udp_socket != INVALID_SOCKET) {
		shutdown(rx_udp_socket, QUISK_SHUT_RD);
		send(rx_udp_socket, (char *)&msg, 2, 0);
		send(rx_udp_socket, (char *)&msg, 2, 0);
		QuiskSleepMicrosec(3000000);
		close(rx_udp_socket);
		rx_udp_socket = INVALID_SOCKET;
	}
	rx_udp_started = 0;
	
//	if (quisk_using_udp) {
//		quisk_using_udp = 0;
		
#ifdef MS_WINDOWS
		WSACleanup();
#endif
//	}
	Py_INCREF (Py_None);
	return Py_None;
}

int afedri_read_rx_udp(complex * samp)	// Read samples from UDP
{		// Size of complex sample array is SAMP_BUFFER_SIZE
	ssize_t bytes;
	//int SR = 0;
	static int sample_rate = 0;		// Sample rate such as 48000, 96000, 192000
	unsigned char buf[1500];	// Maximum Ethernet is 1500 bytes.
	static unsigned short seq0;	// must be 8 bits
	unsigned short seq_curr = 0;
#ifdef MS_WINDOWS
	__int32 i, count, nSamples, xr, xi, index;
#else
	int32_t i, count, nSamples, xr, xi, index;
#endif
	unsigned char * ptxr, * ptxi;
	static complex dc_average = 0;		// Average DC component in samples
	static complex dc_sum = 0;
	static int dc_count = 0;
	static int dc_key_delay = 0;

	// Data from the receiver is little-endian
//	if ( !rx_udp_read_blocks)
	if(sample_rate != pt_quisk_sound_state->sample_rate)
	{
		sample_rate = pt_quisk_sound_state->sample_rate;
		// "rx_udp_read_blocks" is the number of UDP blocks to read at once
		rx_udp_read_blocks = (int)(pt_quisk_sound_state->data_poll_usec * 1e-6 * sample_rate + 0.5);
		rx_udp_read_blocks = (rx_udp_read_blocks + (RX_UDP_SIZE / 12)) / (RX_UDP_SIZE / 6);	// 6 bytes per sample
		if (rx_udp_read_blocks < 1)
			rx_udp_read_blocks = 1;
#if DEBUG_IO
		printf("read_rx_udp:  rx_udp_read_blocks %d\n", rx_udp_read_blocks);
#endif
	}
/*	if ( ! rx_udp_gain_correct) {
		int dec;
		dec = (int)(rx_udp_clock / sample_rate + 0.5);
		if ((dec / 5) * 5 == dec)		// Decimation by a factor of 5
			rx_udp_gain_correct = 1.31072;
		else						// Decimation by factors of two
			rx_udp_gain_correct = 1.0;
	}
*/	
	nSamples = 0;
	for (count = 0; count < rx_udp_read_blocks; count++) 
	{		// read several UDP blocks
#if DEBUG_IO
//		printf("Data RX Process Begin %u\n",count);
#endif
		bytes = recv(rx_udp_socket, (char *)buf, RX_UDP_SIZE,  0);	// blocking read
		if (bytes != RX_UDP_SIZE) {		// Known size of sample block
			pt_quisk_sound_state->read_error++;
#if DEBUG_IO
			printf("read_rx_udp: Bad block size %i\n", (int)bytes);
#endif
			continue;
		}
		// buf[0] is the sequence number
		// buf[1] is the status:
		//		bit 0:  key up/down state
		//		bit 1:	set for ADC overrange (clip)
		seq_curr = buf[2] | (buf[3] << 8);
		if (seq_curr != seq0) {
#if DEBUG_IO
			printf("read_rx_udp: Bad sequence want %3d got %3d at block %d of %d\n",
					(unsigned int)seq0, (unsigned int)buf[0], count, rx_udp_read_blocks);
#endif
			pt_quisk_sound_state->read_error++;
		}
		seq0 = seq_curr + 1;		// Next expected sequence number
	//	quisk_set_key_down(buf[1] & 0x01);	// bit zero is key state
	//	if (buf[1] & 0x02)					// bit one is ADC overrange
	//		quisk_sound_state.overrange++;
		index = FIRST_IQ_DATA_IDX;
		ptxr = (unsigned char *)&xr;
		ptxi = (unsigned char *)&xi;
		// convert 24-bit samples to 32-bit samples; int must be 32 bits.
			while (index < bytes) 
			{
				xr = xi = 0;
				memcpy (ptxr + 2, buf + index, 2);
				index += 2;
				memcpy (ptxi + 2, buf + index, 2);
				index += 2;
				samp[nSamples++] = (xr + xi * I) * rx_udp_gain_correct;
				xr = xi = 0;
				memcpy (ptxr + 2, buf + index, 2);
				index += 2;
				memcpy (ptxi + 2, buf + index, 2);
				index += 2;
				samp[nSamples++] = (xr + xi * I) * rx_udp_gain_correct;;
	//if (nSamples == 2) printf("%12d %12d\n", xr, xi);
			}
	}
	if (quisk_is_key_down()) {
		dc_key_delay = 0;
		dc_sum = 0;
		dc_count = 0;
	}
	else if (dc_key_delay < pt_quisk_sound_state->sample_rate) {
		dc_key_delay += nSamples;
	}
	else {
		dc_count += nSamples;
		for (i = 0; i < nSamples; i++)		// Correction for DC offset in samples
			dc_sum += samp[i];
		if (dc_count > pt_quisk_sound_state->sample_rate * 2) {
			dc_average = dc_sum / dc_count;
			dc_sum = 0;
			dc_count = 0;
			//printf("dc average %lf   %lf %d\n", creal(dc_average), cimag(dc_average), dc_count);
			//printf("dc polar %.0lf   %d\n", cabs(dc_average),
			   		//	(int)(360.0 / 2 / M_PI * atan2(cimag(dc_average), creal(dc_average))));
		}
	}
	if (use_remove_dc)
		for (i = 0; i < nSamples; i++)	// Correction for DC offset in samples
			samp[i] -= dc_average;
		
//		printf("%u\n", pt_quisk_sound_state->sample_rate);
	return nSamples;
}



// End of most AFEDRI specific code.

///////////////////////////////////////////////////////////////////////////
// The API requires at least two Python functions for Open and Close, plus
// additional Python functions as needed.  And it requires exactly three
// C funcions for Start, Stop and Read samples.  Quisk runs in two threads,
// a GUI thread and a sound thread.  You must not call the GUI or any Python
// code from the sound thread.  You must return promptly from functions called
// by the sound thread.
//
// The calling sequence is Open, Start, then repeated calls to Read, then
// Stop, then Close.

// Start of Application Programming Interface (API) code:

// Called to close the sample source; called from the GUI thread.
static PyObject * close_samples(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	close_rx_udp(self, args);
	Py_INCREF (Py_None);
	return Py_None;
}

// Called to open the sample source; called from the GUI thread.
static PyObject * open_samples(PyObject * self, PyObject * args)
{
	const char * ip;
	int port;
//	const char * name;
//	char buf[128];

	if (!PyArg_ParseTuple (args, "si", &ip, &port))
		return NULL;

//	name = QuiskGetConfigString("sdriq_name", "NoName");
//	sdriq_clock = QuiskGetConfigDouble("sdriq_clock", 66666667.0);

// Record our C-language Start/Stop/Read functions for use by sound.c.
	quisk_sample_source(NULL, NULL, &afedri_read_rx_udp);
//////////////
	return open_rx_udp(ip, port);		// AFEDRI specific
//	return PyString_FromString(buf);		// return a string message
}

// Miscellaneous functions needed by the SDR-IQ; called from the GUI thread as
// a result of button presses.


// Functions callable from Python are listed here:
static PyMethodDef QuiskMethods[] = {
	{"open_samples", open_samples, METH_VARARGS, "Open the AFEDRI SDR-Net."},
	{"close_samples", close_samples, METH_VARARGS, "Close the AFEDRI SDR-Net."},
	{NULL, NULL, 0, NULL}		/* Sentinel */
};

#if PY_MAJOR_VERSION < 3
// Python 2.7:
PyMODINIT_FUNC initafedrinet_io (void)
{
	if (Py_InitModule ("afedrinet_io", QuiskMethods) == NULL) {
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
static struct PyModuleDef afedrinet_iomodule = {
	PyModuleDef_HEAD_INIT,
	"afedrinet_io",
	NULL,
	-1,
	QuiskMethods
} ;

PyMODINIT_FUNC PyInit_afedrinet_io(void)
{
	PyObject * m;

	m = PyModule_Create(&afedrinet_iomodule);
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
