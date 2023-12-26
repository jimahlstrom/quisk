from distutils.core import setup, Extension
import sys

# Afedri hardware support added by Alex, Alex@gmail.com

if sys.platform == "win32":
  Modules = [
    Extension ('afedrinet.afedrinet_io',
      libraries = ['WS2_32'],
      sources = ['../import_quisk_api.c', '../is_key_down.c', 'afedrinet_io.c'],
      include_dirs = ['.', '..'],
    )
  ]
else:
  Modules = [
    Extension ('afedrinet.afedrinet_io',
      libraries = ['m'],
      sources = ['../import_quisk_api.c', '../is_key_down.c', 'afedrinet_io.c'],
      include_dirs = ['.', '..'],
    )
  ]

setup	(name = 'afedrinet_io',
	version = '0.1',
	description = 'Afedri',
	long_description = "Afedri.",
	author = 'Alex',
	author_email = 'Alex@gmail.com',
	#url = 'http://',
	download_url = 'http://james.ahlstrom.name/quisk/',
	packages = ['afedrinet.afedrinet_io'],
	package_dir =  {'afedrinet' : '.'},
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


