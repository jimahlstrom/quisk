# Please do not change this widgets module for Quisk.  Instead copy
# it to your own quisk_widgets.py and make changes there.
#
# This module is used to add extra widgets to the QUISK screen.

import wx, math

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
    bw, bh = self.btnPreamp.GetMinSize()
    init = app.hermes_atten_dB
    self.sliderAtten = app.SliderBoxHH(frame, 'Atten %d dB', init, 0, 31, self.OnAtten, True)
    self.sliderAtten.idName = "RfAtten"
    app.midiControls["RfAtten"]	= (self.sliderAtten, self.OnAtten)
    hardware.ChangeAtten(init)
    gbs.Add(self.sliderAtten, (self.start_row, self.start_col + 2), (1, 6), flag=wx.EXPAND)
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
      gbs.Add(szr, (self.start_row, self.start_col + 10), (1, 2), flag=wx.EXPAND)
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
      gbs.Add(szr, (self.start_row, self.start_col + 10), (1, 18), flag=wx.EXPAND)
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
  def OnBtnPreamp(self, event):
    self.hardware.ChangePreamp(self.btnPreamp.GetValue())
  def OnAtten(self, event):
    value = self.sliderAtten.GetValue()
    self.hardware.ChangeAtten(value)
    self.application.hermes_atten_dB = value
  def Code2FwdRevWatts(self, fwd, rev):	# Convert the fwd/rev power code to watts forward and reverse
    #print (self.hardware.hermes_rev_power, self.hardware.hermes_fwd_power)
    fwd = self.hardware.InterpolatePower(fwd)
    rev = self.hardware.InterpolatePower(rev)
    # Which voltage is forward and reverse depends on the polarity of the current sense transformer
    if fwd >= rev:
      return fwd, rev
    else:
      return rev, fwd
  def UpdateText(self):		# Called at intervals
    conf = self.config
    # Temperature
    temp = self.hardware.hermes_temperature
    if conf.calibrate_temp_20 == conf.calibrate_temp_40:
      temp = " Temp ADC %4d" % temp
    else:
      m = 20.0 / (conf.calibrate_temp_40 - conf.calibrate_temp_20)
      b = 20.0 - m * conf.calibrate_temp_20
      temp = m * temp + b
      temp = (" Temp %3.0f" % temp) + u'\u2103'
    self.text_temperature.SetLabel(temp)
    # power amp current
    current = self.hardware.hermes_pa_current
    if conf.calibrate_current_0 == conf.calibrate_current_1:
      current = " Amps ADC %4d" % current
    else:
      m = 1.0 / (conf.calibrate_current_1 - conf.calibrate_current_0)
      b = - m * conf.calibrate_current_0
      current = m * current + b
      current = " PA %4.0f ma" % (1000*current)
    self.text_pa_current.SetLabel(current)
    # forward and reverse peak power
    if not self.hardware.power_interpolator:	# For no interpolator, show ADC
      fwd = self.hardware.hermes_fwd_power
      rev = self.hardware.hermes_rev_power
      self.text_fwd_power.SetLabel("Fwd ADC %4d" % fwd)
      self.text_swr.SetLabel("Rev ADC %4d" % rev)
      return
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
