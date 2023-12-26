#include <Python.h>	// used by quisk.h
#include <complex.h>	// Used by quisk.h
#include "quisk.h"

// This module provides methods to access the state of the key.
// First call quisk_open_key(name) to choose a method and initialize.
// Subsequent key access uses the method chosen.

static int startup_error = -1;		// -1: port not opened; 0: no error opening port; 1: error opening port
static int bit_cts, bit_dsr;		// modem bits
static char use_cts, use_dsr;		// use of CTS and DSR bits
static int reverse_cts, reverse_dsr;	// opposite polarity for cts/dsr
static char port_name[QUISK_SC_SIZE];	// serial port name
static PyObject * start_up(void);	// open the serial port
static void shut_down(void);		// close the serial port
static void modem_status(void);		// test modem bits
#define MSG_SIZE	(50 + QUISK_SC_SIZE)

//int quisk_serial_key_errors = 0;
int quisk_serial_key_down;		// The cts or dsr bit for CW key is asserted
int quisk_use_serial_port;		// either cts or dsr is being used
int quisk_serial_ptt;			// The cts or dsr bit for PTT is asserted

PyObject * quisk_open_key(PyObject * self, PyObject * args, PyObject * keywds)
{  // return a message for error, or "" for no error
	static char * kwlist[] = {"port", "cts", "dsr", NULL} ;
	PyObject * msg = NULL;
	char * port = NULL;
	char * cts = NULL;
	char * dsr = NULL;

	quisk_serial_key_down = 0;
	quisk_serial_ptt = 0;
	if (!PyArg_ParseTupleAndKeywords (args, keywds, "|sss", kwlist, &port, &cts, &dsr))
		return NULL;
	//quisk_serial_cts and dsr are "None", "CW", "PTT"; and "when high" or "when low"
	if (cts) {
		use_cts = * cts;		// 'N', 'C', 'P'
		reverse_cts = strstr(cts, "when low") != NULL;
	}
	if (dsr) {
		use_dsr = * dsr;		// 'N', 'C', 'P'
		reverse_dsr = strstr(dsr, "when low") != NULL;
	}
	if (port) {
		if (startup_error == 0)		// port is open
			shut_down();
		strncpy(port_name, port, QUISK_SC_SIZE - 1);
		port_name[QUISK_SC_SIZE - 1] = 0;
		if (port_name[0])
			msg = start_up();
	}
	if (startup_error == 0 && (use_cts != 'N' || use_dsr != 'N'))
		quisk_use_serial_port = 1;
	else
		quisk_use_serial_port = 0;
	if (msg == NULL)
		msg = PyUnicode_FromString("");
	return msg;
}

PyObject * quisk_close_key(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	shut_down();
	Py_INCREF (Py_None);
	return Py_None;
}

void quisk_poll_hardware_key(void)
{  // call frequently to check the modem bits
	if ( ! quisk_use_serial_port)
		return;
	modem_status();
	if (use_cts == 'C')
		quisk_serial_key_down = reverse_cts ? bit_cts == 0 : bit_cts != 0;
	else if (use_cts == 'P')
		quisk_serial_ptt = reverse_cts ? bit_cts == 0 : bit_cts != 0;
	if (use_dsr == 'C')
		quisk_serial_key_down = reverse_dsr ? bit_dsr == 0 : bit_dsr != 0;
	else if (use_dsr == 'P')
		quisk_serial_ptt = reverse_dsr ? bit_dsr == 0 : bit_dsr != 0;
}

#if defined(MS_WINDOWS)
#include <stdlib.h>
#include <sys/types.h>
#include <windows.h>
//#include <processthreadsapi.h>
//#include <timeapi.h>
#include <avrt.h>

static HANDLE hComm = INVALID_HANDLE_VALUE;		// Windows handle to read the serial port

static void modem_status(void)
{
	DWORD dwModemStatus;

	if (hComm == INVALID_HANDLE_VALUE) {
		bit_cts = bit_dsr = 0;
	}
	else {
		if (!GetCommModemStatus(hComm, &dwModemStatus)) {
			bit_cts = bit_dsr = 0;	// Error in GetCommModemStatus;
		}
		else {
			bit_cts = MS_CTS_ON & dwModemStatus;
			bit_dsr = MS_DSR_ON & dwModemStatus;
		}
	}
}

static PyObject * start_up(void)
{
	char msg[MSG_SIZE];

	hComm = CreateFile(port_name,  GENERIC_READ, 0, 0, OPEN_EXISTING,
		0, 0);
		//FILE_FLAG_OVERLAPPED, 0);
	if (hComm == INVALID_HANDLE_VALUE) {
		snprintf(msg, MSG_SIZE, "Open Morse key serial port %s failed.", port_name);
		startup_error = 1;
		return PyUnicode_FromString(msg);
	}
	startup_error = 0;
	return PyUnicode_FromString("");
}

static void shut_down(void)
{
	if (hComm != INVALID_HANDLE_VALUE)
        	CloseHandle(hComm);
	hComm = INVALID_HANDLE_VALUE;
	startup_error = -1;
	quisk_serial_key_down = 0;
	quisk_use_serial_port = 0;
	quisk_serial_ptt = 0;
}

// Changes for MacOS support thanks to Mario, DL3LSM.
// Broken by N2ADR April, 2020.
#elif defined(__MACH__)

static PyObject * start_up(void)
{
	startup_error = 0;
	return PyUnicode_FromString("");
}

static void shut_down(void)
{
	startup_error = -1;
	quisk_serial_key_down = 0;
	quisk_use_serial_port = 0;
	quisk_serial_ptt = 0;
}

static void modem_status(void)
{
}

#else
// Not MS Windows and not __MACH__:

// Access the serial port.  This code sets DTR high, and monitors DSR and CTS.
// When DSR is high set the RTS signal high. When DSR goes low set RTS low after a delay.

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>

static int fdComm = -1;			// File descriptor to read the serial port

static void modem_status(void)
{
	int bits;
	struct timeval tv;
	double time;
	static double time0=0;	// time when the key was last down

        if (fdComm >= 0) {
	        ioctl(fdComm, TIOCMGET, &bits);	// read modem bits
	        bit_cts = bits & TIOCM_CTS;
	        bit_dsr = bits & TIOCM_DSR;
		if (bit_dsr) {
			if ( ! (bits & TIOCM_RTS)) {	// set RTS
				bits |= TIOCM_RTS;
				ioctl(fdComm, TIOCMSET, &bits);
			}
			gettimeofday(&tv, NULL);
			time0 = tv.tv_sec + tv.tv_usec / 1.0E6;		// time is in seconds
		}
		else if (bits & TIOCM_RTS) {	// clear RTS after a delay
			gettimeofday(&tv, NULL);
			time = tv.tv_sec + tv.tv_usec / 1.0E6;
			if (time - time0 > pt_quisk_sound_state->quiskKeyupDelay * 1E-3) {
				bits &= ~TIOCM_RTS;
				ioctl(fdComm, TIOCMSET, &bits);
			}
		}
        }
}

static PyObject * start_up(void)
{
	int bits;
	char msg[MSG_SIZE];
	struct timespec tspec;

	fdComm = open(port_name, O_RDWR | O_NOCTTY);
	if (fdComm < 0) {
		snprintf(msg, MSG_SIZE, "Open morse key serial port %s failed.", port_name);
		startup_error = 1;
		return PyUnicode_FromString(msg);
	}
	else {
		ioctl(fdComm, TIOCMGET, &bits);		// read modem bits
		bits |= TIOCM_DTR;			// Set DTR
		bits &= ~TIOCM_RTS;			// Clear RTS at first
		ioctl(fdComm, TIOCMSET, &bits);
	}
	tspec.tv_sec = 0;
	tspec.tv_nsec = 10000 * 1000;
	nanosleep(&tspec, NULL);
	startup_error = 0;
	return PyUnicode_FromString("");
}

static void shut_down(void)
{
	if (fdComm >= 0)
		close(fdComm);
	fdComm = -1;
	startup_error = -1;
	quisk_serial_key_down = 0;
	quisk_use_serial_port = 0;
	quisk_serial_ptt = 0;
}
#endif
