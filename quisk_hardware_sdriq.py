# Please do not change this hardware control module.
# It provides support for the SDR-IQ by RfSpace.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import wx, traceback, types
try:
  import serial
except:
  serial = None
from quisk_hardware_model import Hardware as BaseHardware

DEBUG = 0

if sys.version_info.major > 2:
  Q3StringTypes = str
else:
  Q3StringTypes = (str, unicode)

class Hardware(BaseHardware):
  decimations = [1250, 600, 500, 360]
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.rf_gain_labels = ('RF +30', 'RF +20', 'RF +10', 'RF 0 dB')
    if conf.fft_size_multiplier == 0:
      conf.fft_size_multiplier = 3		# Set size needed by VarDecim
    self.clock = int(conf.sdriq_clock)
    self.device = Sdriq(self, conf.sdriq_name, self.clock, conf.sdriq_decimation)	# SDR-IQ hardware access
    self.busy_cursor = False		# Is the busy cursor displayed?
    rx_bytes = 2	# rx_bytes is the number of bytes in each I or Q sample: 1, 2, 3, or 4
    rx_endian = 0	# rx_endian is the order of bytes in the sample array: 0 == little endian; 1 == big endian
    self.InitSamples(rx_bytes, rx_endian)	# Initialize: read samples from this hardware file and send them to Quisk
  def open(self):	# This method must return a string showing whether the open succeeded or failed.
    return self.device.open()
  def close(self):
    self.device.close()
  def OnButtonRfGain(self, event):
    """Set the SDR-IQ preamp gain and attenuator state.

    self.device.SetGain(gstate, gain)
    gstate == 0:  Gain must be 0, -10, -20, or -30
    gstate == 1:  Attenuator is on  and gain is 0 to 127 (7 bits)
    gstate == 2:  Attenuator is off and gain is 0 to 127 (7 bits)
    gain for 34, 24, 14, 4 db is 127, 39, 12, 4.
    """
    btn = event.GetEventObject()
    n = btn.index
    if n == 0:
      self.device.SetGain(2, 127)
    elif n == 1:
      self.device.SetGain(2, 39)
    elif n == 2:
      self.device.SetGain(2, 12)
    elif n == 3:
      self.device.SetGain(1, 12)
    else:
      print ('Unknown RfGain')
  def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
    if vfo:
      self.device.SetFrequency(vfo - self.transverter_offset)
    return tune, vfo
  def ChangeBand(self, band):
    # band is a string: "60", "40", "WWV", etc.
    BaseHardware.ChangeBand(self, band)
    btn = self.application.BtnRfGain
    if btn:
      if band in ('160', '80', '60', '40'):
        btn.SetLabel('RF +10', True)
      elif band in ('20',):
        btn.SetLabel('RF +20', True)
      else:
        btn.SetLabel('RF +20', True)
  def VarDecimGetChoices(self):		# return text labels for the control
    l = []		# a list of sample rates
    for dec in self.decimations:
      l.append(str(int(float(self.clock) / dec / 1e3 + 0.5)))
    return l
  def VarDecimGetLabel(self):		# return a text label for the control
    return "Sample rate ksps"
  def VarDecimGetIndex(self):		# return the current index
    return self.index
  def VarDecimSet(self, index=None):		# set decimation, return sample rate
    if index is None:		# initial call to set decimation before the call to open()
      rate = self.application.vardecim_set		# May be None or from different hardware
      try:
        dec = int(float(self.clock / rate + 0.5))
        self.index = self.decimations.index(dec)
      except:
        try:
          self.index = self.decimations.index(self.conf.sdriq_decimation)
        except:
          self.index = 0
    else:
      self.index = index
    dec = self.decimations[self.index]
    self.device.SetDecimation(dec)
    if index is not None:
      wx.BeginBusyCursor()
      self.busy_cursor = True
    return int(float(self.clock) / dec + 0.5)
  def HeartBeat(self):
    if self.busy_cursor:
      if self.device.sdriq_decimation == self.device.new_decimation:
        wx.EndBusyCursor()
        self.busy_cursor = False
  def StartSamples(self):	# called by the sound thread
    self.device.StartSamples()
  def GetRxSamples(self):	# called by the sound thread
    # Quisk will call this frequently from the sound thread. Call AddRxSamples() to return the samples.
    self.device.GetRxSamples()
  def StopSamples(self):	# called by the sound thread
    self.device.StopSamples()

# This class provides access to the SDR-IQ by RfSpace.
class Sdriq:
  TYPE_HOST_SET = 0x00
  TYPE_HOST_GET = 0x20
  SDRIQ_READ_TIME = 0.004	# Number of seconds to wait for SDR-IQ data on each read
  def __init__(self, hardware, name, clock, decim):
    self.hardware = hardware
    self.sdriq_name = name		# port name such as "/dev/ttyUSB0" or "COM6"
    self.sdriq_clock = clock
    self.sdriq_decimation = decim	# currently programmed decimation
    self.new_decimation = decim		# new requested decimation
    self.sdriq_idle = -1
    self.port = None
    self.sdriq_gstate = self.new_gstate = 2
    self.sdriq_gain = self.new_gain = 127
    self.sdriq_freq = self.new_freq = 7220000
  def open(self):
    self.sdr_name = ''			# name as reported by the hardware
    self.sdr_serial = ''		# serial number as reported by the hardware
    self.sdr_data = bytearray(0)	# the data block sent by the SDR-IQ
    self.sdr_state = 0
    if not serial:
      self.port = None
      return 'SDR-IQ requires the missing Python "serial" module'
    try:
      self.port = serial.Serial(self.sdriq_name, baudrate=230400, timeout=self.SDRIQ_READ_TIME)
    except:
      if DEBUG:
        traceback.print_exc()
      self.port = None
      return "Can not open SDR-IQ port name %s" % self.sdriq_name
    self.SetItem(0x0018, b"\x81\x01\x00\x00")
    self.port.reset_input_buffer()
    self.port.reset_output_buffer()
    self.SetItem(0x0018, b"\x81\x01\x00\x00")
    self.GetItem(0x0002, b'')		# request serial number
    self.GetItem(0x0005, b'')		# request status
    self.GetItem(0x0001, b'')		# request name
    for i in range(50):
      self.ReadSdriq()	# read the port for data from the GUI thread
      if self.sdr_name:
        break
    else:
      self.port.close()
      self.port = None
      return "No response from SDR-IQ"
    # set the clock speed
    freq = self.sdriq_clock
    data = bytearray(5)
    data[0] = 0
    data[1] = freq & 0xFF
    freq = freq >> 8
    data[2] = freq & 0xFF
    freq = freq >> 8
    data[3] = freq & 0xFF
    freq = freq >> 8
    data[4] = freq & 0xFF
    self.SetItem(0x00B0, data)
    t = "Capture from %s serial %s" % (self.sdr_name, self.sdr_serial)
    if DEBUG: print("Open device:", t)
    self.ProgramAD6620()
    return t
  def close(self):
    if self.port:
      self.port.close()
    self.port = None
  def StartSamples(self):
    if self.sdriq_idle != 2 and self.port:
      buf = bytearray(4)
      buf[0] = 0x81
      buf[1] = 0x02
      buf[2] = 0x00
      buf[3] = 0x01
      self.SetItem(0x0018, buf)
      if DEBUG: print ("StartSamples")
  def StopSamples(self):
    if not self.port:
      return
    if DEBUG: print ("StopSamples")
    buf = bytearray(4)
    buf[0] = 0x81
    buf[1] = 0x01
    buf[2] = 0x00
    buf[3] = 0x00
    for i in range(10):
      self.SetItem(0x0018, buf)
      self.ReadSdriq()
      if self.sdriq_idle == 1:
        if DEBUG: print ("StopSamples at index", i)
        break
  def SetDecimation(self, decim):
    self.new_decimation = decim	
  def SetFrequency(self, freq):
    self.new_freq = freq
  def SetGain(self, gstate, gain):
    self.new_gstate = gstate
    self.new_gain = gain
  def GetItem(self, item, params):
    length = 4 + len(params)
    data = bytearray(4)
    data[0] = length & 0xFF		# length LSB
    data[1] = self.TYPE_HOST_GET | ((length >> 8) & 0x1F)	# 3-bit type and 5-bit length MSB
    data[2] = item & 0xFF		# item LSB
    data[3] = (item >> 8) & 0xFF	# item MSB
    data = data + params
    if self.port.write(data) != length:
      self.hardware.GotReadError(DEBUG, "SDR-IQ GetItem write error")
  def SetItem(self, item, params):
    length = 4 + len(params)
    data = bytearray(4)
    data[0] = length & 0xFF				# length LSB
    data[1] = self.TYPE_HOST_SET | ((length >> 8) & 0x1F)	# 3-bit type and 5-bit length MSB
    data[2] = item & 0xFF			# item LSB
    data[3] = (item >> 8) & 0xFF	# item MSB
    data = data + params
    if self.port.write(data) != length:
      self.hardware.GotReadError(DEBUG, "SDR-IQ SetItem write error")
  def ProgramFrequency(self):
    freq = self.sdriq_freq
    buf = bytearray(6)
    buf[0] = 0
    buf[1] = freq & 0xFF
    freq = freq >> 8
    buf[2] = freq & 0xFF
    freq = freq >> 8
    buf[3] = freq & 0xFF
    freq = freq >> 8
    buf[4] = freq & 0xFF
    buf[5] = 1
    self.SetItem(0x0020, buf)
  def ProgramGain(self):
    gain = self.sdriq_gain
    gstate = self.sdriq_gstate
    buf = bytearray(2)
    if gstate == 0:
      buf[0] = 0
      buf[1] = gain & 0xFF
    elif gstate == 1:
      buf[0] = 1
      buf[1] = gain & 0x7F
      buf[1] |= 0x80
    else:
      buf[0] = 1
      buf[1] = gain & 0x7F
    self.SetItem(0x0038, buf)
  def GetRxSamples(self):	# Check for changes in decimation, frequency or gain.
    if not self.port:		# Poll the device for samples.
      return None
    if self.sdriq_decimation != self.new_decimation:
      if DEBUG: print ("Set decimation to", self.new_decimation, "currently", self.sdriq_decimation)
      self.StopSamples()
      self.ProgramAD6620()
      self.StartSamples()
      self.sdriq_decimation = self.new_decimation
    if self.sdriq_freq != self.new_freq:
      self.sdriq_freq = self.new_freq
      self.ProgramFrequency()
    if self.sdriq_gain != self.new_gain or self.sdriq_gstate != self.new_gstate:
      self.sdriq_gain = self.new_gain
      self.sdriq_gstate = self.new_gstate
      self.ProgramGain()
    self.ReadSdriq()
  def ReadSdriq(self):	# Read all data from the SDR-IQ and process it.
    # The ft245 driver does not have a circular buffer for input; bytes are just appended
    # to the buffer.  When all bytes are read and the buffer goes empty, the pointers are reset to zero.
    # Be sure to empty out the ft245 frequently so its buffer does not overflow.
    if not self.port:
      return
    data = self.port.read(8192)		# this is a blocking read for SDRIQ_READ_TIME seconds
    if isinstance(data, Q3StringTypes):
      data = bytearray(data)
    index = 0
    length = len(data)
    while index < length:
      if self.sdr_state == 0:		# read the first byte
        del self.sdr_data[:]
        byte = data[index]
        index += 1
        self.sdr_length = byte
        self.sdr_state = 1
      elif self.sdr_state == 1:		# read the second byte
        byte = data[index]
        index += 1
        self.sdr_type = (byte >> 5) & 0x7		# 3-bit type
        self.sdr_length |= (byte & 0x1F) << 8		# length including header
        if self.sdr_length == 0:
          if self.sdr_type > 3:		# special length
            self.sdr_length = 8194
          else:				# NAK
            self.sdr_nak = 1
            self.sdr_state = 0
            continue
        self.sdr_length -= 2
        if self.sdr_length <= 0 or (self.sdr_length > 50 and self.sdr_length < 8192):	# out of sync
          self.hardware.GotReadError(DEBUG, "SDR-IQ lost sync: type %d  length %d" % (self.sdr_type, self.sdr_length))
          self.sdr_state = 9
        else:
          self.sdr_state = 2
      elif self.sdr_state == 2:		# read all the "sdr_length" bytes
        index2 = index + self.sdr_length - len(self.sdr_data)
        self.sdr_data += data[index:index2]
        index = index2
        if len(self.sdr_data) >= self.sdr_length:	# we have all the data for this record
          self.sdr_state = 0
          if DEBUG > 1:
            print("Got data type %d length %d" % (self.sdr_type, self.sdr_length))
          if self.sdr_length == 1 and self.sdr_type == 3:	# ACK
            self.sdr_ack = self.sdr_data[0]
          elif self.sdr_type < 2 and self.sdr_length >= 2:	# control item
            item = self.sdr_data[0] | self.sdr_data[1] << 8
            if item == 1:
              self.sdr_name = self.sdr_data[2:-1].decode('utf-8')
            elif item == 2:
              self.sdr_serial = self.sdr_data[2:-1].decode('utf-8')
            elif item == 3:
              self.sdr_interface = self.sdr_data[3] << 8 | self.sdr_data[2]
            elif item == 4:
              if self.sdr_data[2]:
                self.sdr_firmware = self.sdr_data[4] << 8 | self.sdr_data[3]
              else:
                self.sdr_bootcode = self.sdr_data[4] << 8 | self.sdr_data[3]
            elif item == 5:
              self.sdr_status = self.sdr_data[2]
              if self.sdr_status == 0x20:
                self.hardware.GotClip()
            elif item == 0x18:
              self.sdriq_idle = self.sdr_data[3]
              if (DEBUG): print("sdriq_idle", self.sdriq_idle)
          elif self.sdr_type == 4 and self.sdr_length == 8192:	# ADC sample block
            self.hardware.AddRxSamples(self.sdr_data)
      elif self.sdr_state == 9:		# out of sync; try to re-synchronize
        # look for the start of data blocks "\x00\x80"
        byte = data[index]
        index += 1
        if byte == 0x00:
          self.sdr_state = 10
      elif self.sdr_state == 10:
        byte = data[index]
        index += 1
        if byte == 0x80:
          del self.sdr_data[:]
          self.sdr_length = 8192
          self.sdr_state = 2
        elif byte != 0x00:
          self.sdr_state = 9
  def SetAD6620(self, address, value):		# set an AD6620 register
    buf = bytearray(9)
    buf[0] = 0x09
    buf[1] = 0xA0
    buf[2] = address & 0xFF
    buf[3] = (address >> 8) & 0xFF
    buf[4] = value & 0xFF
    value = value >> 8
    buf[5] = value & 0xFF
    value = value >> 8
    buf[6] = value & 0xFF
    value = value >> 8
    buf[7] = value & 0xFF
    buf[8] = 0
    if self.port.write(buf) != len(buf):
      self.hardware.GotReadError(DEBUG, "SDR-IQ SetAD6620 write error")
  def WsetAD6620(self, address, value):		# set an AD6620 register and wait for the ACK
    self.sdr_ack = -1
    self.SetAD6620(address, value)
    for i in range(50):
      self.ReadSdriq()
      if self.sdr_ack != -1:
        break
    if self.sdr_ack != 1:
      self.hardware.GotReadError(DEBUG, "SDR-IQ failed to get ACK for AD6620 address 0x%X" % address)
  def ProgramAD6620(self):
    decim = self.new_decimation
    if decim == 360:
      scale = (4, 18, 5, 4, 13, 6)
      coef = (
131, -230, -38, -304, -235, -346, -237, -181, 12, 149, 310, 349, 320, 154, -60,
-310, -480, -540, -423, -169, 187, 523, 749, 762, 543, 117, -394, -851, -1093, -1025,
-621, 22, 737, 1300, 1522, 1288, 625, -309, -1245, -1893, -2013, -1515, -489, 793, 1957,
 2623, 2533, 1640, 149, -1533, -2893, -3475, -3023, -1584, 480, 2582, 4063, 4405, 3401, 1246,
-1484, -3986, -5455, -5345, -3557, -509, 2951, 5776, 7030, 6193, 3355, -760, -4970, -7969, -8722,
-6815, -2628, 2712, 7632, 10563, 10431, 7033, 1169, -5529, -11037, -13543, -12021, -6623, 1287, 9443,
 15320, 16896, 13319, 5269, -5122, -14811, -20711, -20642, -14088, -2504, 10961, 22272, 27682, 24909, 13986,
-2524, -20051, -33214, -37378, -30153, -12380, 11742, 35506, 51387, 53179, 38008, 7662, -31208, -68176, -91255,
-89756, -57102, 7096, 96306, 197916, 295555, 372388, 414662, 414662, 372388, 295555, 197916, 96306, 7096, -57102,
-89756, -91255, -68176, -31208, 7662, 38008, 53179, 51387, 35506, 11742, -12380, -30153, -37378, -33214, -20051,
-2524, 13986, 24909, 27682, 22272, 10961, -2504, -14088, -20642, -20711, -14811, -5122, 5269, 13319, 16896,
 15320, 9443, 1287, -6623, -12021, -13543, -11037, -5529, 1169, 7033, 10431, 10563, 7632, 2712, -2628,
-6815, -8722, -7969, -4970, -760, 3355, 6193, 7030, 5776, 2951, -509, -3557, -5345, -5455, -3986,
-1484, 1246, 3401, 4405, 4063, 2582, 480, -1584, -3023, -3475, -2893, -1533, 149, 1640, 2533,
 2623, 1957, 793, -489, -1515, -2013, -1893, -1245, -309, 625, 1288, 1522, 1300, 737, 22,
-621, -1025, -1093, -851, -394, 117, 543, 762, 749, 523, 187, -169, -423, -540, -480,
-310, -60, 154, 320, 349, 310, 149, 12, -181, -237, -346, -235, -304, -38, -230, 131)
    elif decim == 500:
      scale = (4, 25, 5, 4, 16, 5)
      coef = (
-197, 356, -153, 176, -101, 34, -125, -46, -106, -7, 12, 115, 129,
 157, 86, 12, -116, -197, -251, -203, -97, 80, 242, 364, 367,
 259, 33, -228, -461, -565, -504, -255, 106, 488, 756, 813, 604,
 172, -377, -868, -1139, -1066, -639, 53, 807, 1390, 1584, 1288, 537,
-470, -1439, -2046, -2060, -1406, -232, 1143, 2290, 2820, 2496, 1339, -366,
-2120, -3369, -3659, -2808, -976, 1340, 3448, 4652, 4486, 2873, 198, -2785,
-5152, -6095, -5184, -2546, 1137, 4785, 7240, 7613, 5604, 1641, -3190, -7438,
-9701, -9091, -5546, 69, 6163, 10849, 12519, 10373, 4745, -2905, -10342, -15198,
-15692, -11253, -2807, 7368, 16229, 20838, 19296, 11436, -946, -14436, -24891, -28637,
-23657, -10406, 8025, 26518, 39215, 41181, 30008, 6896, -23122, -51997, -70364, -69788,
-44995, 4465, 73600, 152608, 228689, 288639, 321648, 321648, 288639, 228689, 152608, 73600,
 4465, -44995, -69788, -70364, -51997, -23122, 6896, 30008, 41181, 39215, 26518, 8025,
-10406, -23657, -28637, -24891, -14436, -946, 11436, 19296, 20838, 16229, 7368, -2807,
-11253, -15692, -15198, -10342, -2905, 4745, 10373, 12519, 10849, 6163, 69, -5546,
-9091, -9701, -7438, -3190, 1641, 5604, 7613, 7240, 4785, 1137, -2546, -5184,
-6095, -5152, -2785, 198, 2873, 4486, 4652, 3448, 1340, -976, -2808, -3659,
-3369, -2120, -366, 1339, 2496, 2820, 2290, 1143, -232, -1406, -2060, -2046,
-1439, -470, 537, 1288, 1584, 1390, 807, 53, -639, -1066, -1139, -868,
-377, 172, 604, 813, 756, 488, 106, -255, -504, -565, -461, -228,
 33, 259, 367, 364, 242, 80, -97, -203, -251, -197, -116, 12,
 86, 157, 129, 115, 12, -7, -106, -46, -125, 34, -101, 176, -153, 356, -197)
    elif decim == 600:
      scale = (5, 30, 4, 5, 17, 5)
      coef = (
 436, -1759, 99, -1281, 0, -280, 619, 409, 553, -71, -344, -753, -537, -203,
 453, 782, 838, 325, -326, -949, -1037, -628, 230, 991, 1330, 923, 10, -1032,
-1569, -1324, -299, 956, 1822, 1739, 716, -809, -2000, -2212, -1212, 520, 2123, 2678,
 1823, -111, -2124, -3143, -2509, -463, 2002, 3548, 3279, 1188, -1699, -3877, -4088,
-2087, 1206, 4069, 4920, 3137, -478, -4094, -5720, -4343, -493, 3887, 6454, 5669, 1741,
-3412, -7052, -7096, -3266, 2607, 7462, 8573, 5084, -1425, -7602, -10058, -7187, -193,
 7400, 11481, 9579, 2301, -6756, -12777, -12244, -4971, 5569, 13854, 15181, 8285, -3699,
-14613, -18387, -12369, 966, 14920, 21888, 17412, 2905, -14598, -25744, -23754, -8362,
 13363, 30114, 32035, 16259, -10708, -35362, -43638, -28445, 5493, 42387, 62053, 49891, 5603, -53825,
-99044, -99811, -38467, 80479, 229234, 365232, 446270, 446270, 365232, 229234, 80479, -38467,
-99811, -99044, -53825, 5603, 49891, 62053, 42387, 5493, -28445, -43638, -35362, -10708, 16259,
 32035, 30114, 13363, -8362, -23754, -25744, -14598, 2905, 17412, 21888, 14920, 966, -12369,
-18387, -14613, -3699, 8285, 15181, 13854, 5569, -4971, -12244, -12777, -6756, 2301, 9579,
 11481, 7400, -193, -7187, -10058, -7602, -1425, 5084, 8573, 7462, 2607, -3266, -7096, -7052, -3412,
 1741, 5669, 6454, 3887, -493, -4343, -5720, -4094, -478, 3137, 4920, 4069, 1206, -2087, -4088,
-3877, -1699, 1188, 3279, 3548, 2002, -463, -2509, -3143, -2124, -111, 1823, 2678, 2123, 520, -1212,
-2212, -2000, -809, 716, 1739, 1822, 956, -299, -1324, -1569, -1032, 10, 923, 1330, 991, 230, -628,
-1037, -949, -326, 325, 838, 782, 453, -203, -537, -753, -344, -71, 553, 409, 619, -280, 0, -1281,
 99, -1759, 436)
    else:	# decim == 1250
      scale = (10, 25, 5, 7, 15, 6)
      coef = (
-378, 13756, -14444, 8014, -7852, 3556, -3779, 2733, -909, 2861, 208, 1827, -755, -243, -2134, -1267, -1705,
 20, 492, 2034, 1885, 1993, 535, -459, -2052, -2387, -2454, -1112, 246, 2053, 2832, 3019, 1774, 133, -1973,
-3220, -3654, -2546, -683, 1769, 3531, 4330, 3431, 1417, -1400, -3730, -5013, -4428, -2350, 831, 3780, 5669,
 5520, 3489, -23, -3635, -6252, -6689, -4839, -1057, 3245, 6715, 7904, 6403, 2443, -2555, -6998, -9129, -8175,
-4172, 1504, 7033, 10318, 10147, 6281, -23, -6747, -11415, -12315, -8815, -1972, 6041, 12354, 14669, 11830, 4593,
-4800, -13060, -17207, -15419, -7992, 2861, 13425, 19944, 19729, 12404, 21, -13318, -22930, -25017, -18239, -4245,
 12519, 26289, 31789, 26259, 10571, -10635, -30306, -41114, -38121, -20661, 6795, 35686, 55688, 58124, 39093, 1561,
-44548, -84372, -101901, -84500, -26969, 66196, 180937, 296484, 390044, 442339, 442339, 390044, 296484, 180937,
 66196, -26969, -84500, -101901, -84372, -44548, 1561, 39093, 58124, 55688, 35686, 6795, -20661, -38121, -41114,
-30306, -10635, 10571, 26259, 31789, 26289, 12519, -4245, -18239, -25017, -22930, -13318, 21, 12404, 19729, 19944,
 13425, 2861, -7992, -15419, -17207, -13060, -4800, 4593, 11830, 14669, 12354, 6041, -1972, -8815, -12315, -11415,
-6747, -23, 6281, 10147, 10318, 7033, 1504, -4172, -8175, -9129, -6998, -2555, 2443, 6403, 7904, 6715, 3245, -1057,
-4839, -6689, -6252, -3635, -23, 3489, 5520, 5669, 3780, 831, -2350, -4428, -5013, -3730, -1400, 1417, 3431, 4330,
 3531, 1769, -683, -2546, -3654, -3220, -1973, 133, 1774, 3019, 2832, 2053, 246, -1112, -2454, -2387, -2052, -459,
 535, 1993, 1885, 2034, 492, 20, -1705, -1267, -2134, -243, -755, 1827, 208, 2861, -909, 2733, -3779, 3556, -7852,
 8014, -14444, 13756, -378 )
    self.WsetAD6620(0x300, 1)
    for i in range(256):
      self.WsetAD6620(i, coef[i])
    self.WsetAD6620(0x301, 0)
    self.WsetAD6620(0x302, -1)
    self.WsetAD6620(0x303, 0)
    self.WsetAD6620(0x304, 0)
    self.WsetAD6620(0x305, scale[3])
    self.WsetAD6620(0x306, scale[0] - 1)
    self.WsetAD6620(0x307, scale[4])
    self.WsetAD6620(0x308, scale[1] - 1)
    self.WsetAD6620(0x309, scale[5])
    self.WsetAD6620(0x30A, scale[2] - 1)
    self.WsetAD6620(0x30B, 0)
    self.WsetAD6620(0x30C, 255)
    self.WsetAD6620(0x30D, 0)
    self.ProgramFrequency()
    self.ProgramGain()
    self.WsetAD6620(0x300, 0)
