# This provides support for the SDR Micron TRX by David Fainitski, N7DDC

import sys, wx, traceback, time
from quisk_hardware_model import Hardware as BaseHardware

DEBUG = 0	# print debug messages during operation
WRITE_LOG = 1	# write the data received from the MicronTRX during self.log_time to the file self.log_name

# https://github.com/Dfinitski/SDR-Micron

# Control from PC to Micron TRX, 64 bytes, MSB first (big-endian)
#
# (0 - 7) Preamble, seven 0x55 and 0xD5
# (8, 9, 10) "UCP"
# (11) SN - Sequence Number 0 - 255
# (12) RX0 enable, mode, 0 - off, 1 - 48kHz, 2 - 96kHz, 3 - 192kHz
# (13) RX1 enable, mode, 0 - off, 1 - 48kHz, 2 - 96kHz, 3- 192kHz
# (14) BS0 enable
# (15) BS1 enable
# (16) TX0 enable
# (17) CDC enable
# (18, 19, 20, 21) - 4 bytes RX0 frequency
# (22, 23, 24, 25) - 4 bytes RX1 frequency
# (26, 27, 28, 29) - 4 bytes BS0 frequency
# (30, 31, 32, 33) - 4 bytes TX0 frequency
# (34) - Attenuator, 0 - 31 (dB value)
# (35) - BS0 Rate
#          0 - 48 kHz, 
#          1 - 96 kHz,
#          2 - 192 kHz,
#          3 - 384 kHz,
#          4 - 768 kHz,
#          5 - 1536 kHz
#          6 - 1920 kHz
# (36) LNA enable
# (37) TX level 0 - 255
# (38) Not used
# (39) BS0 mode
#         0 - 4096 buffered samples
#         1 - 8192 buffered samples
#         2 - 16384 buffered samples
# (40) BS1 mode
#         0 - 4096 buffered samples
#         1 - 8192 buffered samples
#         2 - 16384 buffered samples 
# (41) BS0 period 50 - 255 ms (100ms is recommended)
# (42) BS1 period 50 - 255 ms (100ms is recommended)
# (43) Mic Boost, 0 - 1
# (44) Not used
# (45) PTT from PC
# (46) CW mode 0 - 1
# (47) CWX from PC - key manipulation
# (48) Read only PWR, Output power in Watts
# (49) Read only SWR x.x, from 1.0 to 9.9  as a natural number without decimal dot.
# (50) Key Speed 10 - 63 ppm (20 by default)
# (51) Paddle swap 0, 1
# (52) Key mode, 0 = straight, 01 = Mode A, 10 = Mode B
# (53) Key weight, 33 to 66, (50 by default)
# (54) Key spacing, 0, 1
# (55) ST Level, 0 - 255 sight tone level
# (56) ST Freq MSB- 4 MSB bits
# (57) ST Freq LSB - 8 LSB bits sight tone frequency in Hertz
# (58) Delay ON, 8 - 255 ms (25 by default) - time for TX carrier delay is needed for transmitter preparing to TX
# (59) Hang out time in tens ms 0 - 255 (0 to 2550 msec) (250 ms by default) needed to keep transmitter ready after PTT
# (60) Not used
# (61) Read only FW2 - firmware version MSB
# (62) Read only FW1
# (63) Read only FW0 - firmware version LSB

# Define the name of the hardware and the items on the hardware screen (see quisk_conf_defaults.py):
################ Receivers SdrMicronTRX, The SDR Micron TRX project by David Fainitski.
## hardware_file_name		Hardware file path, rfile
# This is the file that contains the control logic for each radio.
#hardware_file_name = 'microntrxpkg/quisk_hardware.py'


class Hardware(BaseHardware):
  sample_rates =  [48]  #[48, 96, 192, 384, 768, 1536]
  BS1_samples = {0:4096, 1:8192, 2:16384}
  BS1_bytes = {0:4096 * 2, 1:8192 * 2, 2:16384 * 2}	# Two bytes per sample
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.usb = None
    self.prefix = b'\x55\x55\x55\x55\x55\x55\x55\xD5'
    self.read_data = bytearray()
    self.rf_gain_labels = ('RF 0', 'RF -10', 'RF -20', 'RF -31')
    self.index = 1
    self.old_vfo = 0
    self.sdrmicron_clock = 76800000
    self.sdrmicron_decim = 1600
    self.bscope_data = bytearray()
    self.have_version = False
    self.frame_msg = ''
    self.remaining_BS1 = -1
    self.log_name = "microntrx_log.dat"
    self.log_time = 5.0		# seconds of log data
    self.log_time0 = None	# starting time.time() to start log file
    self.log_fp = None
    self.pc_control = bytearray(64)
    self.pc_control[0:8] = self.prefix
    self.pc_control[8:11] = b'UCP'
    self.pc_control[34] = 10	# Attenuator, 0 - 31 (dB value)
    self.pc_control[35] = 0	# BS0 rate: 0:48ksps, 1:96, 2:192, 3:384, 4:768, 5:1536
    self.pc_control[36] = 0	# LNA enable
    self.pc_control[39] = 0	# Buffered BS0 samples, 0:4096, 1:8192, 2:16384
    self.pc_control[40] = 0	# Buffered BS1 samples, 0:4096, 1:8192, 2:16384
    self.pc_control[41] = 100	# BS0 period msec
    self.pc_control[42] = 100	# BS1 period msec
    self.pc_control[58] = 25	# time for TX carrier delay needed for transmitter preparing to TX, msec
    self.pc_control[59] = 25	# tens of msec needed to keep transmitter ready
    
    if conf.fft_size_multiplier == 0:
      conf.fft_size_multiplier = 3		# Set size needed by VarDecim

  def open(self):	# This method must return a string showing whether the open succeeded or failed.
    rx_bytes = 3	# rx_bytes is the number of bytes in each I or Q sample: 1, 2, 3, or 4
    rx_endian = 1	# rx_endian is the order of bytes in the sample array: 0 == little endian; 1 == big endian
    self.InitSamples(rx_bytes, rx_endian)	# Initialize: read samples from this hardware file and send them to Quisk
    bs_bytes = 2
    bs_endian = 1
    self.InitBscope(bs_bytes, bs_endian, self.sdrmicron_clock, self.BS1_samples[self.pc_control[40]])	# Initialize bandscope
    if sys.platform == 'win32':
      return self.open_win()
    return "This code is Windows only"

  def open_win(self):
    try:
      import ftd2xx as d2xx
    except:
      return 'The ftd2xx module is missing'
    enum = d2xx.createDeviceInfoList() # quantity of FTDI devices
    if enum == 0:
        return 'There are no FTDI devices available'
    for i in range(enum):  # Searching and opening needed device
      a = d2xx.getDeviceInfoDetail(i)
      if DEBUG:
        print ("%d: description %s, serial %s" % (i, a['description'], a['serial']))
      if a['description']==b'SDR-Micron':
        try:
          self.usb = d2xx.openEx(a['serial'])
        except:
          return 'Device SDR-Micron could not be opened'
        self.usb.setBitMode(255, 0)  # reset
        time.sleep(0.1)
        self.usb.setBitMode(255, 64)  # Configure FT2232H into Sync FIFO mode 
        self.usb.setTimeouts(100, 100) # read, write
        self.usb.setLatencyTimer(2)
        self.usb.setUSBParameters(32, 32) # in_tx_size, out_tx_size
        time.sleep(1.5) # waiting for device initialisation
        self.usb.read(self.usb.getQueueStatus()) # clean the usb data buffer
        self.frame_msg = a['description'].decode('utf-8') + '   S/N - ' + a['serial'].decode('utf-8')
        return self.frame_msg
    return 'Device SDR-Micron was not found'

  def open_linux(self):		# Not finished
    try:
      import ftdi1 as d2xx
    except:
      return 'The ftdi1 module is missing'
    self.usb = d2xx.new()
    # Open Channel A (interface 1 in libftdi)
    d2xx.set_interface(self.usb, d2xx.INTERFACE_A)
    rc = d2xx.usb_open(self.usb, 0x0403, 0x6014) # 0x6014 is the standard FT232H PID
    if rc == 0:
      d2xx.usb_reset(self.usb)
      d2xx.set_latency_timer(self.usb, 2)
      # Enable Sync FIFO mode
      # 0xFF = pin mask, 0x40 = BITMODE_SYNCFF
      d2xx.set_bitmode(self.usb, 0xFF, d2xx.BITMODE_SYNCFF) 
      if DEBUG:
        print("Sync FIFO active via ftdi1.")
      # To read at maximum speed, use d2xx.read_data()
      d2xx.usb_close(self.usb)
    d2xx.free(self.usb)

  def close(self):
    if self.usb:
      self.usb.setBitMode(255, 0)  # reset
      time.sleep(0.5)
      self.usb.close()
      self.usb = None
    
  def OnButtonRfGain(self, event):
    btn = event.GetEventObject()
    n = btn.index
    if n == 0:
      self.pc_control[34] = 0
    elif n == 1:
      self.pc_control[34] = 10
    elif n == 2:
      self.pc_control[34] = 20
    elif n == 3:
      self.pc_control[34] = 31
    if DEBUG:
      print ('pc_control[34]', self.pc_control[34])
    self.rx_control_upd()

  def ChangeFrequency(self, tx_freq, vfo, source='', band='', event=None):
    if DEBUG:
      print('ChangeFrequency tx_freq', tx_freq, 'vfo', vfo)
    if tx_freq and tx_freq > 0:
      tx_freq = int(tx_freq - self.transverter_offset)
      self.pc_control[30] = self.pc_control[18] = (tx_freq >> 24) & 0xFF	# MSB
      self.pc_control[31] = self.pc_control[19] = (tx_freq >> 16) & 0xFF
      self.pc_control[32] = self.pc_control[20] = (tx_freq >>  8) & 0xFF
      self.pc_control[33] = self.pc_control[21] = (tx_freq      ) & 0xFF	#LSB
      self.rx_control_upd()
    if vfo:
      vfo =  int(vfo - self.transverter_offset)
      if vfo != self.old_vfo:
        self.old_vfo = vfo
        self.pc_control[26] = (vfo >> 24) & 0xFF	# MSB
        self.pc_control[27] = (vfo >> 16) & 0xFF
        self.pc_control[28] = (vfo >>  8) & 0xFF
        self.pc_control[29] = (vfo      ) & 0xFF	#LSB
        self.rx_control_upd()
    return tx_freq, vfo
  
  def VarDecimGetChoices(self): # Return a list/tuple of strings for the decimation control.
    return list(map(str, self.sample_rates)) # convert integer to string

  def VarDecimGetLabel(self):		# return a text label for the control
    return "Sample rate ksps"
  
  def VarDecimGetIndex(self):		# return the current index
    return self.index
  
  def VarDecimSet(self, index=None): # return sample rate
    if index is None: # initial call to set the sample rate before the call to open()
      rate = self.application.vardecim_set
      try:
        self.index = self.sample_rates.index(rate // 1000)
      except:
        self.index = 0
    else:
      self.index = index
    self.pc_control[35] = self.index
    rate = self.sample_rates[self.index] * 1000
    if DEBUG:
      print("varDecimSet index", self.index, "rate", rate)
    self.rx_control_upd()
    return rate
  
  def VarDecimRange(self):  # Return the lowest and highest sample rate.
    #return (48000, 1536000)
    return (48000, 48000)

  def StartSamples(self):	# called by the sound thread
    self.pc_control[12] = 1	# RX0 enable
    self.pc_control[15] = 1	# BS1 enable
    self.rx_control_upd()
    if WRITE_LOG:
      if self.log_fp is None:
        self.log_fp = open(self.log_name, "wb")
  
  def StopSamples(self):	# called by the sound thread
    self.pc_control[12] = 0
    self.pc_control[15] = 0
    self.rx_control_upd()

  def rx_control_upd(self):
    if self.usb:
      try:
        self.usb.write(self.pc_control)
      except:
        print('Error while rx_control_upd')  
    if self.pc_control[11] < 255:	# sequence number
      self.pc_control[11] += 1
    else:
      self.pc_control[11] = 0

  def GetRxSamples(self):	# Read from the MicronTRX. Called frequently from the sound thread.
    if not self.usb:
      return
    count = self.usb.getQueueStatus()	# Read all available bytes
    if count > 0:
      data = bytearray(self.usb.read(count))
      self.read_data += data
      if WRITE_LOG:
        if self.log_fp:
          if self.log_time0 is None:
            self.log_time0 = time.time()
          if time.time() - self.log_time0 < self.log_time:
            self.log_fp.write(data)
          else:
            self.log_fp.close()
            self.log_fp = None
    while True:
      start = self.read_data.find(self.prefix)
      if start < 0:	# No prefix
        return
      if start > 0:
        del self.read_data[0:start]
        if DEBUG:
          print ("USB blocks lost sync")
      if len(self.read_data) < 11:
        return
      block_type = self.read_data[8:11]
      if block_type == b'BS0':
        if len(self.read_data) < 508:
          return
        del self.read_data[0:508]
      elif block_type == b'BS1':
        if len(self.read_data) < 508:
          return
        packet_number = self.read_data[12]
        if packet_number == 0:			# start of a block of data
          self.remaining_BS1 = self.BS1_bytes[self.pc_control[40]]
          self.bscope_data = self.read_data[16:508]		# 492 bytes
          self.remaining_BS1 -= 492
        elif self.remaining_BS1 >= 492:
          self.bscope_data += self.read_data[16:508]		# 492 bytes
          self.remaining_BS1 -= 492
        elif self.remaining_BS1 <= 0:
          pass
        else:
          self.bscope_data += self.read_data[16:self.remaining_BS1 + 16]  
          self.remaining_BS1 = -1
          # self.AddBscopeSamples(self.bscope_data)
        del self.read_data[0:508]
      elif block_type == b'CDM':
        if len(self.read_data) < 180:
          return
        del self.read_data[0:180]
      elif block_type == b'RX0':
        if len(self.read_data) < 508:
          return
        if self.read_data[12]:
          self.GotClip()
        self.AddRxSamples(self.read_data[16:508])
        del self.read_data[0:508]
      elif block_type == b'RX1':
        if len(self.read_data) < 508:
          return
        del self.read_data[0:508]
      elif block_type == b'SBL':
        if len(self.read_data) < 32:
          return
        del self.read_data[0:32]
      elif block_type == b'UCP':
        if len(self.read_data) < 64:
          return
        if self.have_version is False:
          self.have_version = True
          self.frame_msg += '   F/W version - %d.%d' % (self.read_data[61], self.read_data[63])
          self.application.main_frame.SetConfigText(self.frame_msg)
        del self.read_data[0:64]
      else:
        del self.read_data[0:8]
        if DEBUG:
          print("Unknown USB block type")
