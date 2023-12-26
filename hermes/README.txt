This directory has files for the Hermes-Lite project versions 1 and 2.
Please perform all configuration with the Config screens in Quisk.
To start, go to Config/Radios and add a new radio with the general type "Hermes".

Quisk fully supports the Hermes-Lite project. But you may need a custom Widget or
Hardware file for special purposes. If you create these custom files, enter their
location (path) on the Config/radio/Hardware screen. Do not modify the files supplied
by Quisk, as these will be replaced with each new release.



# This is an example of a custom Widgets file. It changes the power calculations
# to that required by an external power bridge. Any methods (functions) defined here are used
# instead of the usual versions.  See the original quisk_widgets.py to see what is available
# for replacement.

from hermes.quisk_widgets import BottomWidgets as BaseWidgets

class BottomWidgets(BaseWidgets):	# Add extra widgets to the bottom of the screen
  # This replaces the default version. You must alter the code to calculate watts for your
  # external power meter that is connected to the Hermes-Lite power ADC.
  def Code2FwdRevWatts(self):	# Convert the HermesLite fwd/rev power code to watts forward and reverse
    # volts = m * code + b	# The N2ADR power circuit is linear in voltage
    # power = (m**2 * code**2  +  2 * b * m * code  +  b**2) / 50
    fwd = self.hardware.hermes_fwd_power	# forward and reverse binary code
    rev = self.hardware.hermes_rev_power
    Vfwd = 3.26 * fwd / 4096.0			# forward and reverse volts
    Vrev = 3.26 * rev / 4096.0
    Pfwd = 2.493 * Vfwd**2 + 0.1165 * Vfwd	# conversion from volts to power in watts
    Prev = 2.493 * Vrev**2 + 0.1165 * Vrev
    return Pfwd, Prev				# return forward and reverse power in watts



# This is an example of a custom Hardware file:

from hermes.quisk_hardware import Hardware as BaseHardware

class Hardware(BaseHardware):
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.usingSpot = False		# Use bit C2[7] as the Spot indicator
  def ChangeBand(self, band):
    # band is a string: "60", "40", "WWV", etc.
    # The call to BaseHardware will set C2 according to the Hermes_BandDict{}
    ret = BaseHardware.ChangeBand(self, band)
    if self.usingSpot:
      byte = self.GetControlByte(0, 2)		# C0 index == 0, C2: user output
      byte |= 0b10000000
      self.SetControlByte(0, 2, byte)
    return ret
  def OnSpot(self, level):
    # level is -1 for Spot button Off; else the Spot level 0 to 1000.
    ret = BaseHardware.OnSpot(self, level)
    if level >= 0 and not self.usingSpot:		# Spot was turned on
      byte = self.GetControlByte(0, 2)
      byte |= 0b10000000
      self.SetControlByte(0, 2, byte)
      self.usingSpot = True
    elif level < 0 and self.usingSpot:			# Spot was turned off
      byte = self.GetControlByte(0, 2)
      byte &= 0b01111111
      self.SetControlByte(0, 2, byte)
      self.usingSpot = False
    return ret
