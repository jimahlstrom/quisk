# This is the hardware control file for my shack.
# It is for the Hermes-Lite2 5 watt output which uses only the antenna tuner.
from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

from hermes.quisk_hardware import Hardware as BaseHw
from n2adr import station_hardware

class Hardware(BaseHw):
  def __init__(self, app, conf):
    BaseHw.__init__(self, app, conf)
    self.GUI = None
    self.vfo_frequency = 0		# current vfo frequency
    # Other hardware
    self.anttuner = station_hardware.AntennaTuner(app, conf)	# Control the antenna tuner
    self.controlbox = station_hardware.ControlBox(app, conf)	# Control my Station Control Box
    self.v2filter = station_hardware.FilterBoxV2(app, conf)	# Control V2 filter box
  def open(self):
    if False:
      from n2adr.station_hardware import StationControlGUI
      self.GUI = StationControlGUI(self.application.main_frame, self, self.application, self.conf)
      self.GUI.Show()
    self.anttuner.open()
    return BaseHw.open(self)
  def close(self):
    self.anttuner.close()
    self.controlbox.close()
    return BaseHw.close(self)
  def ChangeFilterFrequency(self, tx_freq):
    if tx_freq and tx_freq > 0:
      if self.GUI:
        self.GUI.SetTxFreq(tx_freq)
        self.GUI.freq_entry.ChangeValue("%.3f" % (tx_freq * 1E-6))
      else:
        self.anttuner.SetTxFreq(tx_freq)
        self.v2filter.SetTxFreq(tx_freq)
  def ChangeFrequency(self, tx_freq, vfo_freq, source='', band='', event=None):
    self.ChangeFilterFrequency(tx_freq)
    return BaseHw.ChangeFrequency(self, tx_freq, vfo_freq, source, band, event)
  def ChangeBand(self, band):
    # band is a string: "60", "40", "WWV", etc.
    ret = BaseHw.ChangeBand(self, band)
    self.anttuner.ChangeBand(band)
    #self.lpfilter.ChangeBand(band)
    #self.hpfilter.ChangeBand(band)
    self.v2filter.ChangeBand(band)
    self.CorrectSmeter()
    return ret
  def HeartBeat(self):	# Called at about 10 Hz by the main
    self.anttuner.HeartBeat()
    self.v2filter.HeartBeat()
    self.controlbox.HeartBeat()
    return BaseHw.HeartBeat(self)
  def OnSpot(self, level):
    # level is -1 for Spot button Off; else the Spot level 0 to 1000.
    self.anttuner.OnSpot(level)
    return BaseHw.OnSpot(self, level)
  def OnButtonRfGain(self, event):
    self.v2filter.OnButtonRfGain(event)
    self.CorrectSmeter()
  def CorrectSmeter(self):	# S-meter correction can change with band or RF gain
    return
    if self.band == '40':				# Basic S-meter correction by band
      self.correct_smeter = 20.5
    else:
      self.correct_smeter = 20.5
    #self.correct_smeter -= self.rf_gain / 6.0		# Correct S-meter for RF gain
    #self.application.waterfall.ChangeRfGain(self.rf_gain)	# Waterfall colors are constant
  def OnButtonPTT(self, event):
    self.controlbox.OnButtonPTT(event)
    return BaseHw.OnButtonPTT(self, event)
