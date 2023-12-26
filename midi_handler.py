# This module implements Midi processing in Quisk. The OnReadMIDI() method is called for any Midi bytes
# received. Do not change this file. If you want to replace it with your own Midi handler, create a
# configuration file, copy this file into it and make any changes there. Quisk will use your configuration
# file MidiHandler instead of this one.

# Midi messages are generally three bytes long. The first byte is the status and has the most significant bit set.
# Subsequent bytes have the most significant bit zero. The status byte of a channel message is a 1 bit, three bits of message
# type and 4 bits of channel number. This is followed by two bytes of data.

# For Note On (status 0x9?) and Note Off (status 0x8?) the data bytes are the note number and velocity. Velocity
# indicates how hard the key was pressed. If the velocity of a Note On message is zero it is treated the same as Note Off.

# For a control change (status = 0xB?) the data bytes are the controller number and the controller value. For some controllers
# it only matters if the value is less than 64 (down) or 64 or greater (up). For other controllers the value 0 to 127
# is the actual control setting.

import traceback

class MidiHandler:	# Quisk calls this to make the Midi handler instance.
  tune_speed = {0:10, 1:20, 2:50, 3:100, 4:200, 5:500, 6:1000, 7:2000, 8:5000, 9:10000}
  slider_speed = {0:1, 1:2, 2:3, 3:5, 4:7, 5:9, 6:12, 7:15, 8:18, 9:22}
  def __init__(self, app, conf):
    self.app = app		# The application object
    self.conf = conf		# The configuration settings
    self.midi_message = []	# Save Midi bytes until a whole message is received.
  def OnReadMIDI(self, byts):	# Quisk calls this for any Midi bytes received.
    for byt in byts:
      if byt & 0x80:		# this is a status byte and the start of a new message
        self.midi_message = [byt]
      else:
        self.midi_message.append(byt)
      if len(self.midi_message) == 3:
        #print ("0x%2X%02X %d" % tuple(self.midi_message))
        status   = self.midi_message[0]
        status = status & 0xF0	# Ignore channel
        if status == 0x90:	# Note On
          if self.midi_message[2] == 0:		# Note On with zero velocity is the same as Note Off
            self.NoteOff()
          else:
            self.NoteOn()
        elif status == 0x80:	# Note Off
          self.NoteOff()
        elif status == 0xB0:	# Control Change
          try:
            name = self.app.local_conf.MidiNoteDict["0x%02X%02X" % (self.midi_message[0], self.midi_message[1])]
          except:
            pass
            #traceback.print_exc()
          else:
            if len(name) > 3 and name[-3] == " " and name[-2] in "+-" and name[-1] in "0123456789":
              self.JogWheel(name)
            else:
              self.ControlKnob(name)
  def NoteOn(self):
    try:
      name = self.app.local_conf.MidiNoteDict["0x%02X%02X" % (self.midi_message[0], self.midi_message[1])]
      btn = self.app.idName2Button[name]
    except:
      return
    if btn.idName == 'PTT' and not self.conf.midi_ptt_toggle:
      btn.SetValue(True, True)
    else:
      btn.Shortcut(None, name)
  def NoteOff(self):
    try:	# Look up the Note On name
      name = self.app.local_conf.MidiNoteDict["0x9%X%02X" % (self.midi_message[0] & 0xF, self.midi_message[1])]
      btn = self.app.idName2Button[name]
    except:
      return
    if hasattr(btn, "repeat_state"):	# This is a QuiskRepeatbutton
      btn.Shortcut(None, "_end_")
    elif btn.idName == 'PTT' and not self.conf.midi_ptt_toggle:
      btn.SetValue(False, True)
  def ControlKnob(self, name):
    if self.midi_message[2] == 64:	# Mid control
      dec_value = 0.5
    else:
      dec_value = self.midi_message[2] / 127.0
    if name == "Tune":
      tune = self.app.sample_rate * (dec_value - 0.5) * 0.98
      tune = int(tune)
      self.app.ChangeHwFrequency(tune, self.app.VFO, 'FreqEntry')
    elif name == "Rit":		# Offset values by the CW tone frequency
      ctrl, func = self.app.midiControls[name]
      value = self.midi_message[2] - 64		# Center value
      if self.app.mode == 'CWU':
        offset = - self.conf.cwTone
        value = value * 1000 // 63 + offset
      elif self.app.mode == 'CWL':
        offset = self.conf.cwTone
        value = value * 1000 // 63 + offset
      else:
        offset = 0
        value = value * 2000 // 63 + offset
      if value < ctrl.themin:
        value = ctrl.themin
      elif value > ctrl.themax:
        value = ctrl.themax
      ctrl.SetValue(value)
      if self.app.remote_control_head:
        self.app.Hardware.RemoteCtlSend(f'{ctrl.idName};{ctrl.GetValue()}\n')
      func()
    elif name in self.app.midiControls:
      ctrl, func = self.app.midiControls[name]
      if ctrl:
        ctrl.SetDecValue(dec_value, False)
        if self.app.remote_control_head:
          self.app.Hardware.RemoteCtlSend(f'{ctrl.idName};{ctrl.GetValue()}\n')
        func()
    else:	# Try to treat as Note On/Off
      try:
        btn = self.app.idName2Button[name]
      except:
        return
      if self.midi_message[2] == 0:		# Note On with zero velocity is the same as Note Off
        if hasattr(btn, "repeat_state"):	# This is a QuiskRepeatbutton
          btn.Shortcut(None, "_end_")
      else:		# Note On
        btn.Shortcut(None, name)
  def JogWheel(self, name):
    speed = int(name[-1])
    if name[-2] == '+':
      direction = +1
    else:
      direction = -1
    name = name[0:-3]
    if name == "Tune":
      freq = self.app.txFreq + self.app.VFO
      delta = self.tune_speed[speed]
      if self.midi_message[2] < 64:
        freq += direction * delta
      else:
        freq -= direction * delta
      freq = ((freq + delta // 2) // delta) * delta
      tune = freq - self.app.VFO
      d = self.app.sample_rate * 45 // 100
      if -d <= tune <= d:  # Frequency is on-screen
        vfo = self.app.VFO
      else:  # Change the VFO
        vfo = (freq // 5000) * 5000 - 5000
        tune = freq - vfo
      self.app.ChangeHwFrequency(tune, vfo, 'FreqEntry')
    elif name in self.app.midiControls:
      ctrl, func = self.app.midiControls[name]
      self.AdjSlider(ctrl, direction, speed)
      if self.app.remote_control_head:
        self.app.Hardware.RemoteCtlSend(f'{ctrl.idName};{ctrl.GetValue()}\n')
      func()
    else:
      pass #print ("Unknown jog name", name)
  def AdjSlider(self, ctrl, direction, speed):
    value = ctrl.GetValue()
    if self.midi_message[2] < 64:
      value += direction * self.slider_speed[speed]
    else:
      value -= direction * self.slider_speed[speed]
    if value < ctrl.themin:
      value = ctrl.themin
    elif value > ctrl.themax:
      value = ctrl.themax
    ctrl.SetValue(value)
