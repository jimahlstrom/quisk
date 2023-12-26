# This is a replacement hardware file for the Softrock and similar radios.

# Normally Quisk will change the VFO (center frequency) by large amounts, and perform
# fine tuning within the returned bandwidth. This hardware file does all tuning with the VFO.
# This creates a constant offset between the VFO and the tuning frequency. Specify this file
# as your hardware file on the Config/radio/Hardware screen.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import softrock
from softrock.hardware_usb import Hardware as BaseHardware

class Hardware(BaseHardware):
  def ChangeFrequency(self, tx_freq, vfo_freq, source='', band='', event=None):
    vfo_freq = tx_freq - 10000
    tx, vfo = BaseHardware.ChangeFrequency(self, tx_freq, vfo_freq, source, band, event)
    return tx, vfo
