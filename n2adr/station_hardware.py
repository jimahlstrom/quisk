# This file supports various hardware boxes at my shack

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import sys, struct, math, socket, select, time, traceback, os
import wx, wx.lib.buttons

DEBUG = 0

gatewayTime = 0			# time of last gateway command
gatewayLimit = 0.2	# minimum time between gateway commands

class ControlBox:	# Control my station control box
  address = ('192.168.1.194', 0x3A00 + 64)
  def __init__(self, app, conf):
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.setblocking(0)
    self.socket.connect(self.address)
    self.have_data = b'C\000'
    self.want_data = b'C\000'
    self.timer = 0
  def close(self):
    self.want_data = b'C\000'	# raise key if down
    if self.have_data != self.want_data:
      self.socket.send(self.want_data)
      time.sleep(0.1)
      self.socket.send(self.want_data)
  def OnButtonPTT(self, event):
    btn = event.GetEventObject()
    if btn.GetValue():		# Turn the software key bit on or off
      self.want_data = b'C\001'
    else:
      self.want_data = b'C\000'
  def SetKeyDown(self, down):
    if down:			# Turn the software key bit on or off
      self.want_data = b'C\001'
    else:
      self.want_data = b'C\000'
  def HeartBeat(self):
    global gatewayTime
    if not self.socket:
      return
    try:	# The control box echoes its commands
      self.have_data = self.socket.recv(50)
    except socket.error:
      pass
    except socket.timeout:
      pass
    if self.have_data != self.want_data and time.time() - gatewayTime > gatewayLimit:
      gatewayTime = time.time()
      if self.timer <= 10:
        self.timer += 1
        if self.timer == 10:
          print ('Control box error')
      try:
        self.socket.send(self.want_data)
      except socket.error:
        pass
      except socket.timeout:
        pass

class LowPassFilter:	# Control my low pass filter box
  address = ('192.168.1.194', 0x3A00 + 39)
  # Filters are numbered 1 thru 8 for bands: 80, 15, 60, 40, 30, 20, 17, short
  lpfnum = (1, 1, 1, 1, 1, 3,	# frequency 0 thru 5 MHz
               4, 4, 5, 5, 5,	# 6 thru 10
               6, 6, 6, 6, 7,	# 11 thru 15
               7, 7, 7, 2, 2,	# 16 thru 20
               2, 2, 8, 8, 8)	# 21 thru 25; otherwise the filter is 8
  def __init__(self, app, conf):
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.setblocking(0)
    self.socket.connect(self.address)
    self.have_data = None
    self.want_data = b'\000'
    self.old_tx_freq = 0
    self.timer = 0
  def ChangeBand(self, band):
    pass
  def SetTxFreq(self, tx_freq):
    if not self.socket:
      return
    # Filters are numbered 1 thru 8
    if abs(self.old_tx_freq - tx_freq) < 100000:
      return	# Ignore small tuning changes
    self.old_tx_freq = tx_freq
    try:		# Look up filter number based on MHz
      num = self.lpfnum[tx_freq // 1000000]
    except IndexError:
      num = 8
    self.want_data =  bytearray((num, ))
    self.timer = 0
    #print ("LP filter band %d" % num)
  def HeartBeat(self):
    global gatewayTime
    if not self.socket:
      return
    try:	# The HP filter box echoes its commands
      self.have_data = self.socket.recv(50)
    except socket.error:
      pass
    except socket.timeout:
      pass
    if self.have_data != self.want_data and time.time() - gatewayTime > gatewayLimit:
      gatewayTime = time.time()
      if self.timer <= 10:
        self.timer += 1
        if self.timer == 10:
          print ('Low pass filter error')
      try:
        self.socket.send(self.want_data)
      except socket.error:
        pass
      except socket.timeout:
        pass

class HighPassFilter:	# Control my high pass filter box
  address = ('192.168.1.194', 0x3A00 + 21)
  def __init__(self, app, conf):
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.preamp = 0
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.setblocking(0)
    self.socket.connect(self.address)
    self.have_data = None
    self.want_data = bytes(3)
    self.old_tx_freq = 0
    self.timer = 0
  def ChangeBand(self, band):
    btn = self.application.BtnRfGain
    freq = self.application.VFO + self.application.txFreq
    if self.conf.use_sdriq:
      if btn:
        if freq < 5000000:
          btn.SetLabel('RF -10', True)
        elif freq < 13000000:
          btn.SetLabel('RF 0 dB', True)
        else:
          btn.SetLabel('RF +16', True)
    elif self.conf.use_rx_udp:
      if btn:
        if freq < 5000000:
          btn.SetLabel('RF 0 dB', True)
        elif freq < 13000000:
          btn.SetLabel('RF 0 dB', True)
        else:
          btn.SetLabel('RF +16', True)
  def OnButtonRfGain(self, event):
    """Set my High Pass Filter Box preamp gain and attenuator state."""
    btn = event.GetEventObject()
    n = btn.index
    if n == 0:		# 0dB
      self.preamp = 0x00
      self.application.Hardware.rf_gain = 0
    elif n == 1:	# +16
      self.preamp = 0x02
      self.application.Hardware.rf_gain = 16
    elif n == 2:	# -20
      self.preamp = 0x0C
      self.application.Hardware.rf_gain = -20
    elif n == 3:	# -10
      self.preamp = 0x04
      self.application.Hardware.rf_gain = -10
    else:
      print ('Unknown RfGain')
      self.application.Hardware.rf_gain = 0
    self.SetTxFreq(None)
  def SetTxFreq(self, tx_freq):
    """Set high pass filter and preamp/attenuator state"""
    # Filter cutoff in MHz: 0.0, 2.7, 3.95, 5.7, 12.6, 18.2, 22.4
    # Frequency MHz     Bits       Hex      Band
    # =============     ====       ===      ====
    #   0   to  2.70    PORTD, 0   0x01      160
    #  2.7  to  3.95    PORTB, 1   0x02       80
    #  3.95 to  5.70    PORTD, 7   0x80       60
    #  5.70 to 12.60    PORTB, 0   0x01       40, 30
    # 12.60 to 18.20    PORTD, 6   0x40       20, 17
    # 18.20 to 22.40    PORTB, 7   0x80       15
    # 22.40 to 99.99    PORTB, 6   0x40       12, 10
    # Other bits:  Preamp PORTD 0x02, Atten1 PORTD 0x04, Atten2 PORTD 0x08
    if not self.socket:
      return
    if tx_freq is None:
      tx_freq = self.old_tx_freq
    elif abs(self.old_tx_freq - tx_freq) < 100000:
      return	# Ignore small tuning changes
    self.old_tx_freq = tx_freq
    portb = portc = portd = 0
    if self.conf.use_sdriq:
      if tx_freq < 15000000:	# Turn preamp on/off
        self.preamp = 0x00
      else:
        self.preamp = 0x02
    elif self.conf.use_rx_udp:
      pass		# self.preamp is already set
    else:		# turn preamp off
      self.preamp = 0x00
    if tx_freq < 12600000:
      if tx_freq < 3950000:
        if tx_freq < 2700000:
          portd = 0x01
        else:
          portb = 0x02
      elif tx_freq < 5700000:
        portd = 0x80
      else:
        portb = 0x01
    elif tx_freq < 18200000:
      portd = 0x40
    elif tx_freq < 22400000:
      portb = 0x80
    else:
      portb = 0x40
    portd |= self.preamp
    self.want_data = bytearray((portb, portc, portd))
    self.timer = 0
  def HeartBeat(self):
    global gatewayTime
    if not self.socket:
      return
    try:	# The HP filter box echoes its commands
      self.have_data = self.socket.recv(50)
    except socket.error:
      pass
    except socket.timeout:
      pass
    if self.have_data != self.want_data and time.time() - gatewayTime > gatewayLimit:
      gatewayTime = time.time()
      if self.timer <= 10:
        self.timer += 1
        if self.timer == 10:
          print ('High pass filter error')
      try:
        self.socket.send(self.want_data)
      except socket.error:
        pass
      except socket.timeout:
        pass

class FilterBoxV2:	# Control my 2016 high/low pass filter box
  address = ('192.168.1.194', 0x3A00 + 70)
  # Low pass filters are numbered 0 thru 5 for bands: 10, 15, 17, 20, 40, 60
  lpfnum = (5, 5, 5, 5, 5, 5,	# frequency 0 thru 5 MHz
               4, 4, 3, 3, 3,	# 6 thru 10
               3, 3, 3, 3, 2,	# 11 thru 15
               2, 2, 2, 1, 1,	# 16 thru 20
               1, 0, 0, 0, 0)	# 21 thru 25; otherwise the filter is 0
  # High pass filters are numbered 0 thru 2 with cutoff 12.5 MHz, 4.2 MHz, short
  hpfnum = (2, 2, 2, 2, 2, 1,	# frequency 0 thru 5 MHz
               1, 1, 1, 1, 1,	# 6 thru 10
               1, 1, 0, 0, 0,	# 11 thru 15
               0, 0, 0, 0, 0,	# 16 thru 20
               0, 0, 0, 0, 0)	# 21 thru 25; otherwise the filter is 0
  def __init__(self, app, conf):
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.preamp = 0
    self.have_data = None
    self.want_data = b'\x00\x00'
    self.old_tx_freq = 0
    self.timer = 0
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.setblocking(0)
    self.socket.connect(self.address)
  def ChangeBand(self, band):
    pass
  def DefaultPreamp(self, freq):
    preamp = 0
    if self.conf.use_rx_udp:
      btn = self.application.BtnRfGain
      if btn:
        if freq < 5000000:
          btn.SetLabel('RF 0 dB', False)
        elif freq < 13000000:
          btn.SetLabel('RF 0 dB', False)
        else:
          btn.SetLabel('RF +16', False)
          preamp = 1
    return preamp
  def OnButtonRfGain(self, event):
    btn = event.GetEventObject()
    n = btn.index
    if n == 0:		# 0dB
      self.preamp = 0
      self.application.Hardware.rf_gain = 0
    elif n == 1:	# +16
      self.preamp = 1
      self.application.Hardware.rf_gain = 16
    else:
      print ('Unknown RfGain')
      self.preamp = 0
      self.application.Hardware.rf_gain = 0
    if DEBUG: print ('Gain', self.preamp, self.application.Hardware.rf_gain)
    self.SetTxFreq(None)
  def SetPreamp(self, preamp):
    if preamp:
      self.preamp = 1
    else:
      self.preamp = 0
    if DEBUG: print ("Preamp", preamp)
    self.SetTxFreq(None)
  def SetTxFreq(self, tx_freq):
    if tx_freq is None:
      tx_freq = self.old_tx_freq
    elif abs(self.old_tx_freq - tx_freq) < 100000:
      return	# Ignore small tuning changes
    if abs(self.old_tx_freq - tx_freq) > 1000000:
      self.preamp = self.DefaultPreamp(tx_freq)
    self.old_tx_freq = tx_freq
    try:		# Look up low pass filter number based on MHz
      lpf = self.lpfnum[tx_freq // 1000000]
    except IndexError:
      lpf = 0
    try:		# Look up high pass filter number based on MHz
      hpf = self.hpfnum[tx_freq // 1000000]
    except IndexError:
      hpf = 0
    if DEBUG: print ("V2 filter LPF %d HPF %d" % (lpf, hpf))
    lpf = 1 << lpf
    hpf = 1 << hpf
    if self.preamp:
      hpf |= 0b10000000
    self.want_data =  bytearray((lpf, hpf))
    self.timer = 0
  def HeartBeat(self):
    global gatewayTime
    if not self.socket:
      return
    try:	# The V2 filter box echoes its commands
      self.have_data = self.socket.recv(50)
    except socket.error:
      pass
    except socket.timeout:
      pass
    if self.have_data != self.want_data and time.time() - gatewayTime > gatewayLimit:
      gatewayTime = time.time()
      if self.timer <= 10:
        self.timer += 1
        if self.timer == 10:
          print ('V2 filter box error')
      try:
        self.socket.send(self.want_data)
        #print ('V2 filter box send data 0x%X 0x%X' % (ord(self.want_data[0]), ord(self.want_data[1])))
      except socket.error:
        pass
      except socket.timeout:
        pass

class AntennaControl:	# Control my KI8BV dipole
  AntCtrlAddress = ('192.168.1.194', 0x3A00 + 33)
  def __init__(self, app, conf):
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.setblocking(0)
    self.socket.connect(self.AntCtrlAddress)
    self.have_data = None
    self.want_data = b'\00'
    self.timer = 0
  def SetTxFreq(self, tx_freq):
    self.timer = 0
    if tx_freq < 19000000:
      self.want_data = b'\03'
    elif tx_freq < 22000000:
      self.want_data = b'\02'
    elif tx_freq < 26500000:
      self.want_data = b'\01'
    else:
      self.want_data = b'\00'
  def HeartBeat(self):
    global gatewayTime
    try:	# The antenna control box echoes its commands
      self.have_data = self.socket.recv(50)
    except socket.error:
      pass
    except socket.timeout:
      pass
    if self.have_data != self.want_data and time.time() - gatewayTime > gatewayLimit:
      gatewayTime = time.time()
      self.timer += 1
      if self.timer == 10:
        print ('Antenna control error')
        self.timer = 0
      try:
        self.socket.send(self.want_data)
        #print ("Change dipole to ord %d" % ord(self.want_data))
      except socket.error:
        pass
      except socket.timeout:
        pass

class AntennaTuner:     # Control my homebrew antenna tuner and my KI8BV dipole
  address = ('192.168.1.194', 0x3A00 + 47)
  def __init__(self, app, conf):
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.socket = None
    self.have_data = b'\xFF\xFF\xFF'
    self.tx_freq = 0
    self.tune_freq = 0		# Frequency we last tuned
    self.timer = 0
    self.set_L = 0
    self.set_C = 0
    self.set_HighZ = 0
    self.antnum = 0				# Antenna number 0 or 1
    self.dipole2 = AntennaControl(app, conf)	# Control the KI8BV dipole
    if False and conf.use_rx_udp == 10:		# Hermes UDP protocol for Hermes-Lite2
      path = 'TunerLCZ_HL2.txt'
    else:
      path = 'TunerLCZ.txt'
    if sys.platform == "win32":
      path = 'C:/pub/' + path
    else:
      path = '/home/jim/pub/' + path
    fp = open(path, "r")
    lines = fp.readlines()
    fp.close()
    self.TunerLCZ = [(0, 0, 0, 0), (999111000, 0, 0, 0)]	# Add dummy first and last entry.
    for line in lines:
      freq, antL, antC, hilo = line.split()
      freq = int(freq)
      antL = int(antL)
      antC = int(antC)
      hilo = int(hilo)
      self.TunerLCZ.append((freq, antL, antC, hilo))
    self.TunerLCZ.sort()
    self.WantData()
  def WantData(self):
    if self.set_HighZ:
      flags = 0x00
    else:
      flags = 0x08
    if self.antnum:
      flags |= 0x04
    self.want_data = bytearray((self.set_C, self.set_L, flags))
  def open(self):
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.setblocking(0)
    self.socket.connect(self.address)
  def close(self):
    pass
  def OnSpot(self, level):
    # level is -1 for Spot button Off; else the Spot level 0 to 1000.
    pass
  def ChangeBand(self, band):
    pass
  def FindLCZ(self, tx_freq):
    i1 = 0
    i2 = len(self.TunerLCZ) - 1
    while 1:	# binary partition
      i = (i1 + i2) // 2
      if self.TunerLCZ[i][0] < tx_freq:
        i1 = i
      else:
        i2 = i
      if i2 - i1 <= 1:
        break
    # The correct setting is between i1 and i2.
    # Choose the index that is closest in frequency.
    delta1 = tx_freq - self.TunerLCZ[i1][0]
    delta2 = self.TunerLCZ[i2][0] - tx_freq
    if i1 == 0:				# below the first frequency
      index = i1
    elif i2 == len(self.TunerLCZ) - 1:	# above the last frequency
      index = i2
    elif delta2 < delta1:	# i2 is closer
      index = i2
    else:			# i1 is closer
      index = i1
    if DEBUG: print ("AntennaTuner FindLCZ", tx_freq, i1, i2, len(self.TunerLCZ), self.TunerLCZ[index])
    return self.TunerLCZ[index]
  def SetTxFreq(self, tx_freq, no_tune=False):
    #if DEBUG: print("AntennaTuner SetTxFreq", tx_freq)
    self.tx_freq = tx_freq
    if tx_freq is None:
      return
    if tx_freq < 17000000:
      if DEBUG and self.antnum == 1: print ("antnum 0")
      self.antnum = 0
    else:
      if DEBUG and self.antnum == 0: print ("antnum 1")
      self.antnum = 1
    if self.antnum == 1:
      self.dipole2.SetTxFreq(tx_freq)
    if not self.socket:
      return
    if no_tune:		# Change dipole but don't tune the antenna tuner
      return
    if abs(self.tune_freq - tx_freq) < 5000:		# ignore small changes
      return
    self.tune_freq = tx_freq
    f, newL, newC, newH = self.FindLCZ(tx_freq)
    if newH != self.set_HighZ or self.set_C != newC or self.set_L != newL:
      self.set_HighZ = newH
      self.set_C = newC
      self.set_L = newL
      self.WantData()
  def HeartBeat(self):
    global gatewayTime
    if not self.socket:
      return
    try:	# The tuner echoes its command
      self.have_data = data = self.socket.recv(50)
    except socket.error:
      data = ''
    except socket.timeout:
      data = ''
    if self.have_data != self.want_data and time.time() - gatewayTime > gatewayLimit:
      gatewayTime = time.time()
      self.Send()
      self.timer += 1
      if self.timer == 10:
        print ('Antenna tuner error')
        self.timer = 0
    else:
      self.timer = 0
    self.dipole2.HeartBeat()
  def Send(self):
    try:
      self.socket.send(self.want_data)
    except socket.error:
      pass
    except socket.timeout:
      pass

class StationControlGUI(wx.Frame):    # Display a stand-alone control window for my antenna tuner
  def __init__(self, frame, hware, app, conf):
    wx.Frame.__init__(self, parent=frame, title="Station Control",
       style=wx.CAPTION|wx.CLOSE_BOX)
    self.hware = hware
    self.application = app
    self.conf = conf
    self.data_saving = False
    self.tx_freq = 0
    if sys.platform == "win32":
      self.filename = 'C:/pub/TunerLCZ.tmp'
    else:
      self.filename = '/home/jim/pub/TunerLCZ.tmp'
    try:
      fp = open(self.filename, "r")
    except FileNotFoundError:
      lines = ()
    else:
      lines = fp.readlines()
      fp.close()
    self.TunerLCZ = [(0, 0, 0, 0), (999111000, 0, 0, 0)]	# Add dummy first and last entry.
    for line in lines:
      freq, antL, antC, hilo = line.split()
      freq = int(freq)
      antL = int(antL)
      antC = int(antC)
      hilo = int(hilo)
      self.TunerLCZ.append((freq, antL, antC, hilo))
    self.TunerLCZ.sort()
    self.SetBackgroundColour('light steel blue')
    self.Bind(wx.EVT_CHAR_HOOK, self.OnKeyDown)
    sizer = wx.GridBagSizer(hgap=5, vgap=3)
    font = wx.Font(12, wx.FONTFAMILY_SWISS, wx.NORMAL, wx.FONTWEIGHT_NORMAL, False, conf.quisk_typeface)
    row = 0
    szr = wx.BoxSizer(wx.HORIZONTAL)
    self.bands = []
    for band in ('160', '80', '60', '40', '30', '20', '17', '15', '12', '10'):
      b = wx.lib.buttons.GenToggleButton(self, -1, band)
      b.SetUseFocusIndicator(False)
      b.SetBezelWidth(4)
      self.bands.append(b)
      szr.Add(b)
      self.Bind(wx.EVT_BUTTON, self.OnBtnBand, b)
    sizer.Add(szr, pos=(row, 0), span=(1, 3), flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT|wx.RIGHT|wx.TOP, border=20)
    row = 1
    b = wx.StaticText(self, -1, "    Tuner Adjust L   ", style=wx.ALIGN_CENTER)
    b.SetFont(font)
    sizer.Add(b, pos=(row, 0), flag=wx.ALIGN_CENTER_VERTICAL)
    b = self.sliderL = wx.Slider(self, -1, 0, 0, 255, size=(700, -1), style=wx.SL_HORIZONTAL|wx.SL_LABELS)
    b.Bind(wx.EVT_SCROLL, self.OnSliderL)
    sizer.Add(b, pos=(row,1))
    b = self.btnHiLo = wx.lib.buttons.GenToggleButton(self, -1, "HiLo")
    b.SetUseFocusIndicator(False)
    b.SetBezelWidth(4)
    b.SetFont(font)
    self.Bind(wx.EVT_BUTTON, self.OnButtonHiLo, b)
    sizer.Add(b, pos=(row,2), flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT|wx.RIGHT, border=20)
    row = 2
    b = wx.StaticText(self, -1, "    Tuner Adjust C   ")
    b.SetFont(font)
    sizer.Add(b, pos=(row, 0), flag=wx.ALIGN_CENTER_VERTICAL)
    b = self.sliderC = wx.Slider(self, -1, 0, 0, 255, size=(700, -1), style=wx.SL_HORIZONTAL|wx.SL_LABELS)
    b.Bind(wx.EVT_SCROLL, self.OnSliderC)
    sizer.Add(b, pos=(row,1), flag=wx.ALIGN_CENTER_VERTICAL)
    b = wx.lib.buttons.GenButton(self, -1, "Save")
    b.SetUseFocusIndicator(False)
    b.SetBezelWidth(4)
    b.SetFont(font)
    self.Bind(wx.EVT_BUTTON, self.OnButtonSave, b)
    sizer.Add(b, pos=(row,2), flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT|wx.RIGHT, border=20)
    row = 3
    szr = wx.BoxSizer(wx.HORIZONTAL)
    szr.AddSpacer(20)
    b = wx.lib.buttons.GenToggleButton(self, -1, "Key Down")
    b.SetFont(font)
    b.SetUseFocusIndicator(False)
    b.SetBezelWidth(4)
    self.Bind(wx.EVT_BUTTON, self.OnBtnKey, b)
    szr.Add(b)
    szr.AddSpacer(10)
    b = wx.lib.buttons.GenToggleButton(self, -1, "FilterV2 Preamp")
    b.SetFont(font)
    b.SetUseFocusIndicator(False)
    b.SetBezelWidth(4)
    if conf.use_rx_udp == 10:		# Hermes UDP protocol for Hermes-Lite2
      b.Enable(False)
    self.Bind(wx.EVT_BUTTON, self.OnBtnV2Preamp, b)
    szr.Add(b)
    b = wx.StaticText(self, -1, "     Freq MHz ")
    b.SetFont(font)
    szr.Add(b, flag=wx.ALIGN_CENTER_VERTICAL)
    szr.AddSpacer(10)
    self.freq_entry = wx.TextCtrl(self, value='0000.000', style=wx.TE_PROCESS_ENTER)
    self.freq_entry.SetFont(font)
    self.Bind(wx.EVT_TEXT_ENTER, self.OnFreqEntry, self.freq_entry)
    szr.Add(self.freq_entry)
    szr.AddSpacer(10)
    b = wx.lib.buttons.GenButton(self, -1, "Search Freq")
    b.SetUseFocusIndicator(False)
    b.SetBezelWidth(4)
    b.SetFont(font)
    self.Bind(wx.EVT_BUTTON, self.OnButtonSearch, b)
    szr.Add(b)
    sizer.Add(szr, pos=(row, 0), span=(1, 3))
    if frame is None:
      self.SetSizerAndFit(sizer)
    else:
      self.SetSizer(sizer)
      self.Fit()
    w, h = self.GetSize().Get()
    self.SetSize((w + 20, h + 50))
    self.Bind(wx.EVT_CLOSE, self.OnBtnClose)
    if frame is None:	# Start a HeartBeat if the frame does not provide one
      self.timer = wx.Timer(self)
      self.Bind(wx.EVT_TIMER, self.HeartBeat)
      self.timer.Start(100)
  def SetTxFreq(self, tx_freq):
    #print("StationControlGUI SetTxFreq", tx_freq)
    self.tx_freq = tx_freq
    index, tup = self.FindIndexTup(tx_freq)
    self.sliderL.SetValue(tup[1])
    self.sliderC.SetValue(tup[2])
    self.btnHiLo.SetValue(tup[3])
    anttuner = self.hware.anttuner
    fff, anttuner.set_L, anttuner.set_C, anttuner.set_HighZ = tup
    if tx_freq < 17000000:
      anttuner.antnum = 0
    else:
      anttuner.antnum = 1
    if anttuner.antnum == 1:
      anttuner.dipole2.SetTxFreq(tx_freq)
    anttuner.WantData()
  def FindIndexTup(self, tx_freq):
    i1 = 0
    i2 = len(self.TunerLCZ) - 1
    while 1:	# binary partition
      i = (i1 + i2) // 2
      if self.TunerLCZ[i][0] < tx_freq:
        i1 = i
      else:
        i2 = i
      if i2 - i1 <= 1:
        break
    # The correct setting is between i1 and i2.
    # Choose the index that is closest in frequency.
    delta1 = tx_freq - self.TunerLCZ[i1][0]
    delta2 = self.TunerLCZ[i2][0] - tx_freq
    if i1 == 0:				# below the first frequency
      index = i1
    elif i2 == len(self.TunerLCZ) - 1:	# above the last frequency
      index = i2
    elif delta2 < delta1:	# i2 is closer
      index = i2
    else:			# i1 is closer
      index = i1
    if DEBUG: print ("StatuinControlGUI FindLCZ", tx_freq, i1, i2, len(self.TunerLCZ), index, self.TunerLCZ[index])
    return index, self.TunerLCZ[index]
  def OnBtnClose(self, event):
    self.hware.anttuner.close()
    self.hware.controlbox.close()
    self.Destroy()
  def HeartBeat(self, event):
    self.hware.anttuner.HeartBeat()
    self.hware.controlbox.HeartBeat()
    if self.hware.v2filter:
      self.hware.v2filter.HeartBeat()
  def Open(self):	# Initialize the hardware
    self.bands[0].SetToggle(True)	# 160 meters
    self.hware.anttuner.open()
    if self.hware.v2filter:
      self.hware.v2filter.SetTxFreq(1900000)	# Start 160 meters
  def OnKeyDown(self, event):
    key = event.GetRawKeyCode()
    if key == 76:	# L
      self.sliderL.SetValue(self.sliderL.GetValue() + 1)
      self.OnSliderL()
    elif key == 108:	# l
      self.sliderL.SetValue(self.sliderL.GetValue() - 1)
      self.OnSliderL()
    elif key == 67:	# C
      self.sliderC.SetValue(self.sliderC.GetValue() + 1)
      self.OnSliderC()
    elif key == 99:	# c
      self.sliderC.SetValue(self.sliderC.GetValue() - 1)
      self.OnSliderC()
    else:
      event.Skip()
  def OnBtnBand(self, event):
    btn = event.GetEventObject()
    for band in self.bands:
      if band == btn:
        band.SetToggle(True)
      else:
        band.SetToggle(False)
    band = btn.GetLabel()
    f1, f2 = self.conf.BandEdge[band]
    f = (f1 + f2) // 2
    f = ((f + 500) // 1000) * 1000
    if self.hware.v2filter:
      self.hware.v2filter.SetTxFreq(f)
    self.hware.anttuner.SetTxFreq(f, no_tune=True)
    self.freq_entry.ChangeValue("%.3f" % (f * 1E-6))
    self.SetTxFreq(f)
  def OnButtonHiLo(self, event):
    anttuner = self.hware.anttuner
    if self.btnHiLo.GetValue():
      anttuner.set_HighZ = 1
    else:
      anttuner.set_HighZ = 0
    anttuner.WantData()
  def OnFreqEntry(self, event=None):
    freq = self.freq_entry.GetValue()
    freq = float(freq) * 1E6
    freq = int(freq + 0.1)
    self.tx_freq = freq
  def OnButtonSearch(self, event):
    freq = self.freq_entry.GetValue()
    freq = float(freq) * 1E6
    freq = int(freq + 0.1)
    self.tx_freq = freq
    self.SetTxFreq(self.tx_freq)
  def OnButtonSave(self, event):
    freq = self.freq_entry.GetValue()
    freq = float(freq) * 1E6
    freq = int(freq + 0.1)
    self.tx_freq = freq
    L = self.sliderL.GetValue()
    C = self.sliderC.GetValue()
    if self.btnHiLo.GetValue():
      hilo = 1
    else:
      hilo = 0
    tup = (self.tx_freq, L, C, hilo)
    index, t = self.FindIndexTup(self.tx_freq)
    if self.tx_freq == self.TunerLCZ[index][0]:
      self.TunerLCZ[index] = tup
    else:
      self.TunerLCZ.append(tup)
    self.TunerLCZ.sort()
    fp = open(self.filename, 'w')
    for tup in self.TunerLCZ[1:-1]:	# Throw away dummy entries
      fp.write("%12d %2d %2d %d\n" % tup)
    fp.close()
  def OnSliderL(self, event=None):
    self.hware.anttuner.set_L = self.sliderL.GetValue()
    self.hware.anttuner.WantData()
  def OnSliderC(self, event=None):
    self.hware.anttuner.set_C = self.sliderC.GetValue()
    self.hware.anttuner.WantData()
  def OnBtnV2Preamp(self, event):
    if self.hware.v2filter:
      self.hware.v2filter.SetPreamp(event.GetEventObject().GetValue())
  def OnBtnKey(self, event):
    self.hware.controlbox.SetKeyDown(event.GetEventObject().GetValue())

class AT200PC:		# Control an AT-200PC autotuner made by LDG
  def __init__(self, app, conf):
    import serial
    self.application = app	# Application instance (to provide attributes)
    self.conf = conf		# Config file module
    self.serial = None
    self.rx_state = 0
    self.is_standby = None
    self.tx_freq = 0
    self.old_tx_freq = 0
    self.set_L = -9
    self.set_C = -9
    self.set_HiLoZ = -9
    self.tuning_F1 = 0
    self.tuning_F2 = 0
    self.tuning_diff = 0
    self.param1 = [None] * 20	# Parameters returned by the AT-200PC
    self.param2 = [None] * 20
    self.param1[5] = self.param2[5] = self.param2[6] = 0		# power and SWR
    self.param1[7] = self.param2[7] = 1		# Frequency
    self.param1[1] = self.param1[2] = 0		# Inductor, Capacitor
    self.req_swr = 50			# Requested SWR: 50 thru 56 for 1.1, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0
    self.live_update = 0		# Request live update 1 or 0
    self.antenna = 2			# Select antenna 1 or 2
    self.standby = 0			# Set standby mode 1 or 0
    self.timer = 0
    self.error = "AT-200PC serial port is not open"
    self.TunerLC_change = False
    if sys.platform == "win32":
      self.TunerLC_fname = 'C:/pub/TunerLC.txt'
    else:
      self.TunerLC_fname = '/home/jim/pub/TunerLC.txt'
  def UpdateSwr(self):
    if not self.application.bottom_widgets:
      return
    if self.error:
      self.application.bottom_widgets.UpdateText(self.error)
    else:
      power = (self.param1[5] * 256 + self.param2[5]) / 100.0
      swr = self.param2[6]	# swr code = 256 * p**2
      if power >= 2.0:
        freq = self.param1[7] * 256 + self.param2[7]
        freq = 20480000.0 / freq
        ftext = "Tx freq"
        swr = math.sqrt(swr / 256.0)
        swr = (1.0 + swr) / (1.0 - swr)
        if swr > 99.9:
          swr = 99.9
      else:
        freq = self.tuning_diff / 1000.0
        ftext = "Tune delta"
        swr = 0.0
      if self.param1[3] == 0:	# HiLoZ relay value
        t = "Zh"		# High
      else:
        t = "Zl"		# Low
      text = "Watts %.0f   SWR %.1f  %s Ind %d Cap %d   %s %.0f kHz" % (
         power, swr,  t, self.param1[1], self.param1[2], ftext, freq)
      self.application.bottom_widgets.UpdateText(text)
  def HeartBeat(self):
    if not self.serial:
      self.UpdateSwr()
      return
    self.Read()			# Receive from the AT-200PC
    # Call main application with new SWR data
    self.UpdateSwr()
    if self.error:		# Send a couple parameters, see if we get a response
      if self.req_swr - 50 != self.param1[16]:
        self.Write(chr(self.req_swr))	# Send threshold SWR
      elif self.param1[17] != 0:
        self.Write(chr(59))			# Turn off AutoTune
      else:
        self.error = ''
      return
    if self.param1[4] != self.antenna - 1:		# Check correct antenna
      self.Write(chr(9 + self.antenna))
    elif self.is_standby != self.standby:		# Check standby state
      self.Write(chr(45 - self.standby))
    elif self.param1[19] != self.live_update:	# Check live update state
      self.Write(chr(64 - self.live_update))
    elif self.set_L >= 0 and self.set_HiLoZ >= 0 and (	# Check L and Hi/Lo relay
         self.param1[1] != self.set_L or self.param1[3] != self.set_HiLoZ):
      if self.set_HiLoZ:
        self.Write(chr(65) + chr(self.set_L + 128))
      else:
        self.Write(chr(65) + chr(self.set_L))
    elif self.param1[2] != self.set_C and self.set_C >= 0:	# Set C
      self.Write(chr(66) + chr(self.set_C))
    elif self.live_update:	# If our window shows, request an update
      self.timer += 1
      if self.timer > 20:
        self.timer = 0
        self.Write(chr(40))		# Request ALLUPDATE
  def Write(self, s):		# Write a command string to the AT-200PC
    if DEBUG:
      print ('Send', ord(s[0]))
    if self.serial:
      self.serial.setRTS(1)	# Wake up the AT-200PC
      time.sleep(0.003)		# Wait 3 milliseconds
      self.serial.write(s)
      self.serial.setRTS(0)
  def Read(self):	# Receive characters from the AT-200PC
    chars = self.serial.read(1024)
    for ch in chars:
      if self.rx_state == 0:	# Read first of 4 characters; must be decimal 165
        if ord(ch) == 165:
          self.rx_state = 1
      elif self.rx_state == 1:	# Read second byte
        self.rx_state = 2
        self.rx_byte1 = ord(ch)
      elif self.rx_state == 2:	# Read third byte
        self.rx_state = 3
        self.rx_byte2 = ord(ch)
      elif self.rx_state == 3:	# Read fourth byte
        self.rx_state = 0
        byte3 = ord(ch)
        byte1 = self.rx_byte1
        byte2 = self.rx_byte2
        if DEBUG:
          print ('Received', byte1, byte2, byte3)
        if byte1 > 19:			# Impossible command value
          continue
        if byte1 == 1 and self.set_L < 0:	# reported inductor value
          self.set_L = byte2
        if byte1 == 2 and self.set_C < 0:	# reported capacitor value
          self.set_C = byte2
        if byte1 == 3 and self.set_HiLoZ < 0:	# reported Hi/Lo relay
          self.set_HiLoZ = byte2
        if byte1 == 13:				# Start standby
          self.is_standby = 1
        elif byte1 == 14:			# Start active
          self.is_standby = 0
        self.param1[byte1] = byte2
        self.param2[byte1] = byte3
  def OpenPort(self):
    if sys.platform == "win32":
      tty_list = ("COM7", "COM8", "COM10", "COM11")
    else:
      tty_list = ("/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2")
    for tty_name in tty_list:
      try:
        port = serial.Serial(port=tty_name, baudrate=9600, timeout=0)
      except:
        #traceback.print_exc()
        pass
      else:
        port.setRTS(0)
        time.sleep(0.1)
        for i in range(3):
          port.setRTS(1)
          time.sleep(0.003)		# Wait 3 milliseconds
          port.write(chr(41))
          port.setRTS(0)
          time.sleep(0.1)
          chars = port.read(1024)
          #print 'Got', tty_name, len(chars), repr(chars)
          if chars == '?\r\n':		# Wrong port
            break
          if "\xA5\x0B\x01\x20" in chars:
            self.serial = port
            break
        if self.serial:
          break
        else:
          port.close()
  def open(self):
    self.OpenPort()
    if self.serial:
      self.error = "Waiting for AT200PC"
    # TunerLC is a list of (freq, L, C).  Use -L for Low Z, +L for High Z.
    # The first and last entry must have frequency 0 and 99999999.
    self.TunerLC = []
    fp = open(self.TunerLC_fname, 'r')
    for line in fp:
      line = line.split()
      f = int(line[0])
      l = int(line[1])
      c = int(line[2])
      self.TunerLC.append((f, l, c))
    fp.close()
  def close(self):
    if self.serial:
      self.serial.close()
      self.serial = None
    if self.TunerLC_change:
      fp = open(self.TunerLC_fname, 'w')
      for f, l, c in  self.TunerLC:
        fp.write("%9d %4d %4d\n" % (f, l, c))
      fp.close()
  def xxReqSetFreq(self, tx_freq):
    # Set relays for this frequency.  The frequency must exist in the tuner.
    if self.serial and not self.standby and tx_freq > 1500000:
      ticks = int(20480.0 / tx_freq * 1e6 + 0.5)
      self.Write(chr(67) + chr((ticks & 0xFF00) >> 8) + chr(ticks & 0xFF))
  def SetTxFreq(self, tx_freq):
    if tx_freq is None:
      self.set_C = 0
      self.set_L = 0
      self.set_HiLoZ = 0
      return
    self.tx_freq = tx_freq
    if abs(self.old_tx_freq - tx_freq) < 20000:
      d1 = tx_freq - self.tuning_F1
      d2 = tx_freq - self.tuning_F2
      if abs(d1) <= abs(d2):
        self.tuning_diff = d1
      else:
        self.tuning_diff = d2
      return	# Ignore small tuning changes
    self.old_tx_freq = tx_freq
    i1 = 0
    i2 = len(self.TunerLC) - 1
    while 1:	# binary partition
      i = (i1 + i2) // 2
      if self.TunerLC[i][0] < tx_freq:
        i1 = i
      else:
        i2 = i
      if i2 - i1 <= 1:
        break
    # The correct setting is between i1 and i2; interpolate
    F1 = self.TunerLC[i1][0]
    F2 = self.TunerLC[i2][0]
    L1 = self.TunerLC[i1][1]
    L2 = self.TunerLC[i2][1]
    C1 = self.TunerLC[i1][2]
    C2 = self.TunerLC[i2][2]
    frac = (float(tx_freq) - F1) / (F2 - F1)
    C = C1 + (C2 - C1) * frac
    self.set_C = int(C + 0.5)
    L = L1 + (L2 - L1) * frac
    if L < 0:
      L = -L
      self.set_HiLoZ = 1
    else:
      self.set_HiLoZ = 0
    self.set_L = int(L + 0.5)
    # Report the frequency difference
    self.tuning_F1 = F1
    self.tuning_F2 = F2
    d1 = tx_freq - F1
    d2 = tx_freq - F2
    if abs(d1) <= abs(d2):
      self.tuning_diff = d1
    else:
      self.tuning_diff = d2
  def ChangeBand(self, band):
    pass ##self.ReqSetFreq(self.tx_freq)
  def OnSpot(self, level):
    # level is -1 for Spot button Off; else the Spot level 0 to 1000.
    if self.serial:
      if level < 0:
        self.live_update = 0
      elif not self.live_update:
        self.live_update = 1
        self.timer = 999
  def OnAntTuner(self, text):	# One of the tuner buttons was pressed
    if self.serial:
      if text == 'Tune':
        if not self.standby:
          #self.Write(chr(5))		# Request memory tune
          self.Write(chr(6))		# Request full tune
          self.set_C = -9
          self.set_L = -9
          self.set_HiLoZ = -9
      elif text == 'Save':
        self.Write(chr(46))
        if self.set_HiLoZ == 0:		# High Z
          L = self.set_L
        else:						# Low Z
          L = -self.set_L
        for i in range(len(self.TunerLC)):	# Record new freq and L/C
          if abs(self.TunerLC[i][0] - self.tx_freq) < 1000:
            self.TunerLC[i] = (self.tx_freq, L, self.set_C)
            break
        else:
          self.TunerLC.append((self.tx_freq, L, self.set_C))
          self.TunerLC.sort()
          self.TunerLC_change = True
      elif text == 'L+':
        self.set_L += 1
      elif text == 'L-':
        self.set_L -= 1
      elif text == 'C+':
        self.set_C += 1
      elif text == 'C-':
        self.set_C -= 1

class App(wx.App):
  def OnInit(self):
    if sys.path[0] != "'.'":		# Make sure the current working directory is on path
      sys.path.insert(0, '.')
    import quisk_conf_defaults as conf
    import quisk_hardware_model as hardware
    self.bottom_widgets = None
    hardware.anttuner = AntennaTuner(self, conf)	# Control the antenna tuner
    hardware.v2filter = FilterBoxV2(self, conf)	# Control V2 filter box
    hardware.controlbox = ControlBox(self, conf)	# Control my Station Control Box
    self.main_frame = frame = StationControlGUI (None, hardware, self, conf)
    frame.Open()
    frame.Show()
    return True

if __name__ == '__main__':
  App().MainLoop()
