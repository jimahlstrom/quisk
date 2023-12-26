# Please do not change this hardware control module.
# It provides support for the SDR-IQ by RfSpace.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

#import _quisk as QS
import os,sys
#sys.path.append('./afedri')
try:
  import afedrinet_io as AF
  from sdr_control import *
except:
  from afedrinet import afedrinet_io as AF
  from afedrinet.sdr_control import *
from ctypes import *
os.environ['PATH'] = os.path.dirname(__file__) + ';' + os.environ['PATH']
#from quisk import App as parent
from quisk_hardware_model import Hardware as BaseHardware
#from quisk import *

class Hardware(BaseHardware):
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.local_tune = 2
    self.index = 0
    self.clock = 80000000
    self.old_LO_freq = 0
    self.rf_gain_labels = ('RF -10','RF -7','RF -4','RF -1','+2','+5','+8','+11','+14','+17','+20','+23','+26','+29','+32','+35')
    self.conf = conf
    self.plugin = Control(self.conf.rx_udp_ip, self.conf.rx_udp_port)
    self.app = app
    self.decimations = []		# supported decimation rates
    for dec in (53333, 96000, 133333, 185185, 192000, 370370, 740740, 1333333):
            self.decimations.append(dec)
  def open(self):
    self.plugin.OpenHW()		# Return a config message
    RF_Gain_idx = int((10 + self.conf.default_rf_gain) / 3)
    if not 0 <= RF_Gain_idx < len(self.rf_gain_labels):
      RF_Gain_idx = 0
    self.plugin.SetAttenuator(RF_Gain_idx)
    self.app.BtnRfGain.SetIndex(RF_Gain_idx)
    #print ("RF Gain %i" % self.conf.default_rf_gain)
    return AF.open_samples(self.conf.rx_udp_ip, self.conf.rx_udp_port)
  def close(self):
    self.plugin.CloseHW()
  def OnButtonRfGain(self, event):
    btn = event.GetEventObject()
    n = btn.index
    if n > -1 or n < 16 :
            self.plugin.SetAttenuator(n)
    else:
            print ('Unknown RfGain')
  def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
    self.local_tune = tune
    if vfo:
            self.plugin.SetHWLO(vfo)
    return tune, vfo
  def ReturnFrequency(self):	# Return the current tuning and VFO frequency
    return (None, None)		# Return LO frequency
  def GetFirmwareVersion(self):
    return 226
  def HeartBeat(self):
#    self.PrintStatus('Start', 'AFEDRI')
     return
  def VarDecimGetChoices(self):	# Return a list/tuple of strings for the decimation control.
    l = []			# a list of sample rates
    for dec in self.decimations:
      l.append(str( dec ))
    return l
  def VarDecimGetLabel(self):		# return a text label for the control
    return "Sample rate sps"
  def VarDecimGetIndex(self):		# return the current index
    return self.index
  def VarDecimSet(self, index=None):		# set decimation, return sample rate
    if index is None:		# initial call to set decimation before the call to open()
      rate = self.application.vardecim_set		# May be None or from different hardware
      try:
        dec = rate #int(float(self.conf.rx_udp_clock / rate + 0.5))
        self.index = self.decimations.index(dec)
      except:
        try:
          self.index = self.decimations.index(self.conf.sample_rate)
        except:
          self.index = 0
    else:
      self.index = index
    dec = self.decimations[self.index]
    self.plugin.SetHWSR(dec)		# Return a config message
    return dec


 
