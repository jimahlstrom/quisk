# This file is part of the Multus project. See https://www.multus-sdr.com/.
# It controls what items appear on the Hardware configuration screen and
# provides logic for each radio.


################ Receivers Multus CW, A transceiver from the Multus project.
## hardware_file_name		Hardware file path, rfile
# This is the file that contains the control logic for each radio.
#hardware_file_name = "multuspkg/quisk_hardware.py"

## widgets_file_name			Widget file path, rfile
# This optional file adds additional controls for the radio.
#widgets_file_name = ""

## rx_max_amplitude_correct		Max ampl correct, number
# If you get your I/Q samples from a sound card, you will need to correct the
# amplitude and phase for inaccuracies in the analog hardware.  The correction is
# entered using the controls from the "Rx Phase" button on the config screen.
# You must enter a positive number.  This controls the range of the control.
#rx_max_amplitude_correct = 0.2

## rx_max_phase_correct			Max phase correct, number
# If you get your I/Q samples from a sound card, you will need to correct the
# amplitude and phase for inaccuracies in the analog hardware.  The correction is
# entered using the controls from the "Rx Phase" button on the config screen.
# You must enter a positive number.  This controls the range of the control in degrees.
#rx_max_phase_correct = 10.0

## tx_level		Tx Level, dict
# This is the level of the Tx audio sent to SoftRock hardware after all processing as a percentage
# number from 0 to 100.
# The level should be below 100 to allow headroom for amplitude and phase adjustments.
# Changes are immediate (no need to restart).
#tx_level = {}

## digital_tx_level			Digital Tx power %, integer
# Digital modes reduce power by the percentage on the config screen.
# This is the maximum value of the slider.
#digital_tx_level = 100

## keyer_speed		Keyer speed WPM, integer
#  This sets the keyer speed in words per minute.
#keyer_speed = 8
#keyer_speed = 13
#keyer_speed = 18
#keyer_speed = 20
#keyer_speed = 30
#keyer_speed = 40

## keyer_type 		Type of keyer, text choice
# Use "Straight" mode for a straight key or a bug. Connect the tip of the key to the transceiver.
# Otherwise, select Iambic-A or Iambic-B mode of operation.
#keyer_type = "Straight"
#keyer_type = "Iambic-A"
#keyer_type = "Iambic-B"

## keyer_space		Keyer space, text choice
# This sets the type of spacing between elements or letters.
# "Element" attempts to provide proper spacing between the elements of a character.
# (The keyer always provides proper spacing between elements).
# "Letter" turns on letter spacing.  This attempts to provide proper spacing between characters.
#keyer_space = "Element"
#keyer_space = "Letter"

## keyer_weight		Keyer weight, integer choice
# A weight of 50% means that the dit time equals the inter-element time, which is standard.
# Reducing the weight increases the inter-element time, increasing the weight reduces it.
#keyer_weight = 50
#keyer_weight = 25
#keyer_weight = 75

## keyer_paddle		Keyer paddle, text choice
# "Normal" means the left paddle is DIT. "Reverse" means the left paddle is DAH.
#keyer_paddle = "Normal"
#keyer_paddle = "Reverse"

import configure
import softrock
from softrock.hardware_usb import Hardware as BaseHardware
from softrock.hardware_usb import IN, OUT, UBYTE2, UBYTE4
import _quisk as QS

DEBUG = 0

class Hardware(BaseHardware):
  def __init__(self, app, conf):
    # Set constant parameters here:
    conf.usb_vendor_id = 0x16c0
    conf.usb_product_id = 0x05dc
    conf.si570_i2c_address = 0x55
    conf.si570_direct_control = False
    conf.si570_xtal_freq = 114285000
    conf.repeater_delay = 0.25
    self.ptt_count = 200 
    self.ptt_on = 0
    BaseHardware.__init__(self, app, conf)
    QS.set_sparams(multus_cw_samples=1)
    self.use_softrock = True
    if DEBUG:
      print ("The name of this radio is", configure.Settings[1])
      print ("The hardware file type is", conf.hardware_file_type)
  def TransferOut(self, address, message):	# message is a bytes array or an integer 0 to 255
    if isinstance(message, int):
      message = message.to_bytes(1, 'big')
    if self.usb_dev:
      self.usb_dev.ctrl_transfer(OUT, address, self.si570_i2c_address + 0x700, 0, message)
    if DEBUG:
      print ("USB send to 0x%X" % address, message)
  def TransferIn(self, address, length):
    if self.usb_dev:
      recv = self.usb_dev.ctrl_transfer(IN, address, self.si570_i2c_address + 0x700, 0, length)
      if DEBUG:
        print ("USB receive from 0x%X" % address, recv)
      return recv
    return None
  def open(self):
    text = BaseHardware.open(self)
    self.InitKeyer()
    return text
  def ChangeMode(self, mode):
    ret = BaseHardware.ChangeMode(self, mode)
    if mode in ("CWL", "CWU"):
      cw_mode = b'C'	# Adding "b" does not affect the value sent
      if DEBUG:
        print ("Mode is CW")
    else:
      cw_mode = b'U'
      if DEBUG:
        print ("Mode is not CW")
    self.TransferOut(0x70, cw_mode)
    return ret
  def PollCwKey(self):  # Called frequently from the sound thread to check the CW key status
    return	# Quisk is always in Rx
  def PollGuiControl(self):	# Called frequently from the GUI thread
    self.ptt_count -= 1
    if self.ptt_count <= 0:
      self.ptt_count = 200 
      reply = self.TransferIn(0xA5, 1)
      if DEBUG:
        print ("PollGuiControl got", reply)
      if reply:
        ptt = reply[0]	# This is 255 for error
        if ptt in (0, 1) and ptt != self.ptt_on:
          self.ptt_on = ptt
          self.application.pttButton.SetValue(ptt, True)
  def InitKeyer(self):
    # Initialize the keyer parameters
    conf = self.conf
    if not hasattr(conf, "keyer_speed"):
      conf.keyer_speed = 18
    if not hasattr(conf, "keyer_type"):
      conf.keyer_type = "Straight"
    if not hasattr(conf, "keyer_space"):
      conf.keyer_space = "Element"
    if not hasattr(conf, "keyer_weight"):
      conf.keyer_weight = 50
    if not hasattr(conf, "keyer_paddle"):
      conf.keyer_paddle = "Normal"
    # We need to initialize in case the persistent hardware values differ from the Quisk values.
    for name in ("keyer_speed", "keyer_type", "keyer_space", "keyer_weight", "keyer_paddle", "cwTone"):
      self.ImmediateChange(name)
  def ImmediateChange(self, name):
    BaseHardware.ImmediateChange(self, name)
    value = getattr(self.conf, name)
    if DEBUG:
      print ("ImmediateChange", name, value)
    if name == "keyer_speed":
      self.TransferOut(0x7B, value)
    elif name == "keyer_type":
      if value == "Straight":
        mode = 0
      elif value == "Iambic-A":
        mode = 1
      elif value == "Iambic-B":
        mode = 2
      else:
        mode = 0
      self.TransferOut(0x71, mode)
    elif name == "keyer_space":
      if value == "Element":
        spacing = 0
      elif value == "Letter":
        spacing = 1
      else:
        spacing = 0
      self.TransferOut(0x75, spacing)
    elif name == "keyer_weight": 
      weight = int(value)
      self.TransferOut(0x77, weight)
    elif name == "keyer_paddle":
      if value == "Normal":
        paddle = 0
      elif value == "Reverse":
        paddle = 1
      else:
        paddle = 0
      self.TransferOut(0x73, paddle)
    elif name == "cwTone":
      if value < 500:
        tone_index = 0	# 400 Hz
      elif 500 <= value < 700:
        tone_index = 1	# 600 Hz
      elif 700 <= value < 900:
        tone_index = 2	# 800 Hz
      else:
        tone_index = 3	# 1000 Hz
      self.TransferOut(0x7F, tone_index)
      
	  
