# This provides helper functions for our makefile.

# Changes for MacOS support thanks to Mario, DL3LSM.
# Changes for building from macports provided by Eric, KM4DSJ
# Updated code for a Mac build contributed by Christoph, DL1YCF, December 2020.
# Converted from setup.py to makefile by Jim Ahlstrom, June 2026

import sys, os, subprocess, sysconfig

if len(sys.argv) < 2:
  sys.exit(2)

job = sys.argv[1]

def flags ():
  # Fetch compiler flags and link flags from Python
  result = subprocess.run(["python3-config", "--cflags"], capture_output=True, text=True, check=True)
  cflags = result.stdout.strip()
  cflags += " -fPIC"
  result = subprocess.run(["python3-config", "--ldflags"], capture_output=True, text=True, check=True)
  ldflags = result.stdout.strip()
  ldflags += " -shared -lm -Wl,-O1 -Wl,-Bsymbolic-functions -Wl,-z,relro -g -fwrapv -O2 -lfftw3"
  if sys.platform == "darwin":	# Build for Macintosh
    cflags += " -DQUISK_HAVE_PORTAUDIO "
    ldflags += " -lportaudio "
    if os.path.isdir('/opt/local/include'):	# MacPorts
      base_dir = '/opt/local'
    elif os.path.isdir('/usr/local/include'):	# HomeBrew on macOS Intel 
      base_dir = '/usr/local'
    elif os.path.isdir('/opt/homebrew/include'): # HomeBrew on Apple Silicon
      base_dir = '/opt/homebrew'
    else:						# Regular build?
      base_dir = '/usr'
    if os.path.isfile(base_dir + "/include/pulse/pulseaudio.h"):
      cflags += " -DQUISK_HAVE_PULSEAUDIO "
      ldflags += " -lpulse "
    cflags += " -I. -I%s/include " % base_dir
    ldflags += " -L. -L%s/lib " % base_dir
  elif "freebsd" in sys.platform:	#Build for FreeBSD
    cflags += " -DQUISK_HAVE_PULSEAUDIO -I. -I/usr/local/include"
    ldflags += " -lpulse -L. -L/usr/local/lib"
  else:		# Linux
    cflags += " -DQUISK_HAVE_ALSA -DQUISK_HAVE_PULSEAUDIO"
    ldflags += " -lasound -lpulse"
    if os.path.isfile("/usr/include/portaudio.h"):
      cflags += " -DQUISK_HAVE_PORTAUDIO"
      ldflags += " -lportaudio"
  return cflags, ldflags

if job == "--soapy":
  # See if soapy is available
  if os.path.isdir("/usr/include/SoapySDR") or os.path.isdir("/usr/local/include/SoapySDR"):
    print('1')
  else:
    print('0')

elif job == "--suffix":
  # Determine the platform-specific extension suffix (e.g., .cpython-311-x86_64-linux-gnu.so)
  print(sysconfig.get_config_var('EXT_SUFFIX'))

elif job == "--cflags":
  cc, ld = flags()
  print(cc)

elif job == "--ldflags":
  cc, ld = flags()
  print(ld)

else:
  sys.exit(1)
