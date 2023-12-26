# This is the config file for the VHF/UHF receiver and transmitter.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import sys, struct, socket, traceback

settings_file_path = "../quisk_settings.json"

DEBUG = 0
if sys.platform == "win32":
  n2adr_sound_pc_capt = 'Line In (Realtek High Definition Audio)'
  n2adr_sound_pc_play = 'Speakers (Realtek High Definition Audio)'
  n2adr_sound_usb_play = 'Primary'
  n2adr_sound_usb_mic = 'Primary'
  latency_millisecs = 150
  data_poll_usec = 20000
  favorites_file_path = "C:/pub/quisk_favorites.txt"
elif 0:		# portaudio devices
  name_of_sound_play = 'portaudio:CODEC USB'
  microphone_name = "portaudio:AK5370"
  latency_millisecs = 150
  data_poll_usec = 5000
  favorites_file_path = "/home/jim/pub/quisk_favorites.txt"
else:		# alsa devices
  n2adr_sound_pc_capt = 'alsa:ALC1150 Analog'
  n2adr_sound_pc_play = 'alsa:ALC1150 Analog'
  n2adr_sound_usb_play = 'alsa:USB Sound Device'
  n2adr_sound_usb_mic = 'alsa:USB Sound Device'
  latency_millisecs = 150
  data_poll_usec = 5000
  favorites_file_path = "/home/jim/pub/quisk_favorites.txt"

name_of_sound_capt = ""
name_of_sound_play = n2adr_sound_pc_play
microphone_name = n2adr_sound_pc_capt

playback_rate = 48000
agc_off_gain = 80
do_repeater_offset = True

station_display_lines = 1
# DX cluster telent login data, thanks to DJ4CM.
dxClHost = ''
#dxClHost = 'dxc.w8wts.net'
dxClPort = 7373
user_call_sign = 'n2adr'

bandLabels = ['6', '2', '1.25', '70cm', '33cm', '23cm', 'WWV']
bandState['WWV'] = (19990000, 10000, 'AM')
BandEdge['WWV'] = (19500000, 20500000)

use_rx_udp = 17		                    		# Get ADC samples from UDP
rx_udp_ip = "192.168.1.199"	                	# Sample source IP address
rx_udp_port = 0xAA53		                	# Sample source UDP port
#rx_clk38 = 38880000 - 30                      # master clock frequency, 38880 kHz nominal
#rx_udp_clock = rx_clk38 * 32 // 2 // 9  		# ADC sample rate in Hertz
sample_rate = 96000                             # 96, 192, 384, 768, 1152 (for 69120/3/10)
display_fraction = 1.00
fft_size_multiplier = 16
tx_ip = "192.168.1.201"
tx_audio_port = 0xBC79
add_imd_button = 1
add_fdx_button = 1
