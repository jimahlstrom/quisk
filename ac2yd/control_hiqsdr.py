# This provides access to a remote radio.  See ac3yd/remote_common.py and .pdf files for documentation.

from ac2yd.control_common import ControlCommon

class Hardware(ControlCommon):
  def __init__(self, app, conf):
    ControlCommon.__init__(self, app, conf)
    self.index = 0
    self.rx_udp_clock = 122880000
    self.decimations = []		# supported decimation rates
    for dec in (40, 20, 10, 8, 5, 4, 2):
      self.decimations.append(dec * 64)
    self.decimations.append(80)
    self.decimations.append(64)
  def RadioInit(self):	# Send initial parameters not covered by CommonInit()
    pass
  def VarDecimGetChoices(self):		# return text labels for the control
    clock = self.rx_udp_clock
    l = []			# a list of sample rates
    for dec in self.decimations:
      l.append(str(int(float(clock) / dec / 1e3 + 0.5)))
    return l
  def VarDecimGetLabel(self):		# return a text label for the control
    return "Sample rate ksps"
  def VarDecimGetIndex(self):		# return the current index
    return self.index
  def VarDecimSet(self, index=None):		# set decimation, return sample rate
    if index is None:		# initial call to set decimation before the call to open()
      rate = self.application.vardecim_set		# May be None or from different hardware
      try:
        dec = int(float(self.rx_udp_clock // rate + 0.5))
        self.index = self.decimations.index(dec)
      except:
        self.index = 0
    else:
      self.index = index
    dec = self.decimations[self.index]
    return int(float(self.rx_udp_clock) / dec + 0.5)
  def VarDecimRange(self):
    return (48000, 960000)
