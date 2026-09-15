# This provides support for the SDR Micron TRX by David Fainitski, N7DDC

# On Windows this uses Python module ftd2xx. Install with pip.
# On Linux it uses ftdi1. Install python3-ftdi1 with your package manager.

import sys, wx, traceback, time
from quisk_hardware_model import Hardware as BaseHardware

DEBUG = 1	# print debug messages during operation
WRITE_LOG = 1	# write the data received from the MicronTRX during self.log_time to the file self.log_name
#WRITE_LOG = 2	# receive data from the file self.log_name instead of from USB

# https://github.com/Dfinitski/SDR-Micron

# Short Control Packet type "SCP" from PC to Micron TRX, 16 bytes, MSB first (big endian)
#
# (0 - 7) Preamble, seven 0x55 and 0xD5
# (8, 9, 10) "SCP"
#(11) RX0 enable, 0 - 1
#(12) RX1 enable, 0 - 1
#(13) BS0 enable, 0 - 1
#(14) TX0 enable, 0 - 1
#(15) CDC enable, 0 - 1

# Universal control packet type "UCP" from PC to TRX or from TRX to PC, 64 bytes, MSB first (big-endian)
#
# (0 - 7) Preamble, seven 0x55 and 0xD5
# (8, 9, 10) "UCP"
# (11) read only FW prefix MSB
# (12) read only FW prefix 
# (13) read only FW prefix LSB
# (14) read only char '-'
# (15) Not used
# (16) Not used
# (17) Not used
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
# (36) LNA enable
# (37) TX level 0 - 255
# (38) RX BPF bypass 0 - 1
# (39) BS0 mode
#         0 - 4096 buffered samples
#         1 - 8192 buffered samples
#         2 - 16384 buffered samples
# (40) Not used
# (41) Not used
# (42) read-only PTT state from device 0-1
# (43) Mic Boost, 0 - 1
# (44) TX LPF Bypass 0 - 1
# (45) PTT to device 1 - 0
# (46) CW mode on 0 - 1
# (47) CW key manipulation from PC to device 1 - 0
# (48) Read only PWR, Output power in Watts
# (49) Read only SWR x.x, from 1.0 to 9.9  as a natural number without decimal point
# (50) Key Speed 10 - 63 ppm (20 by default)
# (51) Paddle swap 0, 1
# (52) Key mode, 0 = straight, 01 = Mode A, 10 = Mode B
# (53) Key weight, 33 to 66, (50 by default)
# (54) Key auto spacing, 0, 1
# (55) ST Level, 0 - 255 sight tone level
# (56) ST Freq in tens Hertz, 40 - 100 (400 - 1000Hz), 70 by default 
# (57) Not used
# (58) Delay ON, 8 - 255 ms (25 by default) - time for TX carrier delay is needed for transmitter preparing to TX
# (59) Hang out time in tens ms 0 - 255 (0 to 2550 msec) (250 ms by default) needed to keep transmitter ready after PTT
# (60) Read only CW key manipulation from device to PC
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
  BS0_samples = {0:4096, 1:8192, 2:16384}
  BS0_bytes = {0:4096 * 2, 1:8192 * 2, 2:16384 * 2}	# Two bytes per sample
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.usb_windows = None
    self.usb_linux = None
    self.prefix = b'\x55\x55\x55\x55\x55\x55\x55\xD5'
    self.read_data = bytearray()
    self.tx_samples = b''
    self.rf_gain_labels = ('RF 0', 'RF -10', 'RF -20', 'RF -30')
    self.index = 1
    self.old_vfo = 0
    self.sdrmicron_clock = 76800000
    self.sdrmicron_decim = 1600
    self.bscope_data = bytearray()
    self.have_version = False
    self.frame_msg = ''
    self.remaining_BS0 = -1
    self.log_name = "microntrxpkg/microntrx_log.dat"
    self.log_time = 5.0		# seconds of log data
    self.log_time0 = None	# starting time.time() to start log file
    self.log_fp = None
    self.pc_control = bytearray(64)
    self.pc_control[0:8] = self.prefix
    self.pc_control[8:11] = b'UCP'
    self.got_defaults = 0
    self.pc_enables = bytearray(16)
    self.pc_enables[0:8] = self.prefix
    self.pc_enables[8:11] = b'SCP'
    self.fw_version = bytearray
    
    if conf.fft_size_multiplier == 0:
      conf.fft_size_multiplier = 3		# Set size needed by VarDecim

  def open(self):	# This method must return a string showing whether the open succeeded or failed.
    if WRITE_LOG == 1:
      self.log_fp = open(self.log_name, "wb")
    elif WRITE_LOG == 2:
      self.log_fp = open(self.log_name, "rb")
    rx_bytes = 3	# rx_bytes is the number of bytes in each I or Q sample: 1, 2, 3, or 4
    rx_endian = 1	# rx_endian is the order of bytes in the sample array: 0 == little endian; 1 == big endian
    self.InitSamples(rx_bytes, rx_endian)	# Initialize: read samples from this hardware file and send them to Quisk
    self.InitTxSamples(2, 1, 48000)			# Initialize Tx samples to send to the hardware
    bs_bytes = 2
    bs_endian = 1
    #self.InitBscope(bs_bytes, bs_endian, self.sdrmicron_clock, self.BS1_samples[self.pc_control[40]])	# Initialize bandscope
    if WRITE_LOG == 2:
      return "Read data from %s" % self.log_name
    elif sys.platform == 'win32':
      return self.open_win()
    else:
      return self.open_linux()

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
          usb = d2xx.openEx(a['serial'])
        except:
          return 'Device SDR-Micron could not be opened'
        usb.setBitMode(255, 0)  # reset
        time.sleep(0.1)
        usb.setBitMode(255, 64)  # Configure FT2232H into Sync FIFO mode 
        usb.setTimeouts(100, 100) # read, write
        usb.setLatencyTimer(2)
        usb.setUSBParameters(32, 32) # in_tx_size, out_tx_size
        time.sleep(1.5) # waiting for device initialisation
        usb.read(usb.getQueueStatus()) # clean the usb data buffer
        self.frame_msg = a['description'].decode('utf-8') + '   S/N - ' + a['serial'].decode('utf-8')
        self.usb_windows = usb
        return self.frame_msg
    return 'Device SDR-Micron was not found'

  def open_linux(self):		# Not finished
    try:
      import ftdi1
    except:
      return 'The ftdi1 module is missing'
    usb = ftdi1.new()
    # Open Channel A (interface 1 in libftdi)
    ftdi1.set_interface(usb, ftdi1.INTERFACE_A)
    rc = ftdi1.usb_open(usb, 0x0403, 0x6014) # 0x6014 is the standard FT232H PID
    if rc == 0:
      ftdi1.usb_reset(usb)
      ftdi1.set_usb_read_timout(usb, 1)
      ftdi1.set_usb_write_timout(usb, 5)
      ftdi1.set_latency_timer(usb, 2)
      # Enable Sync FIFO mode
      # 0xFF = pin mask, 0x40 = BITMODE_SYNCFF
      ftdi1.set_bitmode(usb, 0xFF, ftdi1.BITMODE_SYNCFF) 
      if DEBUG:
        print("Sync FIFO active via ftdi1.")
      self.usb_linux = usb
      return "Device SDR-Micron opened"
    return 'Device SDR-Micron was not found'

  def usb_read(self):
    if WRITE_LOG == 2:
      data = self.log_fp.read(120)
      return data
    elif self.usb_windows:
      count = self.usb_windows.getQueueStatus()	# Read all available bytes
      if count > 0:
        return self.usb_windows.read(count)	# return Python bytes
    elif self.usb_linux:
      count, data = ftdi1.read_data(self.usb_linux, 1024)
      if count > 0:
        return data	# data is Python bytes
      elif count < 0 and DEBUG:
        print("Error %d in usb_read", count)
    return None

  def usb_write(self, data):
    data = bytes(data)        # Convert bytearray to bytes
    if self.usb_windows:
      try:
        self.usb_windows.write(data)
      except:
        if DEBUG:
          print('Error while usb_write')  
    elif self.usb_linux:
      count = ftdi1.write_data(self.usb_linux, data)
      if count != len(data) and DEBUG:
        print("Failure in usb_write_data %d %d", count, len(data))

  def close(self):
    if self.usb_windows:
      self.usb_windows.setBitMode(255, 0)  # reset
      time.sleep(0.5)
      self.usb_windows.close()
      self.usb_windows = None
    elif self.usb_linux:
      ftdi1.usb_close(self.usb_linux)
      ftdi1.free(self.usb_linux)
      self.usb_linux = None
    
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
      self.pc_control[34] = 30
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
        self.pc_control[18] = (vfo >> 24) & 0xFF	# MSB
        self.pc_control[19] = (vfo >> 16) & 0xFF
        self.pc_control[20] = (vfo >>  8) & 0xFF
        self.pc_control[21] = (vfo      ) & 0xFF	#LSB
        self.rx_control_upd()
    return tx_freq, vfo

  def OnSpot(self, level):
    pass
  
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
    self.pc_enables[11] = 1	# RX0 enable
    #self.pc_enables[13] = 1	# BS0 enable
    self.usb_write(self.pc_enables)
    if DEBUG:
      print ("Start samples")
  
  def StopSamples(self):	# called by the sound thread
    self.pc_enables[11] = 0
    self.pc_enables[13] = 0
    self.usb_write(self.pc_enables)
    if DEBUG:
      print ("Stop Samples")

  def rx_control_upd(self):
    if self.got_defaults:
      self.usb_write(self.pc_control)

  def GetRxSamples(self):	# Read from the MicronTRX. Called frequently from the sound thread.
    # Send Tx samples to the hardware
    self.tx_samples += self.GetTxSamples()
    while len(self.tx_samples) >= 492:
      data = self.prefix + b'TX0\x00\x00\x00\x00\x00' + self.tx_samples[0:492]
      self.usb_write(data)
      #print(data[0:24], len(data))
      #i = data[16] << 8 | data[17]
      #print ("%8d" % i)
      self.tx_samples = self.tx_samples[492:]
    # Read messages from USB
    data = self.usb_read()
    if data:
      self.read_data += data
      if DEBUG > 1:
        print("Read %s bytes, buffer length %d" % (len(data), len(self.read_data)))
      if WRITE_LOG == 1:
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
        packet_number = self.read_data[11]
        if packet_number == 0:			# start of a block of data
          self.remaining_BS0 = self.BS0_bytes[self.pc_control[39]]
          self.bscope_data = self.read_data[16:508]		# 492 bytes
          self.remaining_BS0 -= 492
        elif self.remaining_BS0 >= 492:
          self.bscope_data += self.read_data[16:508]		# 492 bytes
          self.remaining_BS0 -= 492
        elif self.remaining_BS0 <= 0:
          pass
        else:
          self.bscope_data += self.read_data[16:self.remaining_BS0 + 16] 
          self.remaining_BS0 = -1
          # self.AddBscopeSamples(self.bscope_data)
        if DEBUG > 1:
          print("BS0 buffer length %d" % len(self.bscope_data))
        del self.read_data[0:508]
      elif block_type == b'CDM':
        if len(self.read_data) < 180:
          return
        if DEBUG > 1:
          print("CDM")
        del self.read_data[0:180]
      elif block_type == b'RX0':
        if len(self.read_data) < 508:
          return
        if self.read_data[12]:
          self.GotClip()
        self.AddRxSamples(self.read_data[16:508])
        if DEBUG > 1:
          print("RX0")
        del self.read_data[0:508]
      elif block_type == b'RX1':
        if len(self.read_data) < 508:
          return
        if DEBUG > 1:
          print("RX1")
        del self.read_data[0:508]
      elif block_type == b'UCP':
        if len(self.read_data) < 64:
          return
        if self.have_version is False:
          self.have_version = True
          self.fw_version = self.read_data[11:15] + self.read_data[61:64]
          self.frame_msg += '   F/W version - ' + self.fw_version.decode('utf-8')
          self.application.main_frame.SetConfigText(self.frame_msg)
        self.pc_control[11:64] = self.read_data[11:64] # save UCP content with defaults
        self.got_defaults = 1;
        if DEBUG > 1:
          print("UCP")
        del self.read_data[0:64]
      else:
        del self.read_data[0:11] 
        if DEBUG:
          print("Unknown USB block type")
