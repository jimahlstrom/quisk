from distutils.core import setup, Extension
import sys

module2 = Extension ('perseus',
	libraries = ['m', 'perseus-sdr'],
	sources = ['../import_quisk_api.c', 'perseus.c'],
	include_dirs = ['.', '..'],
	)

modulew2 = Extension ('perseus',
	sources = ['../import_quisk_api.c', 'perseus.c'],
	include_dirs = ['.', '..'],
	libraries = ['WS2_32', 'perseus-sdr'],
	)

if sys.platform == "win32":
  Modules = [modulew2]
else:
  Modules = [module2]

setup	(name = 'perseus',
	version = '0.1',
	description = 'perseus is an extension to Quisk to support Microtelecom Perseus SDR hardware',
	long_description = """Microtelecom Perseus SDR HF receiver.
""",
	author = 'Andrea Montefusco IW0HDV',
	author_email = 'andrew@montefusco.com',
	url = 'http://www.montefusco.com',
	download_url = 'http://james.ahlstrom.name/quisk/',
	packages = ['quisk.perseuspkg'],
	package_dir =  {'perseus' : '.'},
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


