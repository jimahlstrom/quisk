from distutils.core import setup, Extension
import sys

module2 = Extension ('soapy',
	libraries = ['m', 'SoapySDR'],
	sources = ['../import_quisk_api.c', 'soapy.c'],
	include_dirs = ['.', '..'],
	)

modulew2 = Extension ('soapy',
	sources = ['../import_quisk_api.c', 'soapy.c'],
	include_dirs = ['.', '..'],
	libraries = ['WS2_32', 'SoapySDR'],
	)

if sys.platform == "win32":
  Modules = [modulew2]
else:
  Modules = [module2]

setup	(name = 'soapy',
	version = '0.1',
	description = 'soapy is an extension to Quisk to support hardware using the SoapySDR API',
	long_description = """SoapySDR is a layer of software that can connect to various SDR
hardware. It provides a standard API to a client program. By using the SoapySDR API, Quisk can
connect to all the hardware devices that SoapySDR supports.
""",
	author = 'James C. Ahlstrom',
	author_email = 'jahlstr@gmail.com',
	url = 'http://james.ahlstrom.name/quisk/soapy.html',
	download_url = 'http://james.ahlstrom.name/quisk/',
	packages = ['quisk.soapypkg'],
	package_dir =  {'soapy' : '.'},
	ext_modules = Modules,
	classifiers = [
		'Development Status :: 6 - Mature',
		'Environment :: X11 Applications',
		'Environment :: Win32 (MS Windows)',
		'Intended Audience :: End Users/Desktop',
		'License :: OSI Approved :: GNU General Public License (GPL)',
		'Natural Language :: English',
		'Operating System :: POSIX :: Linux',
		'Operating System :: Microsoft :: Windows',
		'Programming Language :: Python',
		'Programming Language :: C',
		'Topic :: Communications :: Ham Radio',
	],
)


