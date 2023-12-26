# This provides access to a remote radio.  See ac2yd/remote_common.py and .pdf files for documentation.

from ac2yd.control_common import ControlCommon

class Hardware(ControlCommon):
  def __init__(self, app, conf):
    ControlCommon.__init__(self, app, conf)
    self.hermes_code_version = 40
    self.HL2_TEMP = ";;;"
    self.var_rates = ['48', '96', '192', '384']
    self.var_index = 0
    #app.bandscope_clock = conf.rx_udp_clock
  def ChangeLNA(self, value):
    pass
  def ChangeAGC(self, value):
    pass
  def HeartBeat(self):
    ControlCommon.HeartBeat(self)
    args = self.HL2_TEMP.split(';')
    widg = self.app.bottom_widgets
    if widg:
      widg.text_temperature.SetLabel(args[0])
      widg.text_pa_current.SetLabel(args[1])
      widg.text_fwd_power.SetLabel(args[2])
      widg.text_swr.SetLabel(args[3])
  def RadioInit(self):	# Send initial parameters not covered by CommonInit()
    idName = "RfLna"
    value = self.app.midiControls[idName][0].GetValue()
    self.RemoteCtlSend("%s;%d\n" % (idName, value))
  def VarDecimGetChoices(self):		# return text labels for the control
    return self.var_rates
  def VarDecimGetLabel(self):		# return a text label for the control
    return "Sample rate ksps"
  def VarDecimGetIndex(self):		# return the current index
    return self.var_index
  def VarDecimSet(self, index=None):		# set decimation, return sample rate
    if index is None:		# initial call to set rate before the call to open()
      rate = self.app.vardecim_set		# May be None or from different hardware
    else:
      rate = int(self.var_rates[index]) * 1000
    if rate == 48000:
      self.var_index = 0
    elif rate == 96000:
      self.var_index = 1
    elif rate == 192000:
      self.var_index = 2
    elif rate == 384000:
      self.var_index = 3
    else:
      self.var_index = 0
      rate = 48000
    return rate
  def VarDecimRange(self):
    return (48000, 384000)
