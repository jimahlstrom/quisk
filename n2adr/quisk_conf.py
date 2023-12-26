# This is the config file from my shack, which controls various hardware.
# The files to control my 2010 transceiver and for the improved version HiQSDR
# are in the package directory HiQSDR.

import sys

sample_rate = 48000

if sys.platform == 'win32':
  favorites_file_path = "C:/pub/quisk_favorites.txt"
  settings_file_path  = "C:/pub/quisk_settings.json"
  name_of_sound_play = ""
  name_of_sound_capt = "Primary"
  microphone_name = ""
  name_of_mic_play = ""
else:
  favorites_file_path = "/home/jim/pub/quisk_favorites.txt"
  settings_file_path  = "/home/jim/pub/quisk_settings.json"
  name_of_sound_play = ""
  name_of_sound_capt = "hw:0"
  microphone_name = ""
  name_of_mic_play = ""
  #lin_microphone_name = "ALC1150 Analog"
  #lin_name_of_mic_play = "USB Sound Device"

# These are for CM106 like sound device
#n2adr_sound_usb_mic = 'alsa:USB Sound Device'
#microphone_name = n2adr_sound_usb_mic
#mixer_settings = [
#    (microphone_name, 16, 1),		# PCM capture from line
#    (microphone_name, 14, 0),		# PCM capture switch
#    (microphone_name, 11, 1),		# line capture switch
#    (microphone_name, 12, 0.70),	# line capture volume
#    (microphone_name,  3, 0),		# mic playback switch
#    (microphone_name,  9, 0),		# mic capture switch
#  ]

# These are for Asus internal sound
n2adr_sound_usb_mic = 'alsa:ALC1150 Analog'
microphone_name = n2adr_sound_usb_mic
mixer_settings = [
    (microphone_name, 19, 2),		# PCM capture from line
    (microphone_name, 24, 0),		# PCM capture switch
    (microphone_name, 22, 1),		# line capture switch
    (microphone_name, 27, 0),		# line capture volume boost
    (microphone_name, 21, 0.70),	# line capture volume
  ]
