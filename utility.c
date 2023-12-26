#include <Python.h>
#ifdef MS_WINDOWS
#include <windows.h>
#else
#include <stdlib.h>
#include <sys/time.h>
#endif
#include <complex.h>
#include "quisk.h"

// Access to config file attributes.
// NOTE:  These must be called only from the main (GUI) thread,
//        not from the sound thread.

int QuiskGetConfigInt(const char * name, int deflt)
{  // return deflt for failure.  Accept int or float.
  int res;
  PyObject * attr;  
  if (!quisk_pyConfig || PyErr_Occurred()) {
    return deflt;
  }
  attr = PyObject_GetAttrString(quisk_pyConfig, name);
  if (attr) {
    res = (int)PyInt_AsUnsignedLongMask(attr);  // This works for floats too!
    Py_DECREF(attr);
    return res;		// success
  }
  else {
    PyErr_Clear();
  }
  return deflt;		// failure
}

int QuiskGetConfigBoolean(const char * name, int deflt)		// UNTESTED
{  //  Return 1 for True, 0 for False.  Return deflt for failure.
  int res;
  PyObject * attr;  
  if (!quisk_pyConfig || PyErr_Occurred()) {
    return deflt;
  }
  attr = PyObject_GetAttrString(quisk_pyConfig, name);
  if (attr) {
    res = PyObject_IsTrue(attr);
    Py_DECREF(attr);
    return res;		// success
  }
  else {
    PyErr_Clear();
  }
  return deflt;		// failure
}

double QuiskGetConfigDouble(const char * name, double deflt)
{  // return deflt for failure.  Accept int or float.
  double res;
  PyObject * attr;  

  if (!quisk_pyConfig || PyErr_Occurred())
    return deflt;
  attr = PyObject_GetAttrString(quisk_pyConfig, name);
  if (attr) {
    res = PyFloat_AsDouble(attr);
    Py_DECREF(attr);
    return res;		// success
  }
  else {
    PyErr_Clear();
  }
  return deflt;		// failure
}

char * QuiskGetConfigString(const char * name, char * deflt)
{  // Return the UTF-8 configuration string. Return deflt for failure.
  char * res;
  PyObject * attr;
#if PY_MAJOR_VERSION < 3
  static char retbuf[QUISK_SC_SIZE];
#endif

  if (!quisk_pyConfig || PyErr_Occurred())
    return deflt;
  attr = PyObject_GetAttrString(quisk_pyConfig, name);
  if (attr) {
#if PY_MAJOR_VERSION >= 3
    res = (char *)PyUnicode_AsUTF8(attr);
#else
    if (PyUnicode_Check(attr)) {
      PyObject * pystr = PyUnicode_AsUTF8String(attr);
      strMcpy(retbuf, PyString_AsString(pystr), QUISK_SC_SIZE);
      retbuf[QUISK_SC_SIZE - 1] = 0;
      res = retbuf;
      Py_DECREF(pystr);
    }
    else {
      res = PyString_AsString(attr);
    }
#endif
    Py_DECREF(attr);
    if (res)
      return res;		// success
    else
      PyErr_Clear();
  }
  else {
    PyErr_Clear();
  }
  return deflt;		// failure
}

double QuiskTimeSec(void)
{  // return time in seconds as a double
#ifdef MS_WINDOWS
	FILETIME ft;
	ULARGE_INTEGER ll;

	GetSystemTimeAsFileTime(&ft);
	ll.LowPart  = ft.dwLowDateTime;
	ll.HighPart = ft.dwHighDateTime;
	return (double)ll.QuadPart * 1.e-7;
#else
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + tv.tv_usec * 1e-6;
#endif
}

double QuiskDeltaSec(int timer)
{  // return the number of seconds since the last call for the timer.
   // There are two timers. The "timer" is either 0 or 1. Call first and throw away the result.
	static double time0[2] = {0, 0};
	double now;  // in seconds
	double delta;
#ifdef MS_WINDOWS
	// Code contributed by Ben Cahill
	static double timer_rate = 0;
	LARGE_INTEGER L;
	if (timer_rate == 0) {
		if (QueryPerformanceFrequency(&L))
			timer_rate = (double)L.QuadPart;
		else
			timer_rate = 1.0;
	}
	if (QueryPerformanceCounter(&L))
		now = (double)L.QuadPart / timer_rate;
	else
		now = 0;
#else
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
#else
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
#endif
		return 0;
	now = (double)ts.tv_sec + ts.tv_nsec * 1E-9;
#endif
	if (timer < 0 || timer >= 2)
		return 0;
	if (now < time0[timer])
		now = time0[timer] = 0;
	delta = now - time0[timer];
	time0[timer] = now;
	return delta;
}

void QuiskPrintTime(const char * str, int index)
{  // print the time and a message and the delta time for index 0 to 9
	double tm;
	int i;
	static double time0 = 0;
	static double start_time[10];
#ifdef MS_WINDOWS
	static long long timer_rate = 0;
	LARGE_INTEGER L;
	if ( ! timer_rate) {
		if (QueryPerformanceFrequency(&L))
			timer_rate = L.QuadPart;
		else
			timer_rate = 1;
	}
	if (QueryPerformanceCounter(&L))
		tm = (double)L.QuadPart / timer_rate;
	else
		tm = 0;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	tm = (double)tv.tv_sec + tv.tv_usec * 1e-6;
#endif
	if (index < -9 || index > 9)	// error
		return;
	if (index < 0) {
		start_time[ - index] = tm;
		return;
	}
	if ( ! str) {		// initialize
		time0 = tm;
		for (i = 0; i < 10; i++)
			start_time[i] = tm;
		return;
	}
	// print the time since startup, and the time since the last call
	if (index > 0) {
		if (str[0])	// print message and a newline
			QuiskPrintf ("%12.6lf  %9.3lf  %9.3lf  %s\n",
				tm - time0, (tm - start_time[0])*1e3, (tm - start_time[index])*1e3, str);
		else		// no message; omit newline
			QuiskPrintf ("%12.6lf  %9.3lf  %9.3lf  ",
				tm - time0, (tm - start_time[0])*1e3, (tm - start_time[index])*1e3);
	}
	else {
		if (str[0])	// print message and a newline
			QuiskPrintf ("%12.6lf  %9.3lf  %s\n",
				tm - time0, (tm - start_time[0])*1e3, str);
		else		// no message; omit newline
			QuiskPrintf ("%12.6lf  %9.3lf  ",
				tm - time0, (tm - start_time[0])*1e3);
	}
	start_time[0] = tm;
}

void QuiskSleepMicrosec(int usec)
{
#ifdef MS_WINDOWS
	int msec = (usec + 500) / 1000;		// convert to milliseconds
	if (msec < 1)
		msec = 1;
	Sleep(msec);
#else
	struct timespec tspec;
	tspec.tv_sec = usec / 1000000;
	tspec.tv_nsec = (usec - tspec.tv_sec * 1000000) * 1000;
	nanosleep(&tspec, NULL);
#endif
}

void QuiskMeasureRate(const char * msg, int count, int index, int reset)
{  //measure the sample rate for index 0 to 9. If reset, reset the count and time at each print.
	double tm;
	static unsigned long total[10] = {0,0,0,0,0,0,0,0,0,0};
	static double time0[10] = {0,0,0,0,0,0,0,0,0,0};
	static double time_pr[10] = {0,0,0,0,0,0,0,0,0,0};

	if ( ! msg) {	// init
		time0[index] = 0;
                total[index] = 0;
		return;
	}
	if (count && time0[index] == 0) {		// init
		time0[index] = time_pr[index] = QuiskTimeSec();
		total[index] = 0;
		return;
	}
	if (time0[index] == 0)
		return;
	total[index] += count;
	tm = QuiskTimeSec();
	if (tm > time_pr[index] + 10.0) {	// time to print
		time_pr[index] = tm;
		QuiskPrintf("%s count %ld, time %.3lf, rate %.3lf\n", msg, total[index], tm - time0[index], total[index] / (tm - time0[index]));
                if (reset) {
                        total[index] = 0;
                        time0[index] = tm;
                }
	}
}

char * strMcpy(char * pDest, const char * pSrc, size_t sizeDest)
{  // replacement for strncpy()
	size_t sizeCopy;

	sizeCopy = strnlen(pSrc, sizeDest - 1);
	memcpy(pDest, pSrc, sizeCopy);
	pDest[sizeCopy] = 0;
	return pDest;
}

#ifdef MS_WINDOWS
#define QP_BUF_DELTA   256
PyObject * QuiskPrintf(char * format, ...)
{  // thread safe version of printf() needed by Windows
        int length;
        PyObject * py_string;
        static int buf_size;            // size of buffer
        static int buf_strlen;          // number of chars in buffer
        static char * buffer = NULL;
        va_list args;

        EnterCriticalSection(&QuiskCriticalSection);
        if (buffer == NULL) {   // initialize
                buf_strlen = 0;
                buf_size = QP_BUF_DELTA * 2;
                buffer = malloc(buf_size);
        }
        if (format == NULL) {           // return the string
                py_string = PyUnicode_DecodeUTF8(buffer, buf_strlen, "replace");
                buf_strlen = 0;
                LeaveCriticalSection(&QuiskCriticalSection);
                return py_string;
        }
        if (buf_size - buf_strlen < QP_BUF_DELTA * 2) {     // max addition is QP_BUF_DELTA
                buf_size += QP_BUF_DELTA * 2;
                buffer = realloc(buffer, buf_size);
        }
        va_start(args, format);
        length = vsnprintf(buffer + buf_strlen, QP_BUF_DELTA, format, args);
        if (length < 0) {
                strcpy(buffer + buf_strlen, "Encoding error\n");
                buf_strlen = strlen(buffer);
        }
        else if (length > QP_BUF_DELTA - 1) {
                buf_strlen = strlen(buffer);
        }
        else {
//printf("%s", buffer + buf_strlen);
                buf_strlen += length;
        }
        va_end(args);
        LeaveCriticalSection(&QuiskCriticalSection);
        return NULL;
}
#endif
