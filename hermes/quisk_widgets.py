# Please do not change this widgets module for Quisk.  Instead copy
# it to your own quisk_widgets.py and make changes there.
#
# This module is used to add extra widgets to the QUISK screen.

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import math, wx, time
from configure import ComboCtrl


class BottomWidgets:	# Add extra widgets to the bottom of the screen
  REG_ANTENNA_TUNER = 7
  def __init__(self, app, hardware, conf, frame, gbs, vertBox):
    self.config = conf
    self.hardware = hardware
    self.application = app
    self.start_row = app.widget_row			# The first available row
    self.start_col = app.button_start_col	# The start of the button columns
    self.tuner_time = 0
    self.Widgets_0x06(app, hardware, conf, frame, gbs, vertBox)
  def Widgets_0x06(self, app, hardware, conf, frame, gbs, vertBox):
    self.num_rows_added = 1
    start_row = self.start_row
    self.atu_ctrl = ComboCtrl(frame, "ATU", ["Tune", "Bypass"], True)
    gbs.Add(self.atu_ctrl, (start_row, self.start_col), (1, 2), flag=wx.EXPAND)
    bw, bh = self.atu_ctrl.GetMinSize()
    frame.Bind(wx.EVT_COMBOBOX_CLOSEUP, self.OnAtu)
    init = app.hermes_LNA_dB
    self.sliderLNA = app.SliderBoxHH(frame, 'RfLna %d dB', init, -12, 48, self.OnLNA, True)
    self.sliderLNA.idName = "RfLna"
    app.midiControls["RfLna"]	= (self.sliderLNA,	self.OnLNA)
    hardware.ChangeLNA(init)
    gbs.Add(self.sliderLNA, (start_row, self.start_col + 2), (1, 8), flag=wx.EXPAND)
    if conf.button_layout == "Small screen":
      # Display four data items in a single window
      self.text_temperature = app.QuiskText1(frame, '', bh)
      self.text_pa_current = app.QuiskText1(frame, '', bh)
      self.text_fwd_power = app.QuiskText1(frame, '', bh)
      self.text_swr = app.QuiskText1(frame, '', bh)
      self.text_data = self.text_temperature
      self.text_pa_current.Hide()
      self.text_fwd_power.Hide()
      self.text_swr.Hide()
      b = app.QuiskPushbutton(frame, self.OnTextDataMenu, '..')
      szr = self.data_sizer = wx.BoxSizer(wx.HORIZONTAL)
      szr.Add(self.text_data, 1, flag=wx.ALIGN_CENTER_VERTICAL)
      szr.Add(b, 0, flag=wx.ALIGN_CENTER_VERTICAL)
      gbs.Add(szr, (start_row, self.start_col + 10), (1, 2), flag=wx.EXPAND)
      # Make a popup menu for the data window
      self.text_data_menu = wx.Menu()
      item = self.text_data_menu.Append(-1, 'Temperature')
      app.Bind(wx.EVT_MENU, self.OnDataTemperature, item)
      item = self.text_data_menu.Append(-1, 'PA Current')
      app.Bind(wx.EVT_MENU, self.OnDataPaCurrent, item)
      item = self.text_data_menu.Append(-1, 'Fwd Power')
      app.Bind(wx.EVT_MENU, self.OnDataFwdPower, item)
      item = self.text_data_menu.Append(-1, 'SWR')
      app.Bind(wx.EVT_MENU, self.OnDataSwr, item)
    else:
      szr = wx.BoxSizer(wx.HORIZONTAL)
      gbs.Add(szr, (start_row, self.start_col + 10), (1, 18), flag=wx.EXPAND)
      text_temperature = wx.StaticText(frame, -1, ' Temp 100DC XX', style=wx.ST_NO_AUTORESIZE)
      size = text_temperature.GetBestSize()
      text_temperature.Destroy()
      self.text_temperature = wx.StaticText(frame, -1, '', size=size, style=wx.ST_NO_AUTORESIZE)
      self.text_pa_current = wx.StaticText(frame, -1, '', size=size, style=wx.ST_NO_AUTORESIZE)
      self.text_fwd_power = wx.StaticText(frame, -1, '', size=size, style=wx.ST_NO_AUTORESIZE)
      self.text_swr = wx.StaticText(frame, -1, '', size=size, style=wx.ST_NO_AUTORESIZE)
      flag=wx.ALIGN_LEFT|wx.ALIGN_CENTER_VERTICAL
      szr.Add(self.text_temperature, 0, flag=flag)
      szr.Add(self.text_pa_current, 0, flag=flag)
      szr.Add(self.text_fwd_power, 0, flag=flag)
      szr.Add(self.text_swr, 0, flag=flag)
  def OnAtu(self, event):
    if not self.hardware.io_board.have_IO_Board:
      self.atu_ctrl.SetText("No ATU")
      self.tuner_time = 0
    elif self.atu_ctrl.GetValue() == "Tune":
      self.atu_ctrl.SetText("Tuning")
      self.tuner_time = time.time()
      self.hardware.WriteI2C(0x7d, 0x1D, self.REG_ANTENNA_TUNER, 1)
    else:
      self.atu_ctrl.SetText("Bypass")
      self.tuner_time = 0
      self.hardware.WriteI2C(0x7d, 0x1D, self.REG_ANTENNA_TUNER, 2)
      if self.application.spotButton.GetValue():
        self.application.spotButton.SetValue(False, True)
    self.sliderLNA.SetFocus()
  def OnLNA(self, event=None):
    value = self.sliderLNA.GetValue()
    self.hardware.ChangeLNA(value)
    self.application.hermes_LNA_dB = value
  def Code2Temp(self):		# Convert the HermesLite temperature code to the temperature
    temp = self.hardware.hermes_temperature
    # For best accuracy, 3.26 should be a user's measured 3.3V supply voltage.
    temp = (3.26 * (temp/4096.0) - 0.5)/0.01
    return temp
  def Code2Current(self):	# Convert the HermesLite PA current code to amps
    current = self.hardware.hermes_pa_current
    # 3.26 Ref voltage
    # 4096 steps in ADC
    # Gain of x50 for sense amp
    # Sense resistor is 0.04 Ohms
    current = ((3.26 * (current/4096.0))/50.0)/0.04
    # Scale by resistor voltage divider 1000/(1000+270) at input of slow ADC
    current = current / (1000.0/1270.0)
    return current
  def Code2FwdRevWatts(self, fwd, rev):	# Convert the HermesLite fwd/rev power code to watts forward and reverse
    #print (self.hardware.hermes_rev_power, self.hardware.hermes_fwd_power)
    fwd = self.hardware.InterpolatePower(fwd)
    rev = self.hardware.InterpolatePower(rev)
    # Which voltage is forward and reverse depends on the polarity of the current sense transformer
    if fwd >= rev:
      return fwd, rev
    else:
      return rev, fwd
  def UpdateText(self):
    # Temperature
    temp = self.Code2Temp()
    temp = (" Temp %3.0f" % temp) + u'\u2103'
    self.text_temperature.SetLabel(temp)
    # power amp current
    current = self.Code2Current()
    current = " PA %4.0f ma" % (1000*current)
    self.text_pa_current.SetLabel(current)
    # forward and reverse peak power
    fwd, rev = self.Code2FwdRevWatts(self.hardware.hermes_fwd_peak, self.hardware.hermes_rev_peak)
    # forward less reverse power
    power = fwd - rev
    if power < 0.0:
      power = 0.0
    text = " PEP %3.1f watts" % power
    self.text_fwd_power.SetLabel(text)
    # SWR based on average power
    fwd, rev = self.Code2FwdRevWatts(self.hardware.hermes_fwd_power, self.hardware.hermes_rev_power)
    if fwd >= 0.05:
      gamma = math.sqrt(rev / fwd)
      if gamma < 0.98:
        swr = (1.0 + gamma) / (1.0 - gamma)
      else:
        swr = 99.0
      if swr < 9.95:
        text = " SWR %4.2f" % swr
      else:
        text = " SWR %4.0f" % swr
    else:
      text = " SWR  ---"
    self.text_swr.SetLabel(text)
    if self.tuner_time > 0:
      ret = self.hardware.io_board.Receive(self.REG_ANTENNA_TUNER)
      if ret:
        code = ret[0]
        if code == 0:
          t = "ATU OK"
          self.tuner_time = 0
        elif code == 0xEE:
          t = "ATU RF"
        elif code >= 0xF0:
          t = "Err 0x%X" % code
          self.tuner_time = 0
        else:
          t = "ATU 0x%X" % code
        self.atu_ctrl.SetText(t)
        if code == 0xEE:
          if not self.application.spotButton.GetValue():
            self.application.spotButton.SetValue(True, True)
        elif self.application.spotButton.GetValue():
          self.application.spotButton.SetValue(False, True)
      else:
        code = -1
      if code != 0 and self.tuner_time > 0 and time.time() - self.tuner_time > 20:
        self.tuner_time = 0
        self.atu_ctrl.SetText("ATU Err")
  def OnTextDataMenu(self, event):
    btn = event.GetEventObject()
    btn.PopupMenu(self.text_data_menu, (0,0))
  def OnDataTemperature(self, event):
    self.data_sizer.Replace(self.text_data, self.text_temperature)
    self.text_data.Hide()
    self.text_data = self.text_temperature
    self.text_data.Show()
    self.data_sizer.Layout()
  def OnDataPaCurrent(self, event):
    self.data_sizer.Replace(self.text_data, self.text_pa_current)
    self.text_data.Hide()
    self.text_data = self.text_pa_current
    self.text_data.Show()
    self.data_sizer.Layout()
  def OnDataFwdPower(self, event):
    self.data_sizer.Replace(self.text_data, self.text_fwd_power)
    self.text_data.Hide()
    self.text_data = self.text_fwd_power
    self.text_data.Show()
    self.data_sizer.Layout()
  def OnDataSwr(self, event):
    self.data_sizer.Replace(self.text_data, self.text_swr)
    self.text_data.Hide()
    self.text_data = self.text_swr
    self.text_data.Show()
    self.data_sizer.Layout()
