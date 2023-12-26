# This is the hardware file to support radios accessed by the PerseusSDR interface.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import socket, traceback, time, math
import _quisk as QS
try:
  from perseuspkg import perseus
except:
  #traceback.print_exc()
  perseus = None
  print ("Error: Perseus package not found.\n")

from quisk_hardware_model import Hardware as BaseHardware

DEBUG = 1

# Define the name of the hardware and the items on the hardware screen (see quisk_conf_defaults.py):
################ Receivers PerseusSDR, The PerseusSDR interface to multiple hardware SDRs.
## hardware_file_name		Hardware file path, rfile
# This is the file that contains the control logic for each radio.
#hardware_file_name = 'perseuspkg/quisk_hardware.py'

## widgets_file_name			Widget file path, rfile
# This optional file adds additional controls for the radio.
#widgets_file_name = 'perseuspkg/quisk_widgets.py'

class Hardware(BaseHardware):
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)

    self.rf_gain_labels = ('RF +0', 'RF -10', 'RF -20', 'RF -30')
    self.antenna_labels = ('Wide Band', 'Band Filter')

    self.vardecim_index = 0
    self.fVFO = 0.0	# Careful, this is a float
    if DEBUG: print ("__init__: %s" % conf)
    self.rates = [ 48000,   \
                   95000,   \
                   96000,   \
                   125000,  \
                   192000,  \
                   250000,  \
                   500000,  \
                   1000000, \
                   1600000, \
                   2000000  \
                   ]
    self.current_rate = 192000
    self.att = 0;
    self.wb = 0
    
    def __del__(self):
        # try to clear hardware
        if perseus:
            perseus.close()
            perseus.deinit()

  def get_hw (self):
      return perseus

  def pre_open(self):
    if DEBUG: print ("pre_open")
    pass

  def set_parameter(self, *args):
    pass

  def open(self):	# Called once to open the Hardware
      
    if not perseus:
      return "Perseus module not available"

    txt = perseus.open_device("perseus",2,3)
    if DEBUG: print ("perseus hardware: open")

    return txt

  def close(self):			# Called once to close the Hardware
    if DEBUG: print ("perseus hardware: close")
    if perseus:
      perseus.close_device(1)

  def ChangeGain(self, rxtx):	# rxtx is '_rx' or '_tx'
    if not perseus:
      return
    if DEBUG: print ("perseus hardware: ChangeGain", rxtx)
    pass

  def OnButtonRfGain(self, event):
    #btn = event.GetEventObject()
    n = event.GetEventObject().index
    self.att = n * -10
    if DEBUG: print ("perseus hardware: OnButtonRfGain: %d new attenuation: %d" % (n, self.att))
    perseus.set_attenuator (self.att)

  def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
    fVFO = float(vfo)
    if self.fVFO != fVFO:
      self.fVFO = fVFO
      perseus.set_frequency(fVFO)
    return tune, vfo


  def ReturnFrequency(self):
    # Return the current tuning and VFO frequency.  If neither have changed,
    # you can return (None, None).  This is called at about 10 Hz by the main.
    # return (tune, vfo)	# return changed frequencies
    return None, None		# frequencies have not changed

  def ReturnVfoFloat(self):
    # Return the accurate VFO frequency as a floating point number.
    # You can return None to indicate that the integer VFO frequency is valid.
    return self.fVFO

#  def OnBtnFDX(self, fdx):	# fdx is 0 or 1
#    pass
#
#  def OnButtonPTT(self, event):
#    pass
#
#  def OnSpot(self, level):
#    # level is -1 for Spot button Off; else the Spot level 0 to 1000.
#    pass
#
#  def ChangeMode(self, mode):		# Change the tx/rx mode
#    # mode is a string: "USB", "AM", etc.
#    pass
#
#  def ChangeBand(self, band):
#    pass
#
#  def HeartBeat(self):	# Called at about 10 Hz by the main
#    pass

  def ImmediateChange(self, name, value):
    if DEBUG: print ("perseus hardware: ImmediateChange: perseus: name: %s value: %s" % (name, value))
    if name == 'perseus_setSampleRate_rx':
          value = int(value)
          self.application.OnBtnDecimation(rate=value)
          perseus.set_sampling_rate(value)
          self.curren_dec = value


  def VarDecimGetChoices(self):	# Not used to set sample rate
    if DEBUG: print ("perseus hardware: VarDecimGetChoices")
    return list(map(str, self.rates)) # convert integer to string

  def VarDecimGetLabel(self):	# Return a text label for the decimation control.
    return 'Sample rates: '

  def VarDecimGetIndex(self):	# Return the index 0, 1, ... of the current decimation.
      for i in range(len(self.rates)):
          if self.rates[i] == self.current_rate:
              return i
      return 0

  def VarDecimSet(self, index=None):	# Called when the control is operated; if index==None, called on startup.
      print ("perseus hardware: VarDecimSet: index: %s" % (index))
      if index == None:
          if DEBUG: print ("perseus hardware: VarDecimSet: current sampling rate: %d" % self.current_rate)
          new_rate = self.current_rate = self.application.vardecim_set
      else:
          new_rate = self.rates[index]

      if DEBUG: print ("perseus hardware: VarDecimSet: New sampling rate: %d" % new_rate)
      perseus.set_sampling_rate(int(new_rate))
      self.current_rate = int(new_rate)

      return int(new_rate)

  def VarDecimRange(self):  # Return the lowest and highest sample rate.
      if DEBUG: print ("perseus hardware: VarDecimRange: %s" % self.rates)
      return (self.rates[0], self.rates[-1])

  def OnButtonAntenna(self, event):
    btn = event.GetEventObject()
    n = btn.index
    if DEBUG: print ("OnButtonAntenna: %d status: %d" % (n, self.wb))
    self.wb = n
    perseus.set_input_filter (self.wb)
    
#  def StartSamples(self):	# called by the sound thread
#    print("perseus hardware: StartSamples")

#  def StopSamples(self):	# called by the sound thread
#    print("perseus hardware: StopSamples")
