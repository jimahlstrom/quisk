from setuptools import setup, Extension
import sys
import os

# This file is used to build the Linux and Mac versions of Quisk. Windows builds are not included.

# You must define the version here.  A title string including
# the version will be written to __init__.py and read by quisk.py.

Version = '4.2.41'

fp = open("__init__.py", "w")	# write title string
fp.write("#Quisk version %s\n" % Version)
fp.write("from .quisk import main\n")
fp.close()

sources = ['quisk.c', 'sound.c', 'is_key_down.c', 'microphone.c', 'utility.c',
	'sound_alsa.c', 'sound_pulseaudio.c', 'sound_portaudio.c', 'sound_directx.c', 'sound_wasapi.c',
	'filter.c', 'extdemod.c', 'freedv.c', 'quisk_wdsp.c', 'ac2yd/remote.c']

# Afedri hardware support added by Alex, Alex@gmail.com
mAfedri = Extension ('quisk.afedrinet.afedrinet_io',
	libraries = ['m'],
	sources = ['import_quisk_api.c', 'is_key_down.c', 'afedrinet/afedrinet_io.c'],
	include_dirs = ['.'],
	)

mSoapy = Extension ('quisk.soapypkg.soapy',
	libraries = ['m', 'SoapySDR'],
	sources = ['import_quisk_api.c', 'soapypkg/soapy.c'],
	include_dirs = ['.'],
	)

# Changes for MacOS support thanks to Mario, DL3LSM.
# Changes for building from macports provided by Eric, KM4DSJ
# Updated code for a Mac build contributed by Christoph, DL1YCF, December 2020.
if sys.platform == "darwin":	# Build for Macintosh
  define_macros = [("QUISK_HAVE_PORTAUDIO", None)]	# PortAudio is always available
  libraries = ['portaudio', 'fftw3', 'm']
  if os.path.isdir('/opt/local/include'):	# MacPorts
    base_dir = '/opt/local'
  elif os.path.isdir('/usr/local/include'):	# HomeBrew on macOS Intel 
    base_dir = '/usr/local'
  elif os.path.isdir('/opt/homebrew/include'): # HomeBrew on Apple Silicon
    base_dir = '/opt/homebrew'
  else:						# Regular build?
    base_dir = '/usr'
  if os.path.isfile(base_dir + "/include/pulse/pulseaudio.h"):
    libraries.append('pulse')
    define_macros.append(("QUISK_HAVE_PULSEAUDIO", None))
  Modules = [Extension ('quisk._quisk', include_dirs=['.', base_dir + '/include'], library_dirs=['.', base_dir + '/lib'],
             libraries=libraries, sources=sources, define_macros=define_macros)]
elif "freebsd" in sys.platform:	#Build for FreeBSD
  libraries = ['pulse', 'fftw3', 'm']
  base_dir = '/usr/local'
  define_macros = [("QUISK_HAVE_PULSEAUDIO", None)] # Pulseaudio is in FreeBSD base
  Modules = [Extension ('quisk._quisk', include_dirs=['.', base_dir + '/include'], library_dirs=['.', base_dir + '/lib'],
             libraries=libraries, sources=sources, define_macros=define_macros)]
else:		# Linux
  define_macros = [("QUISK_HAVE_ALSA", None), ("QUISK_HAVE_PULSEAUDIO", None)]
  libraries = ['asound', 'pulse', 'fftw3', 'm']
  if os.path.isfile("/usr/include/portaudio.h"):
    libraries.append('portaudio')
    define_macros.append(("QUISK_HAVE_PORTAUDIO", None))
  Modules = [Extension ('quisk._quisk', libraries=libraries, sources=sources, define_macros=define_macros)]
  Modules.append(mAfedri)
  if os.path.isdir("/usr/include/SoapySDR") or os.path.isdir("/usr/local/include/SoapySDR"):
    Modules.append(mSoapy)

setup	(name = 'quisk',
	version = Version,
	description = 'QUISK is a Software Defined Radio (SDR) transceiver that can control various radio hardware.',
	long_description = """QUISK is a Software Defined Radio (SDR) transceiver.  
You supply radio hardware that converts signals at the antenna to complex (I/Q) data at an
intermediate frequency (IF). Data can come from a sound card, Ethernet or USB. Quisk then filters and
demodulates the data and sends the audio to your speakers or headphones. For transmit, Quisk takes
the microphone signal, converts it to I/Q data and sends it to the hardware.

Quisk can be used with SoftRock, Hermes Lite 2, HiQSDR, Odyssey and many radios that use the Hermes protocol.
Quisk can connect to digital programs like Fldigi and WSJT-X. Quisk can be connected to other software like
N1MM+ and software that uses Hamlib.
""",
	author = 'James C. Ahlstrom',
	author_email = 'jahlstr@gmail.com',
	url = 'http://james.ahlstrom.name/quisk/',
	packages = ['quisk', 'quisk.n2adr', 'quisk.softrock', 'quisk.freedvpkg',
		'quisk.hermes', 'quisk.hiqsdr', 'quisk.afedrinet', 'quisk.soapypkg',
		'quisk.sdrmicronpkg', 'quisk.perseuspkg', 'quisk.ac2yd', 'quisk.multuspkg'],
	package_dir =  {'quisk' : '.'},
	package_data = {'' : ['*.txt', '*.html', '*.so', '*.dll']},
	entry_points = {'gui_scripts' : ['quisk = quisk.quisk:main', 'quisk_vna = quisk.quisk_vna:main']},
	ext_modules = Modules,
	provides = ['quisk'],
	classifiers = [
		'Development Status :: 6 - Mature',
		'Environment :: X11 Applications',
		'Environment :: Win32 (MS Windows)',
		'Intended Audience :: End Users/Desktop',
		'License :: OSI Approved :: GNU General Public License (GPL)',
		'Natural Language :: English',
		'Operating System :: POSIX :: Linux',
		'Operating System :: Microsoft :: Windows',
		'Programming Language :: Python :: 3',
		'Programming Language :: C',
		'Topic :: Communications :: Ham Radio',
	],
)


