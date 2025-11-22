# Please do not change this widgets module for Quisk.  Instead copy
# it to your own quisk_widgets.py and make changes there.
#
# This module is used to add extra widgets to the QUISK screen.

import wx

class BottomWidgets:	# Add extra widgets to the bottom of the screen
  def __init__(self, app, hardware, conf, frame, gbs, vertBox):
    self.config = conf
    self.hardware = hardware
    self.application = app
    self.start_row = app.widget_row		# The first available row
    self.start_col = app.button_start_col	# The start of the button columns
    self.num_rows_added = 1
    self.btnPreamp = app.QuiskCheckbutton(frame, self.OnBtnPreamp, text='Preamp')
    gbs.Add(self.btnPreamp, (self.start_row, self.start_col), flag=wx.EXPAND)
    init = app.hermes_atten_dB
    self.sliderAtten = app.SliderBoxHH(frame, 'Atten %d dB', init, 0, 31, self.OnAtten, True)
    self.sliderAtten.idName = "RfAtten"
    app.midiControls["RfAtten"]	= (self.sliderAtten, self.OnAtten)
    hardware.ChangeAtten(init)
    gbs.Add(self.sliderAtten, (self.start_row, self.start_col + 2), (1, 6), flag=wx.EXPAND)
  def OnBtnPreamp(self, event):
    self.hardware.ChangePreamp(self.btnPreamp.GetValue())
  def OnAtten(self, event):
    value = self.sliderAtten.GetValue()
    self.hardware.ChangeAtten(value)
    self.application.hermes_atten_dB = value
  def UpdateText(self):		# Called at intervals
    pass
