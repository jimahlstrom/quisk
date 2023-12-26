# This hardware file is for use with the Hermes Lite 2. It disables the power amplifier when the
# transmit frequency including the sidebands is outside of the band selected. Enter this file name
# quisk_hardware_hl2_oob.py as your hardware file on the Config/radio/Hardware screen.

from __future__ import print_function
from __future__ import absolute_import

from hermes.quisk_hardware import Hardware as BaseHardware

class Hardware(BaseHardware):
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.bandEdge1 = 0
    self.bandEdge2 = 0
  def ChangeMode(self, mode):
    BaseHardware.ChangeMode(self, mode)
    self.FixBandEdge()
  def ChangeBand(self, band):
    BaseHardware.ChangeBand(self, band)
    self.FixBandEdge()
  def FixBandEdge(self):	# Reduce the band edges accordig to the transmit mode sidebands
    if self.band in ("Audio", "Time"):		# Rx only
      freq1 = 0
      freq2 = 0
    else:
      try:
        freq1, freq2 = self.conf.BandEdge[self.band]
      except:
        freq1 = 0
        freq2 = 0
    mode = self.mode
    if mode in ("CWL", "CWU"):
      freq1 += 40
      freq2 -= 40
    elif mode in ("USB", "DGT-U", "FDV-U", "IMD"):
      freq2 -= 3000
    elif mode in ("LSB", "DGT-L", "FDV-L"):
      freq1 += 3000
    elif mode == "AM":
      freq1 += 3000
      freq2 -= 3000
    elif mode in ("FM", "DGT-FM"):
      freq1 += 8000
      freq2 -= 8000
    else:
      freq1 += 3000
      freq2 -= 3000
    self.bandEdge1 = freq1
    self.bandEdge2 = freq2
  def HeartBeat(self):
    BaseHardware.HeartBeat(self)
    power_amp_enabled = self.pc2hermes[37] & 0b1000
    if self.bandEdge1 <= self.tx_frequency <= self.bandEdge2:	# Tx frequency is in band
      if not power_amp_enabled and self.conf.hermes_power_amp:
        #print ("Turn HL2 power amp on")
        self.SetControlBit(0x09, 19, 1)
      elif power_amp_enabled and not self.conf.hermes_power_amp:	# Should not happen
        #print ("Turn HL2 power amp ??")
        self.SetControlBit(0x09, 19, 0)
    else:	# Tx frequency is out of band
      if power_amp_enabled:
        #print ("Turn HL2 power amp off")
        self.SetControlBit(0x09, 19, 0)
