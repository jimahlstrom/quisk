import sys, wx, wx.lib, os, re, pickle, traceback, json, copy
# Quisk will alter quisk_conf_defaults to include the user's config file.
import quisk_conf_defaults as conf
import _quisk as QS
from quisk_widgets import QuiskPushbutton, QuiskCheckbutton, QuiskBitField, SliderBoxH, SliderBoxHH
from quisk_widgets import FreqFormatter
from quisk_widgets import wxVersion
if wxVersion in ('2', '3'):
  import wx.combo as wxcombo
else:
  wxcombo = wx                  # wxPython Phoenix
try:
  from soapypkg import soapy
except:
  soapy = None

# Settings is [
#   0: Radio_requested, a string radio name or "Ask me" or "ConfigFileRadio"
#   1: Radio in use and last used, a string radio name or "ConfigFileRadio"
#   2: List of radio names
#   3: Parallel list of radio dicts.  These are all the parameters for the corresponding radio.  In
#      general, they are a subset of all the parameters listed in self.sections and self.receiver_data[radio_name].
#   4: Global data common to all radios. This is similar to the radio dicts. Available as local_conf.globals.
#   ]

# radio_dict is a dictionary of variable names and text values for each radio including radio ConfigFileRadio.
# Only variable names from the specified radio and all sections are included. The data comes from the JSON file, and
# may be missing recently added config file items. Use GetValue() to get a configuration datum.

# local_conf is the single instance of class Configuration. conf is the configuration data from quisk_conf_defaults as
# over-writen by JSON data in radio_dict. Items in the radio_dict are generally strings. We convert these strings to Python
# integers, floats, etc. and write them to conf.

# The format for data items is read from quisk_conf_defaults.py, but there are data items not in this file. The
# dictionary name2format has formats and defaults for these additional items.
# The value is a tuple (format name, default value).
name2format = {
  "digital_rx1_name":('text', ''), "digital_rx2_name":('text', ''), "digital_rx3_name":('text', ''),
  "digital_rx4_name":('text', ''), "digital_rx5_name":('text', ''), "digital_rx6_name":('text', ''),
  "digital_rx7_name":('text', ''), "digital_rx8_name":('text', ''), "digital_rx9_name":('text', ''),
  "win_digital_rx1_name":('text', ''), "win_digital_rx2_name":('text', ''), "win_digital_rx3_name":('text', ''),
  "win_digital_rx4_name":('text', ''), "win_digital_rx5_name":('text', ''), "win_digital_rx6_name":('text', ''),
  "win_digital_rx7_name":('text', ''), "win_digital_rx8_name":('text', ''), "win_digital_rx9_name":('text', ''),
  "lin_digital_rx1_name":('text', ''), "lin_digital_rx2_name":('text', ''), "lin_digital_rx3_name":('text', ''),
  "lin_digital_rx4_name":('text', ''), "lin_digital_rx5_name":('text', ''), "lin_digital_rx6_name":('text', ''),
  "lin_digital_rx7_name":('text', ''), "lin_digital_rx8_name":('text', ''), "lin_digital_rx9_name":('text', ''),
}

# Increasing the software version will display a message to re-read the soapy device.
soapy_software_version = 3
wxpython_gtk3_bug = 0

def FormatKhz(dnum):	# Round to 3 decimal places; remove ending ".000"
  t = "%.3f" % dnum
  if t[-4:] == '.000':
    t = t[0:-4]
  return t

def FormatMHz(dnum):	# Pretty print in MHz
  t = "%.6f" % dnum
  for i in range(3):
    if t[-1] == '0':
      t = t[0:-1]
    else:
      break
  return t

def SortKey(x):
  try:
    k = float(x)
  except:
    k = 0.0
  return k

class Configuration:
  def __init__(self, app, AskMe=False, Radio=''):	# Called first
    global application, local_conf, Settings, noname_enable, platform_ignore, platform_accept
    Settings = ["ConfigFileRadio", "ConfigFileRadio", [], [], {}]
    self.globals = Settings[4]		# Will be replaced by quisk_settings.json
    application = app
    local_conf = self
    noname_enable = []
    if sys.platform == 'win32':
      platform_ignore = 'lin_'
      platform_accept = 'win_'
    else:
      platform_accept = 'lin_'
      platform_ignore = 'win_'
    self.sections = []
    self.receiver_data = []
    self.StatePath = conf.settings_file_path
    if not self.StatePath:
      self.StatePath = os.path.join(app.QuiskFilesDir, "quisk_settings.json")
    self.ReadState()
    if AskMe == 'Same':
      pass
    elif Radio:
      choices = Settings[2] + ["ConfigFileRadio"]
      if Radio in choices:
        if Settings[1] != Radio:
          Settings[1] = Radio
          self.settings_changed = True
      else:
        t = "There is no radio named %s. Radios are " % Radio
        for choice in choices:
          t = "%s%s, " % (t, choice)
        t = t[0:-2] + '.'
        dlg = wx.MessageDialog(application.main_frame, t, 'Specify Radio', wx.OK|wx.ICON_ERROR)
        dlg.ShowModal()
        dlg.Destroy()
        sys.exit(0)
    elif AskMe or Settings[0] == "Ask me":
      choices = Settings[2] + ["ConfigFileRadio"]
      dlg = wx.SingleChoiceDialog(None, "", "Start Quisk with this Radio",
          choices, style=wx.DEFAULT_FRAME_STYLE|wx.OK|wx.CANCEL)
      dlg.SetSizeHints(dlg.GetCharWidth() * 36, -1, -1, -1)
      try:
        n = choices.index(Settings[1])		# Set default to last used radio
      except:
        pass
      else:
        dlg.SetSelection(n)
      ok = dlg.ShowModal()
      if ok != wx.ID_OK:
        sys.exit(0)
      select = dlg.GetStringSelection()
      dlg.Destroy()
      if Settings[1] != select:
        Settings[1] = select
        self.settings_changed = True
    else:
      Settings[1] = Settings[0]
    if Settings[1] == "ConfigFileRadio":
      Settings[2].append("ConfigFileRadio")
      Settings[3].append({})
    self.ParseConfig()
    self.originalBandEdge = {}		# save original BandEdge
    self.originalBandEdge.update(conf.BandEdge)
    self.UpdateGlobals()
  def RequiredValues(self, radio_dict):
    radio_type = radio_dict['hardware_file_type']
    # Fill in required values
    if radio_type == "SdrIQ":
      radio_dict["use_sdriq"] = '1'
      if radio_dict['hardware_file_name'] == "sdriqpkg/quisk_hardware.py":
        radio_dict['hardware_file_name'] = "quisk_hardware_sdriq.py"
    else:
      radio_dict["use_sdriq"] = '0'
    if radio_type == "Hermes":
      radio_dict["hermes_bias_adjust"] = "False"
    if radio_type == 'SoapySDR':
      radio_dict["use_soapy"] = '1'
      self.InitSoapyNames(radio_dict)
      if radio_dict.get("soapy_file_version", 0) < soapy_software_version:
        text = "Your SoapySDR device parameters are out of date. Please go to the radio configuration screen and re-read the device parameters."
        dlg = wx.MessageDialog(None, text, 'Please Re-Read Device', wx.OK|wx.ICON_INFORMATION)
        dlg.ShowModal()
        dlg.Destroy()
    else:
      radio_dict["use_soapy"] = '0'
    if radio_type not in ("HiQSDR", "Hermes", "Red Pitaya", "Odyssey", "Odyssey2"):
      radio_dict["use_rx_udp"] = '0'
    if radio_type in ("Hermes", "Red Pitaya", "Odyssey2"):
      if "Hermes_BandDict" not in radio_dict:
        radio_dict["Hermes_BandDict"] = {}
      if "Hermes_BandDictTx" not in radio_dict:
        radio_dict["Hermes_BandDictTx"] = {}
  def UpdateGlobals(self):
    self.RadioName = Settings[1]
    application.BandPlanC2T = {}
    if self.RadioName == "ConfigFileRadio":
      application.BandPlan = conf.BandPlan
      for mode, color in conf.BandPlanColors:
        application.BandPlanC2T[color] = mode
    elif "BandPlanColors" in Settings[4] and "BandPlan" in Settings[4]:
      mode_dict = {"End":None}
      for mode, color in Settings[4]["BandPlanColors"]:
        mode_dict[mode] = color
        application.BandPlanC2T[color] = mode
      plan = []
      for freq, mode in Settings[4]["BandPlan"]:
        freq = int(float(freq) * 1E6 + 0.1)
        try:
          color = mode_dict[mode]
        except:
          print ("Missing color for mode", mode)
          color = '#777777'
        plan.append([freq, color])
      application.BandPlan = plan
    else:
      application.BandPlan = conf.BandPlan
      for mode, color in conf.BandPlanColors:
        application.BandPlanC2T[color] = mode
    if "MidiNoteDict" not in Settings[4]:
      Settings[4]["MidiNoteDict"] = {}
      self.settings_changed = True
    self.MidiNoteDict = Settings[4]["MidiNoteDict"]
    # Convert old to new format, October, 2021
    for txt_note in list(local_conf.MidiNoteDict):	# txt_note is a string
      int_note = int(txt_note, base=0)
      if int_note < 128:
        target = local_conf.MidiNoteDict[txt_note]
        if len(target) > 3 and target[-3] == " " and target[-2] in "+-" and target[-1] in "0123456789":	# Jog wheel
          key = "0xB0%02X" % int_note
        elif target in ("Vol", "STo", "Rit", "Ys", "Yz", "Zo", "Tune"):		# Knob
          key = "0xB0%02X" % int_note
        else:	# Button. Enter as the Note On message.
          key = "0x90%02X" % int_note
        local_conf.MidiNoteDict[key] = target
        del local_conf.MidiNoteDict[txt_note]
        self.settings_changed = True
  def UpdateConf(self):		# Called second to update the configuration for the selected radio
    # Items in the radio_dict are generally strings. Convert these strings to Python integers, floats,
    # etc. and write them to conf.
    if Settings[1] == "ConfigFileRadio":
      return
    radio_dict = self.GetRadioDict()
    # fill in conf from our configuration data; convert text items to Python objects
    errors = ''
    for k, v in list(radio_dict.items()):	# radio_dict may change size during iteration
      if k == 'favorites_file_path':	# A null string is equivalent to "not entered"
        if not v.strip():
          continue
      if k in ('power_meter_local_calibrations', ):	# present in configuration data but not in the config file
        continue
      if k[0:6] == 'soapy_':	# present in configuration data but not in the config file
        continue
      if k[0:6] == 'Hware_':	# contained in hardware file, not in configuration data nor config file
        continue
      try:
        fmt = self.format4name[k]
      except:
        errors = errors + "Ignore obsolete parameter %s\n" % k
        del radio_dict[k]
        self.settings_changed = True
        continue
      k4 = k[0:4]
      if k4 == platform_ignore:
        continue
      elif k4 == platform_accept:
        k = k[4:]
      fmt4 = fmt[0:4]
      if fmt4 not in ('dict', 'list'):
        i1 = v.find('#')
        if i1 > 0:
          v = v[0:i1]
      try:
        if fmt4 == 'text':	# Note: JSON returns Unicode strings !!!
          setattr(conf, k, v)
        elif fmt4 == 'dict':
          if isinstance(v, dict):
            setattr(conf, k, v)
          else:
            raise ValueError()
        elif fmt4 == 'list':
          if isinstance(v, list):
            setattr(conf, k, v)
          else:
            raise ValueError()
        elif fmt4 == 'inte':
          setattr(conf, k, int(v, base=0))
        elif fmt4 == 'numb':
          setattr(conf, k, float(v))
        elif fmt4 == 'bool':
          if v == "True":
            setattr(conf, k, True)
          else:
            setattr(conf, k, False)
        elif fmt4 == 'rfil':
          pass
        elif fmt4 == 'keyc':	# key code
          if v == "None":
            x = None
          else:
            x = eval(v)
            x = int(x)
          if k == 'hot_key_ptt2' and not isinstance(x, int):
            setattr(conf, k, wx.ACCEL_NORMAL)
          else:
            setattr(conf, k, x)
        else:
          print ("Unknown format for", k, fmt)
      except:
        del radio_dict[k]
        self.settings_changed = True
        errors = errors + "Failed to set %s to %s using format %s\n" % (k, v, fmt)
        #traceback.print_exc()
    if conf.color_scheme != 'A':
      conf.__dict__.update(getattr(conf, 'color_scheme_' + conf.color_scheme))
    self.RequiredValues(radio_dict)	# Why not update conf too??? This only updates the radio_dict.
    if errors:
      dlg = wx.MessageDialog(None, errors,
        'Update Settings', wx.OK|wx.ICON_ERROR)
      ret = dlg.ShowModal()
      dlg.Destroy()
  def InitSoapyNames(self, radio_dict):	# Set Soapy data items, but not the hardware available lists and ranges.
    if radio_dict.get('soapy_getFullDuplex_rx', 0):
      radio_dict["add_fdx_button"] = '1'
    else:
      radio_dict["add_fdx_button"] = '0'
    name = 'soapy_gain_mode_rx'
    if name not in radio_dict:
      radio_dict[name] = 'total'
    name = 'soapy_setAntenna_rx'
    if name not in radio_dict:
      radio_dict[name] = ''
    name = 'soapy_gain_values_rx'
    if name not in radio_dict:
      radio_dict[name] = {}
    name = 'soapy_gain_mode_tx'
    if name not in radio_dict:
      radio_dict[name] = 'total'
    name = 'soapy_setAntenna_tx'
    if name not in radio_dict:
      radio_dict[name] = ''
    name = 'soapy_gain_values_tx'
    if name not in radio_dict:
      radio_dict[name] = {}
  def NormPath(self, path):	# Convert between Unix and Window file paths
    if sys.platform == 'win32':
      path = path.replace('/', '\\')
    else:
      path = path.replace('\\', '/')
    return path
  def GetHardware(self):	# Called third to open the hardware file
    if Settings[1] == "ConfigFileRadio":
      return False
    path = self.GetRadioDict()["hardware_file_name"]
    path = self.NormPath(path)
    if not os.path.isfile(path):
      dlg = wx.MessageDialog(None,
        "Can not find the hardware file %s!" % path,
        'Hardware File', wx.OK|wx.ICON_ERROR)
      ret = dlg.ShowModal()
      dlg.Destroy()
      path = 'quisk_hardware_model.py'
    dct = {}
    dct.update(conf.__dict__)		# make items from conf available
    if "Hardware" in dct:
      del dct["Hardware"]
    if 'quisk_hardware' in dct:
      del dct["quisk_hardware"]
    exec(compile(open(path).read(), path, 'exec'), dct)
    if "Hardware" in dct:
      application.Hardware = dct['Hardware'](application, conf)
      return True
    return False
  def Initialize(self):		# Called fourth to fill in our ConfigFileRadio radio from conf
    if Settings[1] == "ConfigFileRadio":
      radio_dict = self.GetRadioDict("ConfigFileRadio")
      typ = self.GuessType()
      radio_dict['hardware_file_type'] = typ
      all_data = []
      all_data = all_data + self.GetReceiverData(typ)
      for name, sdata in self.sections:
        all_data = all_data + sdata
      for data_name, text, fmt, help_text, values in all_data:
        data_name4 = data_name[0:4]
        if data_name4 == platform_ignore:
          continue
        elif data_name4 == platform_accept:
          conf_name = data_name[4:]
        else:
          conf_name = data_name
        try:
          if fmt in ("dict", "list"):
            radio_dict[data_name] = getattr(conf, conf_name)
          else:
            radio_dict[data_name] = str(getattr(conf, conf_name))
        except:
          if data_name == 'playback_rate':
            pass
          else:
            print ('No config file value for', data_name)
  def GetWidgets(self, app, hardware, conf, frame, gbs, vertBox):	# Called fifth
    if Settings[1] == "ConfigFileRadio":
      return False
    path = self.GetRadioDict().get("widgets_file_name", '')
    path = self.NormPath(path)
    if os.path.isfile(path):
      dct = {}
      dct.update(conf.__dict__)		# make items from conf available
      exec(compile(open(path).read(), path, 'exec'), dct)
      if "BottomWidgets" in dct:
        app.bottom_widgets = dct['BottomWidgets'](app, hardware, conf, frame, gbs, vertBox)
    return True
  def OnPageChanging(self, event):	# Called when the top level page changes (not RadioNotebook pages)
    global wxpython_gtk3_bug
    event.Skip()
    notebook = event.GetEventObject()
    index = event.GetSelection()
    page = notebook.GetPage(index)
    if sys.platform != 'win32':		# Work around a bug in wxPython 4.2.1 and gtk3
      w, h = application.main_frame.GetSize()	# Change main window size by +/- one pixel to recalculate sizes
      if wxpython_gtk3_bug:
        wxpython_gtk3_bug = 0
        h += 1
      else:
        wxpython_gtk3_bug = 1
        h -= 1
      application.main_frame.SetSize((w, h))
    if isinstance(page, RadioNotebook):
      if not page.pages:
        page.MakePages()
  def AddPages(self, notebk, width):	# Called sixth to add pages Help, Radios, all radio names
    global win_width
    win_width = width
    self.notebk = notebk
    self.radio_page = Radios(notebk)
    notebk.AddPage(self.radio_page, "Radios")
    self.radios_page_start = notebk.GetPageCount()
    notebk.Bind(wx.EVT_NOTEBOOK_PAGE_CHANGED, self.OnPageChanging, notebk)
    if Settings[1] in Settings[2]:
      page = RadioNotebook(notebk, Settings[1])
      notebk.AddPage(page, "*%s*" % Settings[1])
    for name in Settings[2]:
      if name != Settings[1]:
        page = RadioNotebook(notebk, name)
        notebk.AddPage(page, name)
  def GuessType(self):
    udp = conf.use_rx_udp
    if conf.use_sdriq:
      return 'SdrIQ'
    elif udp == 1:
      return 'HiQSDR'
    elif udp == 2:
      return 'HiQSDR'
    elif udp == 10:
      return 'Hermes'
    elif udp > 0:
      return 'HiQSDR'
    return 'SoftRock USB'
  def FindNotebookPage(self, name):
    for index in range(self.radios_page_start, self.notebk.GetPageCount()):
      tab_text = self.notebk.GetPageText(index)
      if tab_text[0] == '*':
        tab_text = tab_text[1:-1]
      if tab_text == name:
        return index
    return None
  def AddRadio(self, radio_name, typ):
    radio_dict = {}
    radio_dict['hardware_file_type'] = typ
    Settings[2].append(radio_name)
    Settings[3].append(radio_dict)
    for data_name, text, fmt, help_text, values in self.GetReceiverData(typ):
      radio_dict[data_name] = values[0]
    for name, data in self.sections:
      for data_name, text, fmt, help_text, values in data:
        radio_dict[data_name] = values[0]
    # Change some default values in quisk_conf_defaults.py based on radio type
    if typ in ("HiQSDR", "Hermes", "Red Pitaya", "Odyssey", "Odyssey2"):
      radio_dict["add_fdx_button"] = '1'
    page = RadioNotebook(self.notebk, radio_name)
    self.notebk.AddPage(page, radio_name)
    return True
  def CopyRadio(self, old_name, new_name):
    radio_dict = {}
    index = Settings[2].index(old_name)
    radio_dict.update(Settings[3][index])
    Settings[2].append(new_name)
    Settings[3].append(radio_dict)
    page = RadioNotebook(self.notebk, new_name)
    self.notebk.AddPage(page, new_name)
    return True
  def RenameRadio(self, old, new):
    n = self.FindNotebookPage(old)
    if n is None:
      return
    if old == Settings[1]:
      self.notebk.SetPageText(n, "*%s*" % new)
    else:
      self.notebk.SetPageText(n, new)
    index = Settings[2].index(old)
    Settings[2][index] = new
    self.notebk.GetPage(n).NewName(new)
    if old == "ConfigFileRadio":
      for ctrl in noname_enable:
        ctrl.Enable()
    return True
  def DeleteRadio(self, name):
    index = Settings[2].index(name)
    del Settings[2][index]
    del Settings[3][index]
    n = self.FindNotebookPage(name)
    if n is not None:
      self.notebk.DeletePage(n)
      return True
  def GetRadioDict(self, radio_name=None):	# None radio_name means the current radio
    if radio_name:
      index = Settings[2].index(radio_name)
    else:	# index of radio in use
      index = Settings[2].index(Settings[1])
    return Settings[3][index]
  #def GetItem(self, name, deflt=None, accept=None):     # return item or default. accept can be "win_" or "lin_"
  #  dct = self.GetRadioDict()
  #  if accept:
  #    return dct.get(accept+name, deflt)
  #  return dct.get(name, deflt)
  def GetSectionData(self, section_name):
    for sname, data in self.sections:
      if sname == section_name:
        return data
    return None
  def GetReceiverData(self, receiver_name):
    for rxname, data in self.receiver_data:
      if rxname == receiver_name:
        return data
    return ()
  def GetReceiverDatum(self, receiver_name, item_name):
    for rxname, data in self.receiver_data:
      if rxname == receiver_name:
        for data_name, text, fmt, help_text, values in data:
          if item_name == data_name:
            return values[0]
        break
    return ''
  def GetReceiverItemTH(self, receiver_name, item_name):
    for rxname, data in self.receiver_data:
      if rxname == receiver_name:
        for data_name, text, fmt, help_text, values in data:
          if item_name == data_name:
            return text, help_text
        break
    return '', ''
  def ReceiverHasName(self, receiver_name, item_name):
    for rxname, data in self.receiver_data:
      if rxname == receiver_name:
        for data_name, text, fmt, help_text, values in data:
          if item_name == data_name:
            return True
        break
    return False
  def ReadState(self):
    self.settings_changed = False
    global Settings
    try:
      fp = open(self.StatePath, "r")
    except:
      return
    try:
      Settings = json.load(fp)
    except:
      traceback.print_exc()
    fp.close()
    try:	# Do not save settings for radio ConfigFileRadio
      index = Settings[2].index("ConfigFileRadio")
    except ValueError:
      pass
    else:
      del Settings[2][index]
      del Settings[3][index]
    for sdict in Settings[3]:
      # Fixup for prior errors that save dictionaries as strings
      for name in ("tx_level", "HiQSDR_BandDict", "Hermes_BandDict", "Hermes_BandDictTx"):
        if name in sdict:
          if not isinstance(sdict[name], dict):
            print ("Bad dictionary for", name)
            sdict[name] = {}
            self.settings_changed = True
      # Python None is saved as "null"
      if "tx_level" in sdict:
        if "null" in sdict["tx_level"]:
          v = sdict["tx_level"]["null"]
          sdict["tx_level"][None] = v
          del sdict["tx_level"]["null"]
    # Add global section if it is absent
    length = len(Settings)
    if length < 4:	# Serious error
      pass
    elif length == 4:	# Old file without global section
      Settings.append({})
      self.settings_changed = True
    else:		# Settings[4] must be a dict
      if not isinstance(Settings[4], dict):
        Settings[4] = {}
        self.settings_changed = True
    self.globals = Settings[4]
  def SaveState(self):
    if not self.settings_changed:
      return
    try:
      fp = open(self.StatePath, "w")
    except:
      traceback.print_exc()
      return
    json.dump(Settings, fp, indent=2)
    fp.close()
    self.settings_changed = False
  def ParseConfig(self):
    # ParseConfig() fills self.sections, self.receiver_data, and
    # self.format4name with the items that Configuration understands.
    # Dicts and lists are Python objects.  All other items are text, not Python objects.
    #
    # This parses the quisk_conf_defaults.py file and user-defined radios in ./*pkg/quisk_hardware.py.
    #
    # Sections start with 16 #, section name
    # self.sections is a list of [section_name, section_data]
    # section_data is a list of [data_name, text, fmt, help_text, values]

    # Receiver sections start with 16 #, "Receivers ", receiver name, explain
    # self.receiver_data is a list of [receiver_name, receiver_data]
    # receiver_data is a list of [data_name, text, fmt, help_text, values]

    # Variable names start with ## variable_name   variable_text, format
    #     The format is integer, number, text, boolean, integer choice, text choice, rfile
    #     Then some help text starting with "# "
    #     Then a list of possible value#explain with the default first
    #     Then a blank line to end.
    self.format4name = {}
    for name in name2format:
      self.format4name[name] = name2format[name][0]
    self.format4name['hardware_file_type'] = 'text'
    self._ParserConf('quisk_conf_defaults.py')
    # Read any user-defined radio types
    for dirname in os.listdir('.'):
      if not os.path.isdir(dirname) or dirname[-3:] != 'pkg':
        continue
      if dirname in ('freedvpkg', 'sdriqpkg', 'soapypkg'):
        continue
      filename = os.path.join(dirname, 'quisk_hardware.py')
      if not os.path.isfile(filename):
        continue
      try:
        self._ParserConf(filename)
      except:
        traceback.print_exc()
  def _ParserConf(self, filename):
    re_AeqB = re.compile(r"^#?(\w+)\s*=\s*([^#]+)#*(.*)")		# item values "a = b"
    section = None
    data_name = None
    multi_line = False
    fp = open(filename, "r", encoding="utf8")
    for line in fp:
      line = line.strip()
      if not line:
        data_name = None
        continue
      if line[0:27] == '################ Receivers ':
        section = 'Receivers'
        args = line[27:].split(',', 1)
        rxname = args[0].strip()
        section_data = []
        self.receiver_data.append((rxname, section_data))
      elif line[0:17] == '################ ':
        section = line[17:].strip()
        if section in ('Colors', 'Obsolete'):
          section = None
          continue
        rxname = None
        section_data = []
        self.sections.append((section, section_data))
      if not section:
        continue
      if line[0:3] == '## ':		# item_name   item_text, format
        args = line[3:].split(None, 1)
        data_name = args[0]
        args = args[1].split(',', 1)
        dspl = args[0].strip()
        fmt = args[1].strip()
        value_list = []
        if data_name in self.format4name:
          if self.format4name[data_name] != fmt:
            print (filename, ": Inconsistent format for", data_name, self.format4name[data_name], fmt)
        else:
          self.format4name[data_name] = fmt
        section_data.append([data_name, dspl, fmt, '', value_list])
        multi_line = False
      if not data_name:
        continue
      if multi_line:
        value += line
        #print ("Multi", data_name, type(value), value)
        count = self._multi_count(value)
        if count == 0:
          value = eval(value, conf.__dict__)
          value_list.append(value)
          #print ("Multi done", data_name, type(value), value)
          multi_line = False
        continue
      mo = re_AeqB.match(line)
      if mo:
        if data_name != mo.group(1):
          print (filename, ": Parse error for", data_name)
          continue
        value = mo.group(2).strip()
        expln = mo.group(3).strip()
        if value[0] in ('"', "'"):
          value = value[1:-1]
        elif value[0] == '{':		# item is a dictionary
          if self._multi_count(value) == 0:	# dictionary is complete
            value = eval(value, conf.__dict__)
            #print ("Single", data_name, type(value), value)
          else:
            multi_line = True
            #print ("Start multi", data_name, type(value), value)
            continue
        elif value[0] == '[':		# item is a list
          if self._multi_count(value) == 0:	# list is complete
            value = eval(value, conf.__dict__)
            #print ("Single", data_name, type(value), value)
          else:
            multi_line = True
            #print ("Start multi", data_name, type(value), value)
            continue
        if expln:
          value_list.append("%s # %s" % (value, expln))
        else:
          value_list.append(value)
      elif line[0:2] == '# ':
        section_data[-1][3] = section_data[-1][3] + line[2:] + ' '
    fp.close()
  def _multi_count(self, value):
    char_start = value[0]
    if char_start == '{':
      char_end = '}'
    elif char_start == '[':
      char_end = ']'
    count = 0
    for ch in value:
      if ch == char_start:
        count += 1
      elif ch == char_end:
        count -= 1
    return count

class xxConfigHelp(wx.html.HtmlWindow):	# The "Help with Radios" first-level page
  """Create the help screen for the configuration tabs."""
  def __init__(self, parent):
    wx.html.HtmlWindow.__init__(self, parent, -1, size=(win_width, 100))
    if "gtk2" in wx.PlatformInfo:
      self.SetStandardFonts()
    self.SetFonts("", "", [10, 12, 14, 16, 18, 20, 22])
    self.SetBackgroundColour(parent.bg_color)
    # read in text from file help_conf.html in the directory of this module
    self.LoadFile('help_conf.html')

class QPowerMeterCalibration(wx.Frame):
  """Create a window to enter the power output and corresponding ADC value AIN1/2"""
  def __init__(self, parent, local_names):
    self.parent = parent
    self.local_names = local_names
    self.table = []	# calibration table: list of [ADC code, power watts]
    try:	# may be missing in wxPython 2.x
      wx.Frame.__init__(self, application.main_frame, -1, "Power Meter Calibration",
         pos=(50, 100), style=wx.CAPTION|wx.FRAME_FLOAT_ON_PARENT)
    except AttributeError:
      wx.Frame.__init__(self, application.main_frame, -1, "Power Meter Calibration",
         pos=(50, 100), style=wx.CAPTION)
    panel = wx.Panel(self)
    self.MakeControls(panel)
    self.Show()
  def MakeControls(self, panel):
    charx = panel.GetCharWidth()
    tab1 = charx * 5
    y = 20
    # line 1
    txt = wx.StaticText(panel, -1, 'Name for new calibration table', pos=(tab1, y))
    w, h = txt.GetSize().Get()
    tab2 = tab1 + w + tab1 // 2
    self.cal_name = wx.TextCtrl(panel, -1, pos=(tab2, h), size=(charx * 16, h * 13 // 10))
    y += h * 3
    # line 2
    txt = wx.StaticText(panel, -1, 'Measured power level in watts', pos=(tab1, y))
    self.cal_power = wx.TextCtrl(panel, -1, pos=(tab2, y), size=(charx * 16, h * 13 // 10))
    x = tab2 + charx * 20
    add = QuiskPushbutton(panel, self.OnBtnAdd, "Add to Table")
    add.SetPosition((x, y - h * 3 // 10))
    add.SetColorGray()
    ww, hh = add.GetSize().Get()
    width = x + ww + tab1
    y += h * 3
    # line 3
    sv = QuiskPushbutton(panel, self.OnBtnSave, "Save")
    sv.SetColorGray()
    cn = QuiskPushbutton(panel, self.OnBtnCancel, "Cancel")
    cn.SetColorGray()
    w, h = cn.GetSize().Get()
    sv.SetPosition((width // 4, y))
    cn.SetPosition((width - width // 4 - w, y))
    y += h * 12 // 10
    # help text at bottom
    wx.StaticText(panel, -1, '1. Attach a 50 ohm load and power meter to the antenna connector.', pos=(tab1, y))
    w, h = txt.GetSize().Get()
    h = h * 12 // 10
    y += h
    wx.StaticText(panel, -1, '2. Use the Spot button to transmit at a very low power.', pos=(tab1, y))
    y += h
    wx.StaticText(panel, -1, '3. Enter the measured power in the box above and press "Add to Table".', pos=(tab1, y))
    y += h
    wx.StaticText(panel, -1, '4. Increase the power a small amount and repeat step 3.', pos=(tab1, y))
    y += h
    wx.StaticText(panel, -1, '5. Increase power again and repeat step 3.', pos=(tab1, y))
    y += h
    wx.StaticText(panel, -1, '6. Keep adding measurements to the table until you reach full power.', pos=(tab1, y))
    y += h
    wx.StaticText(panel, -1, '7. Ten or twelve measurements should be enough. Then press "Save".', pos=(tab1, y))
    y += h
    wx.StaticText(panel, -1, 'To delete a table, save a table with zero measurements.', pos=(tab1, y))
    y += h * 2
    self.SetClientSize(wx.Size(width, y))
  def OnBtnCancel(self, event=None):
    self.parent.ChangePMcalFinished(None, None)
    self.Destroy()
  def OnBtnSave(self, event):
    name = self.cal_name.GetValue().strip()
    if not name:
      dlg = wx.MessageDialog(self,
        'Please enter a name for the new calibration table.',
        'Missing Name', wx.OK|wx.ICON_ERROR)
      dlg.ShowModal()
      dlg.Destroy()
    elif name in conf.power_meter_std_calibrations:		# known calibration names from the config file
      dlg = wx.MessageDialog(self,
        'That name is reserved. Please enter a different name.',
        'Reserved Name', wx.OK|wx.ICON_ERROR)
      dlg.ShowModal()
      dlg.Destroy()
    elif name in self.local_names:
      if self.table:
        dlg = wx.MessageDialog(self,
          'That name exists. Replace the existing table?',
          'Replace Table', wx.OK|wx.CANCEL|wx.ICON_EXCLAMATION)
        ret = dlg.ShowModal()
        dlg.Destroy()
        if ret == wx.ID_OK:
          self.parent.ChangePMcalFinished(name, self.table)
          self.Destroy()
      else:
        dlg = wx.MessageDialog(self,
          'That name exists but the table is empty. Delete the existing table?.',
          'Delete Table', wx.OK|wx.CANCEL|wx.ICON_EXCLAMATION)
        ret = dlg.ShowModal()
        dlg.Destroy()
        if ret == wx.ID_OK:
          self.parent.ChangePMcalFinished(name, None)
          self.Destroy()
    else:
      self.parent.ChangePMcalFinished(name, self.table)
      self.Destroy()
  def OnBtnAdd(self, event):
    power = self.cal_power.GetValue().strip()
    self.cal_power.Clear()
    try:
      power = float(power)
    except:
      dlg = wx.MessageDialog(self, 'Missing or bad measured power.', 'Error in Power', wx.OK|wx.ICON_ERROR)
      dlg.ShowModal()
      dlg.Destroy()
    else:
      ## Convert measured voltage to power
      #power *= 6.388
      #power = power**2 / 50.0
      fwd = application.Hardware.hermes_fwd_power
      rev = application.Hardware.hermes_rev_power
      if fwd >= rev:
        self.table.append([fwd, power])		# Item must use lists; sort() will fail with mixed lists and tuples
      else:
        self.table.append([rev, power])

## Note: The amplitude/phase adjustments have ideas provided by Andrew Nilsson, VK6JBL.
## October 2020: changed to make RxTx frequency and VFO two independent variables.
## June 2024: Added a Grid control and automatic receive measurement.
class QAdjustPhase(wx.Frame):
  """Create a window with amplitude and phase adjustment controls"""
  def __init__(self, parent, width, rx_tx):
    self.rx_tx = rx_tx		# Must be "rx" or "tx"
    if rx_tx == 'tx':
      self.is_tx = 1
      t = "Adjust Sound Card Transmit Amplitude and Phase"
    else:
      self.is_tx = 0
      t = "Adjust Sound Card Receive Amplitude and Phase"
    wx.Frame.__init__(self, application.main_frame, -1, t, pos=(50, 100), style=wx.CAPTION|wx.RESIZE_BORDER)
    self.client_width = application.screen_width * 5 // 10 
    self.SetClientSize((self.client_width, application.screen_height * 5 // 10))
    self.new_amplitude = self.new_phase = 0.0
    self.new_tune = 0
    self.bandAmplPhase = copy.deepcopy(application.bandAmplPhase)
    self.new_cell = None
    self.dirty = False
    self.manual = True
    self.panel = wx.Panel(self)
    self.MakeControls()
    self.Redraw()
    self.Show()
    self.Bind(wx.EVT_CLOSE, self.OnBtnExit)
    QS.softrock_corrections(1)
    QS.set_ampl_phase(0.0, 0.0, self.is_tx)
    self.grid.GoToCell(0, 2)
  def MakeControls(self):		# Make controls for phase/amplitude adjustment
    panel = self.panel
    sizer = wx.BoxSizer(wx.VERTICAL)
    panel.SetSizer(sizer)
    sl_max = application.screen_width * 4 // 10		# maximum +/- value for slider
    self.ampl_scale = float(conf.rx_max_amplitude_correct) / sl_max
    self.phase_scale = float(conf.rx_max_phase_correct) / sl_max
    main_font = wx.Font(conf.default_font_size, wx.FONTFAMILY_SWISS, wx.NORMAL,
          wx.FONTWEIGHT_NORMAL, False, conf.quisk_typeface)
    self.SetFont(main_font)
    panel.SetFont(main_font)
    charx = self.GetCharWidth()
    chary = self.GetCharHeight()
    # Create the grid heading and Destroy button
    self.cell_amph = wx.StaticText(panel, -1, "")
    font = wx.Font(conf.default_font_size, wx.FONTFAMILY_SWISS, wx.NORMAL,
          wx.FONTWEIGHT_BOLD, False, conf.quisk_typeface)
    self.cell_amph.SetFont(font)
    self.cell_amph.SetLabel("Cell frequency %s, gain %.3f, phase %.3f\u00B0      " % (FreqFormatter(144000000), -1.123, -12.456))
    dv = wx.lib.buttons.GenButton(panel, label="  Destroy  ")
    dv.SetBezelWidth(3)
    dv.SetBackgroundColour("#DDD")
    dv.Bind(wx.EVT_BUTTON, self.OnBtnDestroy)
    hbox = wx.BoxSizer(wx.HORIZONTAL)
    sizer.Add(hbox, flag=wx.BOTTOM|wx.EXPAND, border=chary * 3 // 10)
    hbox.Add(self.cell_amph, flag=wx.ALL|wx.ALIGN_CENTER_VERTICAL, border=chary // 2)
    hbox.Add(dv, flag=wx.LEFT|wx.RIGHT|wx.ALIGN_CENTER_VERTICAL, border=charx * 5)
    # Create the grid
    self.grid = wx.grid.Grid(panel, style=wx.BORDER_SIMPLE)
    self.grid.CreateGrid(4, 3)
    self.grid.HideRowLabels()
    self.selected_row = 0
    self.selected_col = 0
    self.cell_bg_color = self.grid.GetDefaultCellBackgroundColour()
    self.grid.SetDefaultCellFont(main_font)
    width, h = self.GetTextExtent("987-24 000")
    self.grid.SetDefaultColSize(width, True)
    self.grid.EnableEditing(False)
    self.grid.SetColLabelValue(0, "Band")
    self.grid.SetColLabelValue(1, "VFO")
    self.grid.SetColLabelValue(2, "Tune")
    self.grid.SetColSize(0, width // 2)
    self.grid.SetColSize(1, width * 14 // 10)
    num_tune = (self.client_width - width * 2) // width
    #print ("num_tune", num_tune)
    for col in range(3, num_tune + 2):
      self.grid.AppendCols()
      self.grid.SetColLabelValue(col, "Tune")
    #self.grid.Bind(wx.grid.EVT_GRID_CELL_LEFT_CLICK, self.OnGridLeftClick)
    self.grid.Bind(wx.grid.EVT_GRID_SELECT_CELL, self.OnGridSelectCell)
    gbox = wx.BoxSizer(wx.VERTICAL)
    sizer.Add(gbox, proportion=1, flag=wx.LEFT|wx.RIGHT|wx.EXPAND, border=charx)
    gbox.Add(self.grid, proportion=1)
    # Create the New Cell text, Manual/Measure, AddCell button
    sbs = wx.StaticBoxSizer(wx.VERTICAL, panel, " New Cell")
    sizer.Add(sbs, flag=wx.EXPAND|wx.LEFT|wx.TOP|wx.RIGHT|wx.BOTTOM, border=charx*3)
    self.add_cell_text = wx.StaticText(panel, -1, "Band")
    self.add_cell_text.SetFont(main_font)
    sbs.Add(self.add_cell_text, flag=wx.LEFT|wx.TOP, border=charx)
    box0 = wx.BoxSizer(wx.HORIZONTAL)
    sbs.Add(1, charx)
    sbs.Add(box0, flag=wx.EXPAND)
    rb_manual = wx.RadioButton(panel, -1, "Manual adjustment", style=wx.RB_GROUP)
    self.Bind(wx.EVT_RADIOBUTTON, self.OnBtnManMeasure)
    rb_measure = wx.RadioButton(panel, -1, "Measure")
    if self.is_tx:
      rb_manual.Enable(False)
      rb_measure.Enable(False)
    b = wx.lib.buttons.GenButton(panel, label="  Add cell  ")
    b.Bind(wx.EVT_BUTTON, self.OnBtnAddCell)
    b.SetBezelWidth(3)
    b.SetBackgroundColour("#DDD")
    box0.Add(rb_manual, flag=wx.LEFT|wx.ALIGN_CENTER_VERTICAL, border=charx)
    box0.Add(rb_measure, flag=wx.LEFT|wx.ALIGN_CENTER_VERTICAL, border=charx*5)
    box0.Add(b, flag=wx.LEFT, border=charx*5)
    # Create the amplitude slider controls
    box1 = wx.BoxSizer(wx.HORIZONTAL)
    box2 = wx.BoxSizer(wx.HORIZONTAL)
    sbs.Add(box1, flag=wx.EXPAND)
    sbs.Add(box2, flag=wx.EXPAND)
    # ST_ELLIPSIZE_END needed to work around a layout bug
    fine = wx.StaticText(panel, -1, 'Gain Fine', style=wx.ST_ELLIPSIZE_END)
    coarse = wx.StaticText(panel, -1, 'Gain Coarse', style=wx.ST_ELLIPSIZE_END)
    box1.Add(fine, flag=wx.LEFT|wx.ALIGN_CENTER_VERTICAL, border=charx*3)
    box2.Add(coarse, flag=wx.LEFT|wx.ALIGN_CENTER_VERTICAL, border=charx*3)
    self.ampl1 = wx.Slider(panel, -1, 0, -sl_max, sl_max)
    self.ampl2 = wx.Slider(panel, -1, 0, -sl_max, sl_max)
    box1.Add(self.ampl1, flag=wx.LEFT, proportion=1, border=0)
    box2.Add(self.ampl2, flag=wx.LEFT, proportion=1, border=0)
    self.ampl1.Bind(wx.EVT_SCROLL, self.OnAmpl1)
    self.ampl2.Bind(wx.EVT_SCROLL, self.OnAmpl2)
    # Create the phase slider controls
    box3 = wx.BoxSizer(wx.HORIZONTAL)
    box4 = wx.BoxSizer(wx.HORIZONTAL)
    sbs.Add(box3, flag=wx.EXPAND)
    sbs.Add(box4, flag=wx.EXPAND)
    fine = wx.StaticText(panel, -1, 'Phase Fine', style=wx.ST_ELLIPSIZE_END)
    coarse = wx.StaticText(panel, -1, 'Phase Coarse', style=wx.ST_ELLIPSIZE_END)
    box3.Add(fine, flag=wx.LEFT|wx.ALIGN_CENTER_VERTICAL, border=charx*3)
    box4.Add(coarse, flag=wx.LEFT|wx.ALIGN_CENTER_VERTICAL, border=charx*3)
    self.phas1 = wx.Slider(panel, -1, 0, -sl_max, sl_max)
    self.phas2 = wx.Slider(panel, -1, 0, -sl_max, sl_max)
    box3.Add(self.phas1, flag=wx.LEFT, proportion=1, border=0)
    box4.Add(self.phas2, flag=wx.LEFT, proportion=1, border=0)
    self.phas1.Bind(wx.EVT_SCROLL, self.OnPhase1)
    self.phas2.Bind(wx.EVT_SCROLL, self.OnPhase2)
    # Create the button row
    btnbox = wx.BoxSizer(wx.HORIZONTAL)
    sizer.Add(btnbox, flag=wx.EXPAND)
    btns = []
    btnbox.Add(1, 1, proportion=10)
    b = wx.lib.buttons.GenButton(panel, label="  Save  ")
    btnbox.Add(b)
    btnbox.Add(1, 1, proportion=5)
    btns.append(b)
    size = b.GetSize()
    b.Bind(wx.EVT_BUTTON, self.OnBtnSave)
    b = wx.lib.buttons.GenButton(panel, label="  Exit  ")
    btnbox.Add(b)
    btnbox.Add(1, 1, proportion=5)
    btns.append(b)
    b.Bind(wx.EVT_BUTTON, self.OnBtnExit)
    b = wx.lib.buttons.GenButton(panel, label="  Help  ")
    btnbox.Add(b)
    btnbox.Add(1, 1, proportion=10)
    btns.append(b)
    b.Bind(wx.EVT_BUTTON, self.OnBtnHelp)
    #b.Bind(wx.EVT_BUTTON, self.OnGraphData)
    for b in btns:
      b.SetBezelWidth(3)
      b.SetBackgroundColour("#DDD")
      b.SetSize(size)
    sizer.Add(charx, chary)
  def Redraw(self):
    self.grid.ClearGrid()
    # Print available data cells
    row = 0
    bands = []
    self.cell_dict = {}
    for band in self.bandAmplPhase:
      if band != "Version" and self.rx_tx in self.bandAmplPhase[band]:
        bands.append(band)
    bands.sort(reverse=True)
    for band in bands:
      vfos_tunes = self.bandAmplPhase[band][self.rx_tx]
      if vfos_tunes:
        if self.grid.GetNumberRows() <= row:
          self.grid.AppendRows()
        self.grid.SetCellValue(row, 0, band)
        self.grid.SetCellAlignment(row, 0, wx.ALIGN_CENTER, wx.ALIGN_CENTER)
        for vfo, tunes in vfos_tunes:
          if self.grid.GetNumberRows() <= row:
            self.grid.AppendRows()
          self.cell_dict[(row, 0)] = band, vfo, 0.0
          self.grid.SetCellValue(row, 1, FreqFormatter(vfo))
          self.grid.SetCellAlignment(row, 1, wx.ALIGN_RIGHT, wx.ALIGN_CENTER)
          col = 2
          for tune, ampl, phase in tunes:
            if self.grid.GetNumberCols() <= col:
              self.grid.AppendCols()
              self.grid.SetColLabelValue(col, "Tune")
            self.grid.SetCellValue(row, col, FreqFormatter(tune))
            self.grid.SetCellAlignment(row, col, wx.ALIGN_RIGHT, wx.ALIGN_CENTER)
            self.cell_dict[(row, col)] = tune, ampl, phase
            if (vfo, tune) == self.new_cell:
              self.grid.GoToCell(row, col)
            col += 1
          row += 1
    self.new_cell = None
    self.grid.ForceRefresh()
  def SetNewCellText(self):
    vfo = FreqFormatter(application.VFO)
    self.add_cell_text.SetLabel("Band %s,  Center (VFO) %s,  Tune %d,  Gain %5.3f,  Phase %5.3f\u00B0" % (
          application.lastBand, vfo, self.new_tune, self.new_amplitude + 1.0, self.new_phase))
  def SlowHeartBeat(self):
    if self.manual:
      self.new_tune = application.txFreq
    else:
      self.new_tune, gain, phase = QS.softrock_corrections(2)
      ampl = gain - 1.0
      self.new_amplitude = ampl
      phase *= 57.2958
      self.new_phase = phase
      self.PosAmpl(ampl)
      self.PosPhase(phase)
    self.SetNewCellText()
  def OnGridSelectCell(self, event=None):
    self.grid.SetCellBackgroundColour(self.selected_row, self.selected_col, self.cell_bg_color)
    if event:
      event.Skip()
      row = self.selected_row = event.GetRow()
      col = self.selected_col = event.GetCol()
    else:
      row = self.selected_row
      col = self.selected_col
    self.grid.SetCellBackgroundColour(row, col, '#FEA')
    self.grid.ForceRefresh()
    band, vfo, z = self.cell_dict.get((row, 0), ('', None, 0))
    tune, am, ph = self.cell_dict.get((row, col), (None, None, None))
    if not band or vfo is None:
      self.cell_amph.SetLabel("Cell is empty")
    elif col == 0:
      self.cell_amph.SetLabel("Band %s" % band)
    elif col == 1:
      self.cell_amph.SetLabel("Center (VFO) frequency %s" % FreqFormatter(vfo))
    elif tune is None:
      self.cell_amph.SetLabel("Cell is empty")
    else:
      self.cell_amph.SetLabel("Cell frequency %s, gain %.3f, phase %.3f\u00B0" % (FreqFormatter(vfo + tune), am + 1.0, ph))
  def PosAmpl(self, ampl):	# set pos1, pos2 for amplitude
    pos2 = round(ampl / self.ampl_scale)
    remain = ampl - pos2 * self.ampl_scale
    pos1 = round(remain / self.ampl_scale * 50.0)
    self.ampl1.SetValue(pos1)
    self.ampl2.SetValue(pos2)
  def PosPhase(self, phase):	# set pos1, pos2 for phase
    pos2 = round(phase / self.phase_scale)
    remain = phase - pos2 * self.phase_scale
    pos1 = round(remain / self.phase_scale * 50.0)
    self.phas1.SetValue(pos1)
    self.phas2.SetValue(pos2)
  def OnAmpl2(self, event):		# re-center the fine slider when the coarse slider is adjusted
    ampl = self.ampl_scale * self.ampl1.GetValue() / 50.0 + self.ampl_scale * self.ampl2.GetValue()
    self.PosAmpl(ampl)
    self.OnAmpl1(event)
  def OnAmpl1(self, event):
    ampl = self.ampl_scale * self.ampl1.GetValue() / 50.0 + self.ampl_scale * self.ampl2.GetValue()
    self.new_amplitude = ampl
    self.SetNewCellText()
    QS.set_ampl_phase(self.new_amplitude, self.new_phase, self.is_tx)
  def OnPhase2(self, event):	# re-center the fine slider when the coarse slider is adjusted
    phase = self.phase_scale * self.phas1.GetValue() / 50.0 + self.phase_scale * self.phas2.GetValue()
    self.PosPhase(phase)
    self.OnPhase1(event)
  def OnPhase1(self, event):
    phase = self.phase_scale * self.phas1.GetValue() / 50.0 + self.phase_scale * self.phas2.GetValue()
    self.new_phase = phase
    self.SetNewCellText()
    QS.set_ampl_phase(self.new_amplitude, self.new_phase, self.is_tx)
  def OnBtnAddCell(self, event):
    band = application.lastBand
    vfo = application.VFO
    self.new_cell = (vfo, self.new_tune)
    ampl = self.new_amplitude
    phase = self.new_phase
    tune = self.new_tune
    if band not in self.bandAmplPhase:
      self.bandAmplPhase[band] = {}
    if self.rx_tx not in self.bandAmplPhase[band]:
      self.bandAmplPhase[band][self.rx_tx] = []
    vfos_tunes = self.bandAmplPhase[band][self.rx_tx]
    for i in range(0, len(vfos_tunes)):
      vfo_tunes = vfos_tunes[i]
      if vfo_tunes[0] == vfo:
        tunes = vfo_tunes[1]
        for j in range(0, len(tunes)):
          if tunes[j][0] == tune:
            tunes[j] = [tune, ampl, phase]
            break
        else:
          tunes.append([tune, ampl, phase])
          tunes.sort()
        self.dirty = True
        self.Redraw()
        return
    vfos_tunes.append([vfo, [[tune, ampl, phase]]])
    vfos_tunes = vfos_tunes.sort()
    self.dirty = True
    self.Redraw()
    self.grid.ClearSelection()
  def OnBtnDestroy(self, event):
    # Create a list of cells to be deleted
    col_row = []
    # print("Rows", self.grid.GetSelectedRows())
    # print("Cols", self.grid.GetSelectedCols())
    # print("Cells", self.grid.GetSelectedCells())
    # print("Left", self.grid.GetSelectionBlockTopLeft())
    # print("Right", self.grid.GetSelectionBlockBottomRight())
    #
    # delete the cell at the grid cursor
    row = self.grid.GetGridCursorRow()
    col = self.grid.GetGridCursorCol()
    cr = col, row
    if cr not in col_row:
      col_row.append(cr)
    #rows = self.grid.GetSelectedRows()	# Not used
    # delete whole columns
    cols = self.grid.GetSelectedCols()
    for col in cols:
      for row in range(0, self.grid.GetNumberRows()):
        cr = col, row
        if cr not in col_row:
          col_row.append(cr)
    # delete cells
    cells = self.grid.GetSelectedCells()
    for cell in cells:
      row, col = cell.Get()
      cr = col, row
      if cr not in col_row:
        col_row.append(cr)
    # delete blocks
    top_left_list = self.grid.GetSelectionBlockTopLeft()
    bottom_right_list = self.grid.GetSelectionBlockBottomRight()
    for i in range(len(top_left_list)):
      row1, col1 = top_left_list[i].Get()
      row2, col2 = bottom_right_list[i].Get()
      for row in range(row1, row2 + 1):
        for col in range(col1, col2 + 1):
          cr = col, row
          if cr not in col_row:
            col_row.append(cr)
    col_row.sort(reverse=True)
    changed = False
    for col, row in col_row:
      try:
        band, vfo, z = self.cell_dict[(row, 0)]
        vfos_tunes = self.bandAmplPhase[band][self.rx_tx]
      except:
        continue
      if col in (0, 1):		# Destroy row
        for i in range(0, len(vfos_tunes)):
          if vfos_tunes[i][0] == vfo:
            del vfos_tunes[i]
            changed = True
            break
      else:		# Destroy cell
        try:
          tune, am, ph = self.cell_dict[(row, col)]
        except:
          continue
        for i in range(0, len(vfos_tunes)):
          if vfos_tunes[i][0] != vfo:
            continue
          tunes = vfos_tunes[i][1]
          for j in range(0, len(tunes)):
            if tunes[j][0] == tune:
              del tunes[j]
              changed = True
              break
    if changed:
      self.dirty = True
      self.Redraw()
      self.selected_row = self.grid.GetGridCursorRow()
      self.selected_col = self.grid.GetGridCursorCol()
      self.OnGridSelectCell()
    self.grid.ClearSelection()
  def OnBtnManMeasure(self, event):
    self.manual = event.GetEventObject().GetLabel()[0:3] == "Man"
    self.ampl1.Enable(self.manual)
    self.ampl2.Enable(self.manual)
    self.phas1.Enable(self.manual)
    self.phas2.Enable(self.manual)
    if self.manual:
      QS.softrock_corrections(1)
      QS.set_ampl_phase(self.new_amplitude, self.new_phase, self.is_tx)
    else:
      QS.softrock_corrections(2)
      QS.set_ampl_phase(0.0, 0.0, self.is_tx)
  def OnBtnSave(self, event):
    application.bandAmplPhase = copy.deepcopy(self.bandAmplPhase)
    self.dirty = False
  def OnBtnExit(self, event):
    #self.OnGraphData(None)
    if self.dirty:
      dlg = wx.MessageDialog(self,
        "Your changes are not saved. Do you want to save them?", "Changes Were Made",
        wx.OK|wx.CANCEL|wx.CANCEL_DEFAULT|wx.ICON_WARNING)
      dlg.SetOKLabel("Discard Changes")
      ret = dlg.ShowModal()
      dlg.Destroy()
      if ret == wx.ID_CANCEL:
        return
    QS.softrock_corrections(0)
    ampl, phase = application.GetAmplPhase(self.rx_tx)
    QS.set_ampl_phase(ampl, phase, self.is_tx)
    application.w_phase = None
    self.Destroy()
  def OnBtnHelp(self, event=None):
    dlg = wx.MessageDialog(self,
'For manual adjustment attach a signal generator or look at a strong signal in the band. Set the Quisk frequency to the signal frequency. \
Adjust the amplitude and phase sliders to minimize the image. To save the adjustment, press Add Cell. \
For automatic measurement, attach a signal generator and choose some frequencies. Press Add Cell for each one. \
Adjustments must be made for both receive and transmit on each band. \
No changes are made to the table of adjustments until you press Save. \
The maximum slider adjustment range can be changed on the radio Hardware screen. \
For more information, press the main "Help" button, then "Documentation", then "SoftRock".'
    , "Adjustment Help", style=wx.OK)
    dlg.ShowModal()
  def OnGraphData(self, event):
    if not hasattr(self, "VFO"):
      self.VFO = []
      self.Tune = []
      self.Gain = []
      self.Phase = []
    if event:
      self.VFO.append(application.VFO)
      self.Tune.append(self.new_tune)
      self.Gain.append(self.new_amplitude + 1.0)
      self.Phase.append(self.new_phase)
    else:
      for name in ("VFO", "Tune", "Gain", "Phase"):
        print ("%s = [" % name)
        lst = getattr(self, name)
        for i in range(0, len(lst)):
          if name[0] in "VT":
            print("%d," % lst[i])
          else:
            print("%.5f," % lst[i])
        print ("]")

class ListEditDialog(wx.Dialog):	# Display a dialog with a List-Edit control, plus Ok/Cancel
  def __init__(self, parent, title, choice, choices, width):
    wx.Dialog.__init__(self, parent, title=title, style=wx.CAPTION|wx.CLOSE_BOX)
    cancel = wx.Button(self, wx.ID_CANCEL, "Cancel")
    bsize = cancel.GetSize()
    margin = bsize.height
    self.combo = wx.ComboBox(self, -1, choice, pos=(margin, margin), size=(width - margin * 2, -1), choices=choices, style=wx.CB_DROPDOWN)
    y = margin + self.combo.GetSize().height + margin
    x = width - margin * 2 - bsize.width * 2
    x = x // 3
    ok = wx.Button(self, wx.ID_OK, "OK", pos=(margin + x, y))
    cancel.SetPosition((width - margin - x - bsize.width, y))
    self.SetClientSize(wx.Size(width, y + bsize.height * 14 // 10))
  def GetValue(self):
    return self.combo.GetValue()

class RadioNotebook(wx.Notebook):	# The second-level notebook for each radio name
  def __init__(self, parent, radio_name):
    wx.Notebook.__init__(self, parent)
    font = wx.Font(conf.config_font_size, wx.FONTFAMILY_SWISS, wx.NORMAL,
          wx.FONTWEIGHT_NORMAL, False, conf.quisk_typeface)
    self.SetFont(font)
    self.SetBackgroundColour(parent.bg_color)
    self.radio_name = radio_name
    self.pages = []
    self.Bind(wx.EVT_NOTEBOOK_PAGE_CHANGED, self.OnPageChanging)
  def MakePages(self):
    radio_name = self.radio_name
    radio_dict = local_conf.GetRadioDict(radio_name)
    radio_type = radio_dict['hardware_file_type']
    if radio_type == 'SoapySDR':
      page = RadioHardwareSoapySDR(self, radio_name)
    else:
      page = RadioHardware(self, radio_name)
    self.AddPage(page, "Hardware")
    self.pages.append(page)
    page = RadioSound(self, radio_name)
    self.AddPage(page, "Sound")
    self.pages.append(page)
    for section, names in local_conf.sections:
      if section in ('Sound', 'Bands', 'Filters'):		# There is a special page for these sections
        continue
      page = RadioSection(self, radio_name, section, names)
      self.AddPage(page, section)
      self.pages.append(page)
    page = RadioBands(self, radio_name)
    self.AddPage(page, "Bands")
    self.pages.append(page)
    #if "use_rx_udp" in radio_dict and radio_dict["use_rx_udp"] == '10':
    #  page = RadioFilters(self, radio_name)
    #  self.AddPage(page, "Filters")
    #  self.pages.append(page)
  def NewName(self, new_name):
    self.radio_name = new_name
    for page in self.pages:
      page.radio_name = new_name
  def OnPageChanging(self, event):
    global wxpython_gtk3_bug
    event.Skip()
    index = event.GetSelection()
    page = self.GetPage(index)
    if sys.platform != 'win32':		# Work around a bug in wxPython 4.2.1 and gtk3
      w, h = application.main_frame.GetSize()	# Change main window size by +/- one pixel to recalculate sizes
      if wxpython_gtk3_bug:
        wxpython_gtk3_bug = 0
        h += 1
      else:
        wxpython_gtk3_bug = 1
        h -= 1
      application.main_frame.SetSize((w, h))

class ChoiceCombo(wx.Choice):
  text_for_blank = "-blank-"
  def __init__(self, parent, value, choices):
    wx.Choice.__init__(self, parent, choices=choices)
    self.choices = choices[:]
    self.handler = None
    try:
      index = self.choices.index(value)
    except:
      index = 0
    self.Bind(wx.EVT_CHOICE, self.OnChoice)
    self._ChangeItems(index)
  def _ChangeItems(self, index):
    length = len(self.choices)
    if length <= 0:
      self.Enable(False)
    elif length == 1:
      wx.Choice.SetSelection(self, 0)
      self.Enable(False)
    else:
      wx.Choice.SetSelection(self, index)
      self.Enable(True)
    self._ReplaceBlank()
    self.GetValue()
  def _ReplaceBlank(self):
    try:
      n = self.choices.index('')
    except:
      pass
    else:
      self.SetString(n, self.text_for_blank)
  def SetItems(self, lst):
    wx.Choice.SetItems(self, lst)
    self.choices = lst[:]
    self._ChangeItems(0)
  def SetSelection(self, n):
    if 0 <= n < len(self.choices):
      wx.Choice.SetSelection(self, n)
    self.GetValue()
  def SetText(self, text):
    try:
      n = self.choices.index(text)
    except:
      print("Failed to set choice list to", text)
    else:
      wx.Choice.SetSelection(self, n)
    self.GetValue()
  def GetValue(self):
    n = self.GetSelection()
    if n == wx.NOT_FOUND:
      self.value = ''
    else:
      self.value = self.GetString(n)
      if self.value == self.text_for_blank:
        self.value = ''
    return self.value
  def OnChoice(self, event):
    event.Skip()
    old = self.value
    self.GetValue()
    if self.value != old:
      if self.handler:
        self.handler(self)

class ComboCtrl(wxcombo.ComboCtrl):
  def __init__(self, parent, value, choices, no_edit=False):
    self.value = value
    self.choices = choices[:]
    self.handler = None
    self.dirty = False
    try:
      self.bgColor = wx.SystemSettings.GetColour(wx.SYS_COLOUR_LISTBOX)
    except:
      self.bgColor = wx.Colour('#f6f5f4')
    if no_edit:
      wxcombo.ComboCtrl.__init__(self, parent, -1, style=wx.CB_READONLY)
    else:
      wxcombo.ComboCtrl.__init__(self, parent, -1, style=wx.TE_PROCESS_ENTER)
      self.Bind(wx.EVT_KILL_FOCUS, self.OnKillFocus)
      self.Bind(wx.EVT_TEXT, self.OnText)
      self.Bind(wx.EVT_TEXT_ENTER, self.OnTextEnter)
    try:
      self.height = parent.quisk_height
    except:
      self.chary = self.GetCharHeight()
      self.height = self.chary * 14 // 10
    self.SetBackgroundColour(self.bgColor)
    self.Refresh()
    try:
      font = parent.font
    except:
      font = self.GetFont()
    self.ctrl = ListBoxComboPopup(choices, font)
    self.SetPopupControl(self.ctrl)
    self.SetText(value)
    self.SetSizes()
    self.Bind(wx.EVT_COMBOBOX_CLOSEUP, self.OnCloseup)
  def SetItems(self, lst):
    self.ctrl.SetItems(lst)
    self.choices = lst[:]
    self.SetSizes()
  def SetSizes(self):
    charx = self.GetCharWidth()
    wm = charx
    w, h = self.GetTextExtent(self.value)
    if wm < w:
      wm = w
    for ch in self.choices:
      w, h = self.GetTextExtent(ch)
      if wm < w:
        wm = w
    wm += charx * 5
    self.SetSizeHints(wm, self.height, 9999, self.height)
  def SetValue(self, value):
    wxcombo.ComboCtrl.SetValue(self, value)
    self.SetBackgroundColour(self.bgColor)
    self.Refresh()
  def SetSelection(self, n):	# Set text to item in list box. Name conflict with wxcombo.ComboCtrl.
    self.SetBackgroundColour(self.bgColor)
    self.Refresh()
    try:
      text = self.choices[n]
    except IndexError:
      self.SetText('')
      self.value = ''
    else:
      self.ctrl.SetSelection(n)
      self.SetText(text)
      self.value = text
  def OnText(self, event):
    self.dirty = True
    self.SetBackgroundColour('#e0e0ff')
    self.Refresh()
  def OnTextEnter(self, event=None):
    self.SetBackgroundColour(self.bgColor)
    self.Refresh()
    self.dirty = False
    if event:
      event.Skip()
    new = self.GetValue()
    if self.value != new:
      self.value = new
      if self.handler:
        self.handler(self)
  def OnKillFocus(self, event):
    event.Skip()
    if self.dirty:
      self.OnTextEnter(None)
  def OnListbox(self):
    self.Dismiss()
    self.OnTextEnter()
  def OnButtonClick(self):
    wxcombo.ComboCtrl.OnButtonClick(self)
    wxcombo.ComboCtrl.SetSelection(self, 0, 0)
    wxcombo.ComboCtrl.SetInsertionPointEnd(self)
  def OnCloseup(self, event):
    event.Skip()
    wxcombo.ComboCtrl.SetSelection(self, 0, 0)
    wxcombo.ComboCtrl.SetInsertionPointEnd(self)

class ListBoxComboPopup(wxcombo.ComboPopup):
  text_for_blank = "-blank-"
  def __init__(self, choices, font):
    wxcombo.ComboPopup.__init__(self)
    self.choices = choices
    self.font = font
    self.lbox = None
    self.index = None
  def Create(self, parent):
    self.lbox = wx.ListBox(parent, choices=self.choices, style=wx.LB_SINGLE|wx.LB_NEEDED_SB)
    self.lbox.SetFont(self.font)
    self.lbox.Bind(wx.EVT_MOTION, self.OnMotion)
    self.lbox.Bind(wx.EVT_LEFT_DOWN, self.OnLeftDown)
    self._ReplaceBlank()
    return True
  def SetItems(self, lst):
    self.choices = lst[:]
    self.lbox.Set(self.choices)
    self._ReplaceBlank()
  def _ReplaceBlank(self):
    try:
      n = self.choices.index('')
    except:
      pass
    else:
      self.lbox.SetString(n, self.text_for_blank)
  def SetSelection(self, n):
    self.lbox.SetSelection(n)
  def GetStringValue(self):
    try:
      text = self.choices[self.lbox.GetSelection()]
    except IndexError:
      text = ''
    else:
      if text == self.text_for_blank:
        text = ''
    return text
  def GetAdjustedSize(self, minWidth, prefHeight, maxHeight):
    chary = self.lbox.GetCharHeight()
    height = chary * len(self.choices) * 15 // 10 + chary
    if height > prefHeight:
      height = prefHeight
    return (minWidth, height)
  def OnLeftDown(self, event):
    event.Skip()
    self.index = self.lbox.GetSelection()	# index of selected item
    if wxVersion in ('2', '3'):
      self.GetCombo().OnListbox()
    else:
      self.GetComboCtrl().OnListbox()
  def OnMotion(self, event):
    event.Skip()
    item = self.lbox.HitTest(event.GetPosition())
    if item >= 0:
      self.lbox.SetSelection(item)
  def GetControl(self):
    return self.lbox

class QuiskTextCtrl(wx.TextCtrl):
  def __init__(self, parent, text, style):
    wx.TextCtrl.__init__(self, parent, -1, text, style=style)
    self.dirty = False
    self.bgColor = wx.Colour('#f6f5f4')
    #self.Bind(wx.EVT_KILL_FOCUS, self.OnKillFocus)
    if not (style & wx.TE_READONLY):
      self.Bind(wx.EVT_TEXT, self.OnText)
      self.Bind(wx.EVT_TEXT_ENTER, self.OnTextEnter)
  def SetValue(self, value):
    wx.TextCtrl.SetValue(self, value)
    self.SetBackgroundColour(self.bgColor)
    self.Refresh()
    self.dirty = False
  def OnText(self, event):
    event.Skip()
    self.dirty = True
    self.SetBackgroundColour('#e0e0ff')
    self.Refresh()
  def OnTextEnter(self, event=None):
    if event:
      event.Skip()
    self.SetBackgroundColour(self.bgColor)
    self.Refresh()
    self.dirty = False
  #def OnKillFocus(self, event):
  #  event.Skip()
  #  if self.dirty:
  #    self.OnTextEnter(None)

class QuiskControl(wx.Control):
  def __init__(self, parent, text, height, pos=wx.DefaultPosition, style=0):
    wx.Control.__init__(self, parent, -1, pos=pos, style=style)
    self.text = text
    self.handler = None
    self.value = ''
    if wxVersion in ('2', '3'):
      self.SetBackgroundStyle(wx.BG_STYLE_CUSTOM)
    else:
      self.SetBackgroundStyle(wx.BG_STYLE_PAINT)
    from quisk_widgets import button_font
    self.SetFont(button_font)
    w, h = self.GetTextExtent(text)
    self.SetSizeHints(w + height * 3, height, 999, height)
    self.bg_brush = wx.Brush('#dcdcdc')
    self.black_brush = wx.Brush(wx.Colour(0x000000))
    self.text_color = wx.Colour(0x000000)
    self.black_pen = wx.Pen(self.text_color)
    self.no_pen = wx.Pen(self.text_color, style=wx.PENSTYLE_TRANSPARENT)
    self.Bind(wx.EVT_PAINT, self.OnPaint)
    self.Bind(wx.EVT_LEFT_DOWN, self.OnLeftDown)
    self.MakeMenu()
  def OnPaint(self, event):
    dc = wx.AutoBufferedPaintDC(self)
    dc.SetBackground(self.bg_brush)
    dc.SetTextForeground(self.text_color)
    dc.Clear()
    self._Glyph1(dc)
  def _Glyph1(self, dc):
    win_w, win_h = self.GetClientSize()
    x = win_w - win_h
    l = win_h  * 20 // 100
    if l < 3:
      l = 3
    y = win_h // 2 - 2
    pen = wx.Pen('#555555', 1)
    dc.SetPen(pen)
    points = [(0, 0), (l, l), (l + 1, l), (l * 2 + 2, -1)]
    dc.DrawLines(points, x, y)
    pen = wx.Pen('#888888', 1)
    dc.SetPen(pen)
    points = [(1, 0), (l, l - 1), (l + 1, l - 1), (l * 2 + 1, -1)]
    dc.DrawLines(points, x, y)
    w, h = dc.GetTextExtent(self.text)
    x = (win_w - win_h - w) // 2
    if x < 0:
      x = 0
    y = (win_h - h) // 2
    if y < 0:
      y = 0
    dc.DrawText(self.text, x, y)
  def _Glyph2(self, dc):	# Not used
    dc.SetBrush(self.black_brush)
    win_w, win_h = self.GetClientSize()
    spacing = 2
    rect_h = (win_h - spacing * 4) // 3
    if rect_h % 2 == 0:		# Make rect_h an odd number
      rect_h -= 1
    spacing = (win_h - rect_h * 3) // 4
    width = rect_h * 25 // 10
    x0 = win_w - (rect_h + 2 + width + 3)
    y0 = (win_h - rect_h * 3 - spacing * 2) // 2
    x = (x0 - dc.GetCharWidth()) // 2
    y = (win_h - dc.GetCharHeight()) // 2
    dc.DrawText(u"\u2BA9", x, y)
    for y in range(3):
      dc.SetPen(self.no_pen)
      dc.DrawRectangle(x0, y0, rect_h, rect_h)
      x = x0 + rect_h + 2
      y = y0 + rect_h // 2
      dc.SetPen(self.black_pen)
      dc.DrawLine(x, y, x + width, y)
      y0 += rect_h + spacing
  def MakeMenu(self):
    self.menu = wx.Menu()
  def OnLeftDown(self, event):
    pos = wx.Point(0, 0)
    self.PopupMenu(self.menu, pos)

class MidiButton(QuiskControl):
  def __init__(self, parent, text, height, pos=wx.DefaultPosition, style=0):
    QuiskControl.__init__(self, parent, text, height, pos, style)
  def MakeMenu(self):
    main_names = []
    bands = wx.Menu()
    filters = wx.Menu()
    modes = wx.Menu()
    screens = wx.Menu()
    for idName in application.idName2Button:
      if not idName:
        item = None
      elif idName in conf.BandList or idName in ("Audio", "Time"):
        item = bands.Append(-1, idName)
      elif idName[0:7] == 'Filter ':
        item = filters.Append(-1, idName)
      elif idName in ("CW U/L", "CWL", "CWU", "SSB U/L", "LSB", "USB", "AM", "FM", "DGT",
                      "DGT-U", "DGT-L", "DGT-FM", "DGT-IQ", "FDV", "FDV-U", "IMD", ):
        item = modes.Append(-1, idName)
      elif idName in ("Graph", "GraphP1", "GraphP2", "WFall", "Scope", "Config", "Audio FFT", "Bscope", "RX Filter", "Help"):
        item = screens.Append(-1, idName)
      else:
        item = None
        main_names.append(idName)
      if item:
        self.Bind(wx.EVT_MENU, self.OnMenu, item)
    main_names.sort()
    self.menu = wx.Menu()
    main1 = wx.Menu()
    main2 = wx.Menu()
    for name in main_names:
      if name[0].upper() in "ABCDEFGHIJKLMN":
        item = main1.Append(-1, name)
      else:
        item = main2.Append(-1, name)
      self.Bind(wx.EVT_MENU, self.OnMenu, item)
    self.menu.AppendSubMenu(main1, "Button A-N")
    self.menu.AppendSubMenu(main2, "Button O-Z")
    item = self.menu.AppendSubMenu(bands, "Bands")
    if bands.GetMenuItemCount() <= 0:
      self.menu.Enable(item.GetId(), False)
    item = self.menu.AppendSubMenu(filters, "Filters")
    if filters.GetMenuItemCount() <= 0:
      self.menu.Enable(item.GetId(), False)
    item = self.menu.AppendSubMenu(modes, "Modes")
    if modes.GetMenuItemCount() <= 0:
      self.menu.Enable(item.GetId(), False)
    item = self.menu.AppendSubMenu(screens, "Screens")
    if screens.GetMenuItemCount() <= 0:
      self.menu.Enable(item.GetId(), False)
  def OnMenu(self, event):
    self.value = self.menu.GetLabel(event.GetId())
    if self.handler:
      self.handler(self)

class MidiKnob(QuiskControl):
  def __init__(self, parent, text, height, pos=wx.DefaultPosition, style=0):
    QuiskControl.__init__(self, parent, text, height, pos, style)
  def MakeMenu(self):
    self.menu = wx.Menu()
    for name in application.midiControls:
      ctrl, func = application.midiControls[name]
      if name != "Tune" and (not ctrl or not func):
        continue
      item = self.menu.Append(-1, name)
      self.Bind(wx.EVT_MENU, self.OnMenu, item)
  def OnMenu(self, event):
    self.value = self.menu.GetLabel(event.GetId())
    if self.handler:
      self.handler(self)

class MidiJogWheel(QuiskControl):
  def __init__(self, parent, text, height, pos=wx.DefaultPosition, style=0):
    self.jog_direction = "+"
    self.jog_speed = "5"
    self.MakeText()
    QuiskControl.__init__(self, parent, self.text, height, pos, style)
  def MakeText(self):
    self.text = "Jog Wheel  %s%s" % (self.jog_direction, self.jog_speed)
  def MakeMenu(self):
    self.menu = wx.Menu()
    self.direc_menu = wx.Menu()
    item = self.direc_menu.AppendRadioItem(-1, "Move +")
    self.Bind(wx.EVT_MENU, self.OnDirecMenu, item)
    item = self.direc_menu.AppendRadioItem(-1, "Move -")
    self.Bind(wx.EVT_MENU, self.OnDirecMenu, item)
    self.menu.AppendSubMenu(self.direc_menu, "Direction")
    self.speed_menu = wx.Menu()
    for i in range(10):
      item = self.speed_menu.AppendRadioItem(-1, "Speed %d" % i)
      self.Bind(wx.EVT_MENU, self.OnSpeedMenu, item)
      if i == 5:
        item.Check()
    self.menu.AppendSubMenu(self.speed_menu, "Speed")
    controls = wx.Menu()
    for name in application.midiControls:
      ctrl, func = application.midiControls[name]
      if name != "Tune" and (not ctrl or not func):
        continue
      item = controls.Append(-1, name)
      self.Bind(wx.EVT_MENU, self.OnMenu, item)
    self.menu.AppendSubMenu(controls, "Name")
  def OnMenu(self, event):
    value = self.menu.GetLabel(event.GetId())
    self.value = "%s %s%s" % (value, self.jog_direction, self.jog_speed)
    if self.handler:
      self.handler(self)
  def OnDirecMenu(self, event):
    value = self.menu.GetLabel(event.GetId())
    self.jog_direction = value[-1]
    self.MakeText()
    self.Refresh()
    if self.handler:
      self.handler(self, "%s%s" % (self.jog_direction, self.jog_speed))
  def OnSpeedMenu(self, event):
    value = self.menu.GetLabel(event.GetId())
    self.jog_speed = value[-1]
    self.MakeText()
    self.Refresh()
    if self.handler:
      self.handler(self, "%s%s" % (self.jog_direction, self.jog_speed))
  def ChangeSelection(self, text):
    self.jog_direction = text[0]
    self.jog_speed = text[1]
    self.MakeText()
    self.Refresh()
    if self.jog_direction == '+':
      self.direc_menu.FindItemByPosition(0).Check()
    else:
      self.direc_menu.FindItemByPosition(1).Check()
    index = int(self.jog_speed)
    self.speed_menu.FindItemByPosition(index).Check()

class ControlMixin:
  def __init__(self, parent):
    self.font = wx.Font(conf.config_font_size, wx.FONTFAMILY_SWISS, wx.NORMAL,
          wx.FONTWEIGHT_NORMAL, False, conf.quisk_typeface)
    self.SetFont(self.font)
    self.row = 1
    self.charx = self.GetCharWidth()
    self.chary = self.GetCharHeight()
    self.quisk_height = self.chary * 14 // 10
    # GBS
    self.gbs = wx.GridBagSizer(2, 2)
    self.gbs.SetEmptyCellSize((self.charx, self.charx))
    self.SetSizer(self.gbs)
    self.gbs.Add((self.charx, self.charx), (0, 0))
  def MarkCols(self):
    for col in range(1, self.num_cols):
      c = wx.StaticText(self, -1, str(col % 10))
      self.gbs.Add(c, (self.row, col))
    self.row += 1
  def NextRow(self, row=None):
    if row is None:
      self.row += 1
    else:
      self.row = row
  def AddHelpButton(self, col, text, help_text, border=1):
    hbtn = QuiskPushbutton(self, self._BTnHelp, "..")
    hbtn.SetColorGray()
    hbtn.quisk_help_text = help_text
    hbtn.quisk_caption = text
    h = self.quisk_height + 2
    hbtn.SetSizeHints(h, h, h, h)
    self.gbs.Add(hbtn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
  def AddTextL(self, col, text, span=None):
    c = wx.StaticText(self, -1, text)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(c, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL)
    else:
      self.gbs.Add(c, (self.row, col), span=(1, span), flag=wx.ALIGN_CENTER_VERTICAL)
    return c
  def AddTextC(self, col, text, span=None, flag=wx.ALIGN_CENTER):
    c = wx.StaticText(self, -1, text)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(c, (self.row, col), flag=flag)
    else:
      self.gbs.Add(c, (self.row, col), span=(1, span), flag=flag)
    return c
  def AddTextCHelp(self, col, text, help_text, span=None):
    bsizer = wx.BoxSizer(wx.HORIZONTAL)
    txt = wx.StaticText(self, -1, text)
    bsizer.Add(txt, flag=wx.ALIGN_CENTER_VERTICAL)
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    bsizer.Add(btn, flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT|wx.RIGHT, border=self.charx)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(bsizer, (self.row, col), flag = wx.ALIGN_CENTER)
    else:
      self.gbs.Add(bsizer, (self.row, col), span=(1, span), flag = wx.ALIGN_CENTER)
    return bsizer
  def AddTextLHelp(self, col, text, help_text, span=None):
    bsizer = wx.BoxSizer(wx.HORIZONTAL)
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    bsizer.Add(btn, flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT|wx.RIGHT, border=self.charx)
    txt = wx.StaticText(self, -1, text)
    bsizer.Add(txt, flag=wx.ALIGN_CENTER_VERTICAL)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(bsizer, (self.row, col), flag = 0)
    else:
      self.gbs.Add(bsizer, (self.row, col), span=(1, span), flag = 0)
    return bsizer
  def AddTextColorChangeHelp(self, col, text, color, btn_text, handler, help_text, border=2, span1=1, span2=1, span3=1):
    txt = wx.StaticText(self, -1, text)
    self.gbs.Add(txt, (self.row, col), span=(1, span1), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
    col += span1
    Cbar = wx.StaticText(self, -1, '')
    h = self.quisk_height - 2
    Cbar.SetSizeHints(h, h, -1, h)
    if color:
      Cbar.SetBackgroundColour(color)
    self.gbs.Add(Cbar, (self.row, col), span=(1, span2),
       flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT,
       border=self.charx*2//10)
    col += span2
    btn = QuiskPushbutton(self, handler, btn_text)
    btn.SetColorGray()
    h = self.quisk_height + 2
    btn.SetSizeHints(-1, h, -1, h)
    self.gbs.Add(btn, (self.row, col), span=(1, span3), flag = wx.EXPAND)
    col += span3
    hbtn = QuiskPushbutton(self, self._BTnHelp, "..")
    hbtn.SetColorGray()
    hbtn.quisk_help_text = help_text
    hbtn.quisk_caption = text
    h = self.quisk_height + 2
    hbtn.SetSizeHints(h, h, h, h)
    self.gbs.Add(hbtn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, Cbar, btn
  def AddTextEditHelp(self, col, text1, text2, help_text, border=2, span1=1, span2=1, no_edit=True):
    txt = wx.StaticText(self, -1, text1)
    self.gbs.Add(txt, (self.row, col), span=(1, span1), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
    col += span1
    #txt = wx.StaticText(self, -1, text2)
    if no_edit:
      edt = QuiskTextCtrl(self, text2, style=wx.TE_READONLY)
    else:
      edt = QuiskTextCtrl(self, text2, style=wx.TE_PROCESS_ENTER)
    #self.gbs.Add(txt, (self.row, col), span=(1, span2), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
    self.gbs.Add(edt, (self.row, col), span=(1, span2),
       flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT,
       border=self.charx*2//10)
    col += span2
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text1
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    self.gbs.Add(btn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, edt, btn
  def AddTextSliderHelp(self, col, text, value, themin, themax, handler, help_text, border=2, span=1, scale=1):
    display = "%" in text
    sld = SliderBoxHH(self, text, value, themin, themax, handler, display, scale)
    self.gbs.Add(sld, (self.row, col), span=(1, span),
       flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT,
       border=self.charx*2//10)
    col += span
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    self.gbs.Add(btn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return sld, btn
  def AddPopupMenuHelp(self, Qclass, col, text, btn_text, help_text, span1=1, span2=1, border=1):
    if text:
      txt = wx.StaticText(self, -1, text)
      self.gbs.Add(txt, (self.row, col), span=(1, span1), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
      col += span1
    ctrl = Qclass(self, btn_text, self.quisk_height - 2, style=wx.BORDER_RAISED)
    self.gbs.Add(ctrl, (self.row, col), span=(1, span2), flag = wx.EXPAND|wx.RIGHT|wx.TOP|wx.BOTTOM|wx.LEFT, border=2)
    col += span2
    self.AddHelpButton(col, text, help_text, border)
    return ctrl
  def AddTextButtonHelp(self, col, text, butn_text, handler, help_text, span1=1, span2=1, border=1):
    if text:
      txt = wx.StaticText(self, -1, text)
      self.gbs.Add(txt, (self.row, col), span=(1, span1), flag = 0)
      col += span1
    else:
      txt = None
    btn = QuiskPushbutton(self, handler, butn_text)
    btn.SetColorGray()
    h = self.quisk_height + 2
    btn.SetSizeHints(-1, h, -1, h)
    self.gbs.Add(btn, (self.row, col), span=(1, span2), flag = wx.EXPAND)
    col += span2
    hbtn = QuiskPushbutton(self, self._BTnHelp, "..")
    hbtn.SetColorGray()
    hbtn.quisk_help_text = help_text
    hbtn.quisk_caption = text
    h = self.quisk_height + 2
    hbtn.SetSizeHints(h, h, h, h)
    self.gbs.Add(hbtn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, btn
  def AddText2CheckHelp(self, col, text, butn1_text, handler1, butn2_text, handler2, help_text, span1=1, span2=1, border=1):
    if text:
      txt = wx.StaticText(self, -1, text)
      self.gbs.Add(txt, (self.row, col), span=(1, span1), flag = wx.ALIGN_CENTER_VERTICAL | wx.EXPAND)
      col += span1
    else:
      txt = None
    btn1 = QuiskCheckbutton(self, handler1, butn1_text)
    btn1.SetColorGray()
    btn2 = QuiskCheckbutton(self, handler2, butn2_text)
    btn2.SetColorGray()
    w1 = btn1.GetSize().Width
    w2 = btn2.GetSize().Width
    w = max(w1, w2)
    h = self.quisk_height + 2
    btn1.SetSizeHints(w, h, w, h)
    btn2.SetSizeHints(w, h, w, h)
    bsizer = wx.BoxSizer(wx.HORIZONTAL)
    bsizer.Add(btn1, proportion=1)
    bsizer.Add(2, h, proportion=0)
    bsizer.Add(btn2, proportion=1)
    self.gbs.Add(bsizer, (self.row, col), span=(1, span2), flag = wx.EXPAND)
    col += span2
    hbtn = QuiskPushbutton(self, self._BTnHelp, "..")
    hbtn.SetColorGray()
    hbtn.quisk_help_text = help_text
    hbtn.quisk_caption = text
    h = self.quisk_height + 2
    hbtn.SetSizeHints(h, h, h, h)
    self.gbs.Add(hbtn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, btn1, btn2
  def AddText2ButtonHelp(self, col, text, butn1_text, handler1, butn2_text, handler2, help_text, span1=1, span2=1, border=1):
    if text:
      txt = wx.StaticText(self, -1, text)
      self.gbs.Add(txt, (self.row, col), span=(1, span1), flag = 0)
      col += span1
    else:
      txt = None
    btn1 = QuiskPushbutton(self, handler1, butn1_text)
    btn1.SetColorGray()
    btn2 = QuiskPushbutton(self, handler2, butn2_text)
    btn2.SetColorGray()
    w1 = btn1.GetSize().Width
    w2 = btn2.GetSize().Width
    w = max(w1, w2)
    h = self.quisk_height + 2
    btn1.SetSizeHints(w, h, 999, h)
    btn2.SetSizeHints(w, h, 999, h)
    bsizer = wx.BoxSizer(wx.HORIZONTAL)
    bsizer.Add(btn1, proportion=1)
    bsizer.Add(2, h, proportion=0)
    bsizer.Add(btn2, proportion=1)
    self.gbs.Add(bsizer, (self.row, col), span=(1, span2), flag = wx.EXPAND)
    col += span2
    hbtn = QuiskPushbutton(self, self._BTnHelp, "..")
    hbtn.SetColorGray()
    hbtn.quisk_help_text = help_text
    hbtn.quisk_caption = text
    h = self.quisk_height + 2
    hbtn.SetSizeHints(h, h, h, h)
    self.gbs.Add(hbtn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, btn1, btn2
  def AddTextCtrl(self, col, text, handler=None, span=None):
    c = wx.TextCtrl(self, -1, text, style=wx.TE_RIGHT)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(c, (self.row, col), flag=wx.ALIGN_CENTER)
    else:
      self.gbs.Add(c, (self.row, col), span=(1, span), flag=wx.ALIGN_CENTER)
    if handler:
      c.Bind(wx.EVT_TEXT, handler)
    return c
  def AddBoxSizer(self, col, span):
    bsizer = wx.BoxSizer(wx.HORIZONTAL)
    self.gbs.Add(bsizer, (self.row, col), span=(1, span))
    return bsizer
  def AddColSpacer(self, col, width):		# add a width spacer to row 0
    self.gbs.Add((width * self.charx, 1), (0, col))		# width is in characters
  def AddRadioButton(self, col, text, handler=None, span=None, start=False):
    if start:
      c = wx.RadioButton(self, -1, text, style=wx.RB_GROUP)
    else:
      c = wx.RadioButton(self, -1, text)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(c, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL)
    else:
      self.gbs.Add(c, (self.row, col), span=(1, span), flag=wx.ALIGN_CENTER_VERTICAL)
    if handler:
      c.Bind(wx.EVT_RADIOBUTTON, handler)
    return c
  def AddCheckBox(self, col, text, handler=None, flag=0, border=0):
    btn = wx.CheckBox(self, -1, text)
    h = self.quisk_height + 2
    btn.SetSizeHints(-1, h, -1, h)
    if col >= 0:
      self.gbs.Add(btn, (self.row, col), flag=flag, border=border*self.charx)
    if self.radio_name == "ConfigFileRadio":
      btn.Enable(False)
      noname_enable.append(btn)
    if handler:
      btn.Bind(wx.EVT_CHECKBOX, handler)
    return btn
  def AddBitField(self, col, number, name, band, value, handler=None, span=None, border=1):
    bf = QuiskBitField(self, number, value, self.quisk_height, handler)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(bf, (self.row, col), flag=wx.ALIGN_RIGHT|wx.ALIGN_CENTER_VERTICAL|wx.RIGHT|wx.LEFT, border=border*self.charx)
    else:
      self.gbs.Add(bf, (self.row, col), span=(1, span), flag=wx.ALIGN_RIGHT|wx.ALIGN_CENTER_VERTICAL|wx.RIGHT|wx.LEFT, border=border*self.charx)
    bf.quisk_data_name = name
    bf.quisk_band = band
    return bf
  def AddPushButton(self, col, text, handler, border=0):
    btn = QuiskPushbutton(self, handler, text)
    btn.SetColorGray()
    h = self.quisk_height + 2
    btn.SetSizeHints(-1, h, -1, h)
    if col >= 0:
      self.gbs.Add(btn, (self.row, col), flag=wx.RIGHT|wx.LEFT, border=border*self.charx)
    if self.radio_name == "ConfigFileRadio":
      btn.Enable(False)
      noname_enable.append(btn)
    return btn
  def AddPushButtonR(self, col, text, handler, border=0):
    btn = self.AddPushButton(-1, text, handler, border)
    if col >= 0:
      self.gbs.Add(btn, (self.row, col), flag=wx.ALIGN_RIGHT|wx.RIGHT|wx.LEFT, border=border*self.charx)
    return btn
  def AddComboCtrl(self, col, value, choices, right=False, no_edit=False, span=None, border=1):
    if no_edit:
      cb = ChoiceCombo(self, value, choices)
    else:
      cb = ComboCtrl(self, value, choices)
    if col < 0:
      pass
    elif span is None:
      self.gbs.Add(cb, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT|wx.LEFT, border=border*self.charx)
    else:
      self.gbs.Add(cb, (self.row, col), span=(1, span), flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT|wx.LEFT, border=border*self.charx)
    if self.radio_name == "ConfigFileRadio":
      cb.Enable(False)
      noname_enable.append(cb)
    return cb
  def AddComboCtrlTx(self, col, text, value, choices, right=False, no_edit=False):
    c = wx.StaticText(self, -1, text)
    if col >= 0:
      self.gbs.Add(c, (self.row, col))
      cb = self.AddComboCtrl(col + 1, value, choices, right, no_edit)
    else:
      cb = self.AddComboCtrl(col, value, choices, right, no_edit)
    return c, cb
  def AddTextComboHelp(self, col, text, value, choices, help_text, no_edit=False, border=2, span_text=1, span_combo=1):
    txt = wx.StaticText(self, -1, text)
    self.gbs.Add(txt, (self.row, col), span=(1, span_text), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
    col += span_text
    cb = self.AddComboCtrl(-1, value, choices, False, no_edit)
    if no_edit:
      if '#' in value:
        value = value[0:value.index('#')]
      value = value.strip()
      l = len(value)
      for i in range(len(choices)):
        ch = choices[i]
        if '#' in ch:
          ch = ch[0:ch.index('#')]
        ch.strip()
        if value == ch[0:l]:
          cb.SetSelection(i)
          break
      else:
        if 'fail' in value:
          pass
        else:
          print ("Failure to set value for", text, value, choices)
    self.gbs.Add(cb, (self.row, col), span=(1, span_combo),
       flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT, border=self.charx*2//10)
    col += span_combo
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    self.gbs.Add(btn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, cb, btn
  def AddTextDblSpinnerHelp(self, col, text, value, dmin, dmax, dinc, help_text, border=2, span_text=1, span_spinner=1):
    txt = wx.StaticText(self, -1, text)
    self.gbs.Add(txt, (self.row, col), span=(1, span_text), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
    col += span_text
    spn = wx.SpinCtrlDouble(self, -1, initial=value, min=dmin, max=dmax, inc=dinc)
    self.gbs.Add(spn, (self.row, col), span=(1, span_spinner),
       flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT,
       border=self.charx*2//10)
    col += span_spinner
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    self.gbs.Add(btn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, spn, btn
  def AddTextSpinnerHelp(self, col, text, value, imin, imax, help_text, border=2, span_text=1, span_spinner=1):
    txt = wx.StaticText(self, -1, text)
    self.gbs.Add(txt, (self.row, col), span=(1, span_text), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx)
    col += span_text
    spn = wx.SpinCtrl(self, -1, "")
    spn.SetRange(imin, imax)
    spn.SetValue(value)
    self.gbs.Add(spn, (self.row, col), span=(1, span_spinner),
       flag=wx.ALIGN_CENTER_VERTICAL|wx.EXPAND|wx.RIGHT,
       border=self.charx*2//10)
    col += span_spinner
    btn = QuiskPushbutton(self, self._BTnHelp, "..")
    btn.SetColorGray()
    btn.quisk_help_text = help_text
    btn.quisk_caption = text
    h = self.quisk_height + 2
    btn.SetSizeHints(h, h, h, h)
    self.gbs.Add(btn, (self.row, col), flag=wx.ALIGN_CENTER_VERTICAL|wx.RIGHT, border=self.charx*border)
    return txt, spn, btn
  def _BTnHelp(self, event):
    btn = event.GetEventObject()
    caption = btn.quisk_caption
    i = caption.find('%')	# remove formats %.2f
    if i > 0:
      caption = caption[0:i]
    dlg = wx.MessageDialog(self, btn.quisk_help_text, caption, style=wx.OK|wx.ICON_INFORMATION)
    dlg.ShowModal()
    dlg.Destroy()
  def ErrorCheck(self, ctrl):	# Return True for OK
    name = ctrl.quisk_data_name
    value = ctrl.GetValue()
    #if name == 'quisk_debug_sound' and value in ("New Sound", "Debug New Sound") and not QS.get_params('have_soundio'):
    #  dlg = wx.MessageDialog(None, "Please install the libsoundio-dev package, and re-make", 'New Sound', wx.OK|wx.ICON_ERROR)
    #  dlg.ShowModal()
    #  dlg.Destroy()
    #  return False
    return True
  def OnChange(self, ctrl):
    value = ctrl.GetValue()
    self.OnChange2(ctrl, value)
  def OnChange2(self, ctrl, value):
    # Careful: value is Unicode
    name = ctrl.quisk_data_name
    try:
      fmt4 = local_conf.format4name[name]
    except:
      if name[0:4] in ("lin_", "win_"):
        fmt4 = local_conf.format4name[name[4:]]
    fmt4 = fmt4[0:4]
    ok, x = self.EvalItem(value, fmt4)	# Only evaluates integer, number, boolean, text, rfile
    if ok:
      radio_dict = local_conf.GetRadioDict(self.radio_name)
      if name in name2format and name2format[name][1] == value:
        if name in radio_dict:
          del radio_dict[name]
          local_conf.settings_changed = True
      else:
        radio_dict[name] = value
        local_conf.settings_changed = True
      # Immediate changes
      if self.radio_name == Settings[1]:	# changed for current radio
        if name in ('hot_key_ptt_toggle', 'hot_key_ptt_if_hidden', 'keyupDelay', 'cwTone', 'pulse_audio_verbose_output',
                    'start_cw_delay', 'start_ssb_delay', 'maximum_tx_secs', 'quisk_serial_cts', 'quisk_serial_dsr',
                    'hot_key_ptt1', 'hot_key_ptt2', 'midi_ptt_toggle', 'TxRxSilenceMsec', 'hermes_lite2_enable',
                    'fixed_tune_offset'):
          setattr(conf, name, x)
          application.ImmediateChange(name)
        elif name[0:4] in ('lin_', 'win_'):
          name = name[4:]
          if name in ('quisk_serial_port', ):
            setattr(conf, name, x)
            application.ImmediateChange(name)
        elif name == "reverse_tx_sideband":
          setattr(conf, name, x)
          QS.set_tx_audio(reverse_tx_sideband=x)
        elif name == "dc_remove_bw":
          setattr(conf, name, x)
          QS.set_sparams(dc_remove_bw=x)
        elif name == "digital_output_level":
          setattr(conf, name, x)
          QS.set_sparams(digital_output_level=x)
        elif name == "file_play_level":
          setattr(conf, name, x)
          QS.set_sparams(file_play_level=x)
        elif name[0:7] == 'hermes_':
          if name == 'hermes_TxLNA_dB':
            application.Hardware.ChangeTxLNA(x)
          elif name == "hermes_bias_adjust" and self.HermesBias0:
            self.HermesBias0.Enable(x)
            self.HermesBias1.Enable(x)
            self.HermesWriteBiasButton.Enable(x)
            application.Hardware.EnableBiasChange(x)
          elif hasattr(application.Hardware, "ImmediateChange"):
            setattr(conf, name, x)
            application.Hardware.ImmediateChange(name)
        elif name[0:6] == 'keyer_':
          setattr(conf, name, x)
          application.ImmediateChange(name)
  def FormatOK(self, value, fmt4):		# Check formats integer, number, boolean
    ok, v = self.EvalItem(value, fmt4)
    return ok
  def EvalItem(self, value, fmt4):		# Return Python integer, number, boolean, text, rfile, keycode
    # return is (item_is_ok, evaluated_item)
    if fmt4 not in ('inte', 'numb', 'bool', 'keyc'):	# only certain formats are evaluated
      return True, value	# text, rfile are returned by default
    jj = value.find('#')
    if jj > 0:
      value = value[0:jj]
    try:	# only certain formats are evaluated
      if fmt4 == 'inte':
        v = int(value, base=0)
      elif fmt4 == 'numb':
        v = float(value)
      elif fmt4 == 'bool':
        if value == "True":
          v = True
        else:
          v = False
      elif fmt4 == 'keyc':	# key code
        if value == "None":
          v = None
        else:
          v = eval(value)
          v = int(v)
      else:
        raise ValueError
    except:
      #traceback.print_exc()
      dlg = wx.MessageDialog(None,
        "Can not set item with format %s to value %s" % (fmt4, value),
        'Change to item', wx.OK|wx.ICON_ERROR)
      dlg.ShowModal()
      dlg.Destroy()
      return False, None
    return True, v
  def GetValue(self, name, radio_dict):
    try:
      value = radio_dict[name]
    except:
      pass
    else:
      return value
    # Value was not in radio_dict.  Get it from conf.  There are values for platform win_data_name and lin_data_name.
    # The win_ and lin_ names are not in conf.
    try:
      fmt = local_conf.format4name[name]
    except:
      fmt = ''		# not all items in conf are in section_data or receiver_data
    try:
      if fmt == 'dict':				# make a copy for this radio
        value = {}
        value.update(getattr(conf, name))
      elif fmt == 'list':			# make a copy for this radio
        value = getattr(conf, name)[:]
      else:
        value = str(getattr(conf, name))
    except:
      return ''
    else:
      return value

class BaseWindow(wx.ScrolledWindow, ControlMixin):
  def __init__(self, parent):
    wx.ScrolledWindow.__init__(self, parent)
    ControlMixin.__init__(self, parent)

class ConfigConfig(BaseWindow):
  def __init__(self, parent, width):
    BaseWindow.__init__(self, parent)
    self.width = width
    self.SetBackgroundColour(parent.bg_color)
    self.num_cols = 7
    #self.MarkCols()
    self.radio_name = None
    # Choice (combo) box for decimation
    lst = application.Hardware.VarDecimGetChoices()
    if lst:
      txt = application.Hardware.VarDecimGetLabel()
      index = application.Hardware.VarDecimGetIndex()
    else:
      txt = "Variable decimation"
      lst = ["None"]
      index = 0
    help_text = "If your hardware supports different sample rates, choose one here. For SoftRock, \
change the sample rate of your sound card."
    txt, cb, btn = self.AddTextComboHelp(1, txt, lst[index], lst, help_text, True)
    if lst:
      cb.handler = application.OnBtnDecimation
    self.btn_decimation = cb
    self.NextRow()
    self.NextRow()
    help_text = "SoftRock and other radios that use a sound card for Rx and Tx will need small corrections \
to the amplitude and phase. Click here for an adjustment screen."
    txt, btn = self.AddTextButtonHelp(1, "Adjust receive amplitude and phase", "Rx Phase..", self.OnBtnPhase, help_text)
    self.rx_phase = btn
    if not conf.name_of_sound_capt:
      btn.Enable(0)
    self.NextRow()
    txt, btn = self.AddTextButtonHelp(1, "Adjust transmit amplitude and phase", "Tx Phase..", self.OnBtnPhase, help_text)
    self.tx_phase = btn
    if not conf.name_of_mic_play:
      btn.Enable(0)
    self.NextRow()
    help_text = "There is a color bar under the X-axis showing the band plan. Click here for a screen to change the colors."
    txt, btn = self.AddTextButtonHelp(1, "Colors on X-axis for CW, Phone", "Band plan..", self.OnBtnBandPlan, help_text)
    self.NextRow()
    help_text = "Click here to configure the link to WSJT-X."
    txt, btn = self.AddTextButtonHelp(1, "Configure WSJT-X", "WSJT-X..", self.OnConfigureWsjtx, help_text)
    self.NextRow()
    lst = ("Never", "Main Rx0 on startup", "Main Rx0 now")
    value = application.local_conf.globals.get("start_wsjtx", "Never")
    txt, cb, btn = self.AddTextComboHelp(1, "Start WSJT-X", value, lst, help_text, True)
    cb.handler = application.OnStartWsjtx
    self.NextRow()
    help_text = "This controls the Tx level for non-digital modes. It is usually 100%."
    c1, btn = self.AddTextSliderHelp(1, "Tx level %d%%  ", 100, 0, 100, self.OnTxLevel, help_text, span=2)
    self.NextRow()
    level = conf.digital_tx_level
    help_text = "This controls the TX level for digital modes. Digital modes require greater linearity, and the digital level is often 25%."
    c2, btn = self.AddTextSliderHelp(1, "Digital Tx level %d%%  ", level, 0, level, self.OnDigitalTxLevel, help_text, span=2)
    if not hasattr(application.Hardware, "SetTxLevel"):
      c1.slider.Enable(0)
      c2.slider.Enable(0)
    self.NextRow()
    #### Make controls SECOND column
    self.row = 3
    self.AddTextL(4, "Configuration for the file record button:  " + conf.Xbtn_text_file_rec, span=2)
    self.NextRow()
    # File for recording speaker audio
    text = "Record Rx audio to WAV files 1, 2, ... "
    path = application.file_name_rec_audio
    help_text = 'These check buttons control what happens when you press the "File Record" button. \
Please choose a directory for your recordings; perhaps the "Music" directory on your computer. \
Then choose your file name. This "Record Rx audio" file is used for recording the radio speaker sound. \
Recording stops when the button is released. When pressed again new files are created with names 001, 002, etc. \
You will need to delete recordings you no longer need.'
    self.file_button_rec_speaker = self.MakeFileButton(text, path, 0, 'rec_audio', help_text)
    # File for recording samples
    text = "Record I/Q samples to WAV files 1, 2, ... "
    path = application.file_name_rec_samples
    help_text = 'This file is used to record the I/Q samples and it works like the "Record Rx audio" button.'
    self.file_button_rec_iq = self.MakeFileButton(text, path, 1, 'rec_samples', help_text)
    # File for recording the microphone
    text = "Record the mic to make a CQ message"
    path = application.file_name_rec_mic
    help_text = 'This file is used to record the microphone. It can be used to record a CQ message that can be played \
over and over until someone answers. Change the "Play audio" to the same file to test the recording. Keep recording and \
playing until you are satisfied, and then un-check the "Record mic" button. The file is replaced when the Record button \
is pressed again (no 001, 002, ...).'
    self.file_button_rec_mic = self.MakeFileButton(text, path, 2, 'rec_mic', help_text)
    ## Play buttons
    self.AddTextL(4, "Configuration for the file play button:  " + conf.Xbtn_text_file_play, span=2)
    self.NextRow()
    # File for playing speaker audio
    text = "Play audio from a WAV file"
    path = application.file_name_play_audio
    help_text = 'These record check buttons control what happens when you press the "File Play" button. \
This button plays normal audio files, not samples.'
    self.file_button_play_speaker = self.MakeFileButton(text, path, 10, 'play_audio', help_text)
    # file for playing samples
    text = "Receive saved I/Q samples from a file"
    path = application.file_name_play_samples
    help_text = 'This button plays I/Q samples. You can tune in different stations from the I/Q recording.'
    self.file_button_play_iq = self.MakeFileButton(text, path, 11, 'play_samples', help_text)
    # File for playing a file to the mic input for a CQ message
    text = "Repeat a CQ message until a station answers"
    path = application.file_name_play_cq
    help_text = "This button enables the File Play button to start playing your CQ message."
    self.file_button_play_mic = self.MakeFileButton(text, path, 12, 'play_cq', help_text)
    # CQ repeat time
    help_text = 'This is the amount of time to wait for someone to respond to your CQ message. \
The CW message will then repeat. A time of zero means no repeat.'
    sl, btn = self.AddTextSliderHelp(4, "    Repeat secs %.1f  ", 0, 0, 100, self.OnPlayFileRepeat, help_text, span=2, scale=0.1)
    self.NextRow()
    self.FitInside()
    self.SetScrollRate(1, 1)
  def MakeFileButton(self, text, path, index, name, help_text):
    if index < 10:	# record buttons
      cb = self.AddCheckBox(4, text, self.OnCheckRecPlay)
    elif index == 10:
      cb = self.AddRadioButton(4, text, self.OnCheckRecPlay, start=True)
    else:
      cb = self.AddRadioButton(4, text, self.OnCheckRecPlay, start=False)
    txt, b = self.AddTextButtonHelp(5, '', "File..", self.OnBtnFileName, help_text)
    b.check_box = cb
    b.index = cb.index = index
    b.path = cb.path = path
    b.name = 'file_name_' + name
    if index < 10:	# record buttons
      if path:
        enable = True
        if index == 0:	# check record audio if there is a path
          cb.SetValue(True)
      else:
        enable = False
    else:		# playback buttons
      enable = os.path.isfile(path)
    cb.Enable(enable)
    self.NextRow()
    return b
  def OnTxLevel(self, event):
    application.tx_level = event.GetEventObject().GetValue()
    application.Hardware.SetTxLevel()
  def OnDigitalTxLevel(self, event):
    application.digital_tx_level = event.GetEventObject().GetValue()
    application.Hardware.SetTxLevel()
  def OnBtnPhase(self, event):
    btn = event.GetEventObject()
    if btn.GetLabel()[0:2] == 'Tx':
      rx_tx = 'tx'
    else:
      rx_tx = 'rx'
    application.screenBtnGroup.SetLabel('Graph', do_cmd=True)
    if application.w_phase:
      application.w_phase.Raise()
    else:
      application.w_phase = QAdjustPhase(self, self.width, rx_tx)
  def OnBtnFileName(self, event):
    btn = event.GetEventObject()
    dr, fn = os.path.split(btn.path)
    if btn.index in (0, 1):	# record audio or samples
      dlg = wx.FileDialog(self, 'Choose WAV file', dr, fn, style=wx.FD_SAVE, wildcard="Wave files (*.wav)|*.wav")
    elif btn.index == 2:	# record mic
      dlg = wx.FileDialog(self, 'Choose WAV file', dr, fn, style=wx.FD_SAVE|wx.FD_OVERWRITE_PROMPT, wildcard="Wave files (*.wav)|*.wav")
    else:	# play buttons
      dlg = wx.FileDialog(self, 'Choose WAV file', dr, fn, style=wx.FD_OPEN, wildcard="Wave files (*.wav)|*.wav")
    if dlg.ShowModal() == wx.ID_OK:
      path = dlg.GetPath()
      if path[-4:].lower() != '.wav':
        path = path + '.wav'
      btn.path = path
      setattr(application, btn.name, path)
      btn.check_box.path = path
      btn.check_box.Enable(True)
      if btn.index >= 10:	# play buttons
        btn.check_box.SetValue(True)
        application.file_play_source = btn.index
      self.EnableRecPlay()
    dlg.Destroy()
  def ChangePlayFile(self, btn, path):
      btn.path = path
      setattr(application, btn.name, path)
      btn.check_box.path = path
      btn.check_box.Enable(True)
      self.EnableRecPlay()
  def OnBtnBandPlan(self, ctrl):
    from configure import BandPlanDlg
    dlg = BandPlanDlg(self)
    dlg.ShowModal()
    dlg.Destroy()
  def OnConfigureWsjtx(self, ctrl):
    from configure import WsjtxDlg
    dlg = WsjtxDlg(self)
    dlg.ShowModal()
    dlg.Destroy()
  def InitRecPlay(self):
    for btn in (self.file_button_play_speaker, self.file_button_play_iq, self.file_button_play_mic):
      if btn.index == application.file_play_source:
        btn.check_box.SetValue(True)
  def EnableRecPlay(self):	# Enable or disable file record/play buttons on main screen
    enable_rec = False
    enable_play = False
    for btn in (self.file_button_rec_speaker, self.file_button_rec_iq, self.file_button_rec_mic):
      if btn.check_box.GetValue():
        enable_rec = True
        break
    for btn in (self.file_button_play_speaker, self.file_button_play_iq, self.file_button_play_mic):
       if btn.check_box.GetValue() and os.path.isfile(btn.path):
         enable_play = True
         break
    application.btn_file_record.Enable(enable_rec)
    application.btnFilePlay.Enable(enable_play)
  def OnCheckRecPlay(self, event):
    btn = event.GetEventObject()
    if btn.GetValue():
      if btn.index >= 10:	# play button
        application.file_play_source = btn.index
    self.EnableRecPlay()
  def OnPlayFileRepeat(self, event):
    application.file_play_repeat = event.GetEventObject().GetValue() * 0.1
  def OnFilePlayButton(self, play):
    if play:
      for btn in (self.file_button_play_speaker, self.file_button_play_iq, self.file_button_play_mic):
         if application.file_play_source == btn.index and btn.check_box.GetValue() and os.path.isfile(btn.path):
           QS.open_wav_file_play(btn.path)
           break
    else:
      QS.set_file_name(play_button=0)	# Close all play files
  def OnFileRecordButton(self, record):	# The File Record button on the main screen
    if record:
      for btn in (self.file_button_rec_speaker, self.file_button_rec_iq, self.file_button_rec_mic):
        if btn.check_box.GetValue():	# open this file
          if btn.index in (0, 1) and os.path.isfile(btn.path):	# Change path
            direc, fname = os.path.split(btn.path)
            base = os.path.splitext(fname)[0]
            while base and base[-1] in '0123456789':
              base = base[0:-1]
            if not base:
              base = "rec"
            index = 0
            for dir_entry in os.scandir(direc):
              if dir_entry.is_file():
                name = dir_entry.name
                if len(name) > 4 and name[-4] == '.' and name.startswith(base):
                  ma = re.match(base + '([0-9]+)[.]', name)
                  if ma:
                    index = max(index, int(ma.group(1), base=10))
            btn.path = os.path.join(direc, "%s%03d.wav" % (base, index + 1))
          QS.set_file_name(btn.index, btn.path, record_button=1)
          if btn.index == 0:	# Change play files to equal record files
            self.ChangePlayFile(self.file_button_play_speaker, btn.path)
          elif btn.index == 1:
            self.ChangePlayFile(self.file_button_play_iq, btn.path)
    else:
      QS.set_file_name(record_button=0)		# Close all record files

class ConfigTxAudio(BaseWindow):
  def __init__(self, parent):
    BaseWindow.__init__(self, parent)
    self.tmp_playing = False
    self.timer = wx.Timer(self)
    self.Bind(wx.EVT_TIMER, self.OnTimer)
    self.SetBackgroundColour(parent.bg_color)
    self.num_cols = 4
    #self.MarkCols()
    self.NextRow()
    t = "This is a test screen for transmit audio.  SSB, AM and FM have separate settings."
    item = self.AddTextL(1, t, span=2)
    self.NextRow()
    self.NextRow()
    self.NextRow()
    help_text = "Record the transmit audio while adjusting clip and preemphasis; then play back the result."
    txt, self.btn_record, self.btn_playback = self.AddText2CheckHelp(1, "Record transmit audio" + ' ' * 10,
            "Record", self.OnBtnRecord, "Playback", self.OnBtnPlayback, help_text)
    self.btn_playback.Enable(0)
    if not conf.microphone_name:
      self.btn_record.Enable(0)
    self.NextRow()
    help_text = "Audio level that triggers VOX (all modes)."
    self.AddTextSliderHelp(1, "VOX level %d dB", application.levelVOX, -40, 0, application.OnLevelVOX, help_text, span=2)
    self.NextRow()
    help_text = "Time to hold VOX after end of audio in seconds."
    self.AddTextSliderHelp(1, "VOX hold %0.2f secs", application.timeVOX, 0, 4000, application.OnTimeVOX, help_text, span=2, scale=0.001)
    self.NextRow()
    help_text = "Tx audio clipping level in dB for this mode."
    sld, btn = self.AddTextSliderHelp(1, "Clip level %2d dB", 0, 0, 20, application.OnTxAudioClip, help_text, span=2)
    application.CtrlTxAudioClip = sld
    self.NextRow()
    help_text = "Tx audio preemphasis of high frequencies."
    sld, btn = self.AddTextSliderHelp(1, "Preemphasis %4.2f", 0, 0, 100, application.OnTxAudioPreemph, help_text, span=2, scale=0.01)
    application.CtrlTxAudioPreemph = sld
    self.NextRow()
    self.FitInside()
    self.SetScrollRate(1, 1)
  def OnTimer(self, event):
    if not self.tmp_playing:
      self.timer.Stop()
    elif QS.set_record_state(-1):	# poll to see if playback is finished
      self.tmp_playing = False
      self.timer.Stop()
      self.btn_playback.SetValue(False)
      self.btn_record.Enable(1)
  def OnBtnRecord(self, event):
    if event.GetEventObject().GetValue():
      QS.set_kill_audio(1)
      self.btn_playback.Enable(0)
      QS.set_record_state(4)
    else:
      QS.set_kill_audio(0)
      self.btn_playback.Enable(1)
      QS.set_record_state(1)
  def OnBtnPlayback(self, event):
    if event.GetEventObject().GetValue():
      self.btn_record.Enable(0)
      QS.set_record_state(2)
      self.tmp_playing = True
      self.timer.Start(milliseconds=200)
    else:
      self.btn_record.Enable(1)
      QS.set_record_state(3)
      self.tmp_playing = False

class Radios(BaseWindow):	# The "Radios" first-level page
  def __init__(self, parent):
    BaseWindow.__init__(self, parent)
    self.SetBackgroundColour(parent.bg_color)
    self.num_cols = 5
    self.radio_name = None
    self.index = 0
    txt = 'Each "radio" is a block of settings that describes a radio to use with Quisk.'
    self.AddTextL(1, txt, self.num_cols - 1)
    self.NextRow()
    self.NextRow()
    self.radio_buttons = []
    rb = self.AddRadioButton(1, "When Quisk starts, use the radio", self.NewTop, start=True)
    rb.SetValue(True)
    self.start_radio = self.AddComboCtrl(2, 'big_radio_name', choices=[], no_edit=True)
    self.start_radio.handler = self.OnChoiceStartup
    self.radio_buttons.append((rb, self.start_radio))
    self.NextRow()
    rb = self.AddRadioButton(1, "Add a new radio with the general type", self.NewTop)
    choices = []
    for name, data in local_conf.receiver_data:
      choices.append(name)
    choices.sort()
    self.add_type = self.AddComboCtrl(2, '', choices=choices, no_edit=True)
    self.add_type.SetSelection(0)
    item = self.AddTextL(3, "and name the new radio")
    self.add_name = self.AddComboCtrl(4, '', choices=["My Radio", "Transverter", "SoftRock", "HL2"])
    self.radio_buttons.append((rb, self.add_type, self.add_name))
    self.NextRow()
    rb = self.AddRadioButton(1, "Add a new radio copied from radio", self.NewTop)
    self.add_from = self.AddComboCtrl(2, 'big_radio_name', choices=[], no_edit=True)
    item = self.AddTextL(3, "and name the new radio")
    self.add_name2 = self.AddComboCtrl(4, '', choices=["My Radio", "Transverter", "SoftRock", "HL2"])
    self.radio_buttons.append((rb, self.add_from, self.add_name2))
    self.NextRow()
    rb = self.AddRadioButton(1, "Rename the radio named", self.NewTop)
    self.rename_old = self.AddComboCtrl(2, 'big_radio_name', choices=[], no_edit=True)
    item = self.AddTextL(3, "to the new name")
    self.rename_new = self.AddComboCtrl(4, '', choices=["My Radio", "Transverter", "SoftRock", "HL2"])
    self.radio_buttons.append((rb, self.rename_old, self.rename_new))
    self.NextRow()
    rb = self.AddRadioButton(1, "Delete the radio named", self.NewTop)
    self.delete_name = self.AddComboCtrl(2, 'big_radio_name', choices=[], no_edit=True)
    self.radio_buttons.append((rb, self.delete_name))
    self.NextRow()
    self.NextRow()
    self.apply_btn = self.AddPushButton(3, "Apply", self.OnBtnApply)
    self.NewRadioNames()
    self.NewTop()
    self.SetScrollRate(1, 1)
  def NewTop(self, event=None):
    if event:
      btn = event.GetEventObject()
    else:	# set first button
      btn = self.radio_buttons[0][0]
    for i in range(len(self.radio_buttons)):
      data = self.radio_buttons[i]
      rb = data[0]
      if rb == btn:
        self.index = i
      for c in data[1:]:
        c.Enable(rb == btn)
    if self.index == 0:
      self.apply_btn.Enable(False)
    else:
      self.apply_btn.Enable(True)
  def OnBtnApply(self, event):
    index = self.index
    ok = False
    if index == 1:	# add from hardware type
      ok = self.OnBtnAdd()
    elif index == 2:	# add from existing radio
      ok = self.OnBtnCopy()
    elif index == 3:	# rename
      ok = self.OnBtnRename()
    elif index == 4:	# delete
      ok = self.OnBtnDelete()
    if ok:
      self.radio_buttons[0][0].SetValue(True)
      self.NewTop()
  def DuplicateName(self, name):
    if name in Settings[2] or name == "ConfigFileRadio":
      dlg = wx.MessageDialog(self, "The name already exists.  Please choose a different name.",
          'Quisk', wx.OK)
      dlg.ShowModal()
      dlg.Destroy()
      return True
    return False
  def OnBtnAdd(self):
    name = self.add_name.GetValue().strip()
    if not name or self.DuplicateName(name):
      return
    self.add_name.SetValue('')
    typ = self.add_type.GetValue().strip()
    if local_conf.AddRadio(name, typ):
      self.NewRadioNames()
      local_conf.settings_changed = True
      return True
  def OnBtnCopy(self):
    new_name = self.add_name2.GetValue().strip()
    if not new_name or self.DuplicateName(new_name):
      return
    self.add_name2.SetValue('')
    old_name = self.add_from.GetValue()
    if local_conf.CopyRadio(old_name, new_name):
      self.NewRadioNames()
      local_conf.settings_changed = True
      return True
  def OnBtnRename(self):
    old = self.rename_old.GetValue()
    new = self.rename_new.GetValue().strip()
    if not old or not new or self.DuplicateName(new):
      return
    self.rename_new.SetValue('')
    if local_conf.RenameRadio(old, new):
      if Settings[1] == old:
        Settings[1] = new
      self.NewRadioNames()
      local_conf.settings_changed = True
      return True
  def OnBtnDelete(self):
    name = self.delete_name.GetValue()
    if not name:
      return
    dlg = wx.MessageDialog(self,
        "Are you sure you want to permanently delete the radio %s?" % name,
        'Quisk', wx.OK|wx.CANCEL|wx.ICON_EXCLAMATION)
    ret = dlg.ShowModal()
    dlg.Destroy()
    if ret == wx.ID_OK and local_conf.DeleteRadio(name):
      self.NewRadioNames()
      local_conf.settings_changed = True
      return True
  def OnChoiceStartup(self, ctrl):
    choice = self.start_radio.GetValue()
    if Settings[0] != choice:
      Settings[0] = choice
      local_conf.settings_changed = True
  def NewRadioNames(self):		# Correct all choice lists for changed radio names
    choices = Settings[2][:]			# can rename any available radio
    self.rename_old.SetItems(choices)
    self.rename_old.SetSelection(0)
    self.add_from.SetItems(choices)
    self.add_from.SetSelection(0)
    if "ConfigFileRadio" in choices:	# can not delete ConfigFileRadio
      choices.remove("ConfigFileRadio")
    self.delete_name.SetItems(choices)
    self.delete_name.SetSelection(0)
    choices = Settings[2] + ["Ask me"]
    if "ConfigFileRadio" not in choices:
      choices.append("ConfigFileRadio")
    self.start_radio.SetItems(choices)	# can start any radio, plus "Ask me" and "ConfigFileRadio"
    try:	# Set text in control
      index = choices.index(Settings[0])	# last used radio, or new or renamed radio
    except:
      num = len(Settings[2])	# number of radios
      if num == 0:		# no radios
        index = 1		# use ConfigFileRadio
      elif num == 1:		# one radio
        index = 0		# use the one radio
      else:
        index = len(choices) - 2	# use the last radio
      Settings[0] = choices[index]
    self.start_radio.SetSelection(index)

class RadioSection(BaseWindow):		# The pages for each section in the second-level notebook for each radio
  help_cw = \
  'If your CW key is not connected to your radio hardware, you can connect the key to Quisk using a serial port or MIDI. '\
  'For the serial port, enter the serial port name and either CTS or DSR for the key. '\
  'You can use MIDI for keying. See the "Keys" screen . '\
  'If you turn on the Quisk internal sidetone, be sure to use the Fast Sound setting for Windows. '\
  'And for Linux, use Alsa for the radio sound output. '\
  'Reduce the hardware poll time for faster response.'
  def __init__(self, parent, radio_name, section, names):
    BaseWindow.__init__(self, parent)
    self.radio_name = radio_name
    self.section = section
    self.names = names
    self.MakeControls()
  def MakeControls(self):
    self.num_cols = 8
    #self.MarkCols()
    self.NextRow(3)
    Keys = self.section == "Keys"
    if Keys:
      col = 5
    else:
      col = 1
    start_row = self.row
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    radio_type = radio_dict['hardware_file_type']
    for name, text, fmt, help_text, values in self.names:
      if name == 'remote_radio_password':
        self.AddTextButtonHelp(col, text, "Change", self.OnChangePassword, help_text)
        if col == 1:
          col = 4
        else:
          col = 1
          self.NextRow()
      elif name == 'remote_radio_ip' and radio_type != "Control Head":
        continue
      elif name == 'favorites_file_path':
        self.favorites_path = radio_dict.get('favorites_file_path', '')
        row = self.row
        self.row = 1
        item, self.favorites_combo, btn = self.AddTextComboHelp(1, text, self.favorites_path, values, help_text, False, span_text=1, span_combo=4)
        self.favorites_combo.handler = self.OnButtonChangeFavorites
        item = self.AddPushButtonR(7, "Change..", self.OnButtonChangeFavorites, border=0)
        self.row = row
      else:
        if name == "keyupDelay":
          if col != 1:
            col = 1
            self.NextRow()
          self.NextRow()
          self.NextRow()
          self.AddTextCHelp(1, "CW Settings for Remote and Local Operation", self.help_cw, 8)
          col = 1
          self.NextRow()
          self.NextRow()
        if fmt[0:4] in ('dict', 'list'):
          continue
        if name[0:4] == platform_ignore:
          continue
        if name == 'use_fast_sound' and sys.platform != 'win32':
          continue
        value = self.GetValue(name, radio_dict)
        no_edit = "choice" in fmt or fmt == 'boolean'
        if name == "midi_cwkey_device":
          # Start of MIDI
          values = QS.control_midi(get_in_names=1)
          values.insert(0, '')
        txt, cb, btn = self.AddTextComboHelp(col, text, value, values, help_text, no_edit)
        cb.handler = self.OnChange
        cb.quisk_data_name = name
        if Keys:
          self.NextRow()
        elif col == 1:
          col = 4
        else:
          col = 1
          self.NextRow()
    if self.section == "Keys":
      if self.radio_name == Settings[1]:	# Current radio in use
        application.config_midi_window = self
      charx = self.charx
      self.list_ctrl = list_ctrl = wx.ListCtrl(self, size=(charx * 19 + 5, self.quisk_height * 8),
                     style=wx.LC_REPORT | wx.LC_SINGLE_SEL | wx.BORDER_SIMPLE)
      list_ctrl.InsertColumn(0, 'Midi', width=charx*10)
      list_ctrl.InsertColumn(1, 'Quisk', width=charx*9)
      list_ctrl.SetBackgroundColour(wx.Colour(0xeaeaea))
      list_ctrl.Bind(wx.EVT_LIST_ITEM_SELECTED, self.OnMidiListSelected)
      index = 0
      for txt_note in local_conf.MidiNoteDict:		# txt_note is a string
        int_note = int(txt_note, base=0)
        self.list_ctrl.InsertItem(index, txt_note)
        self.list_ctrl.SetItemData(index, int_note)
        self.list_ctrl.SetItem(index, 1, local_conf.MidiNoteDict[txt_note])
        index += 1
      list_ctrl.SortItems(lambda x, y: x - y)
      rows = self.row - start_row + 1
      self.row = start_row
      self.gbs.Add(list_ctrl, (start_row, 1), span=(rows, 1), flag=wx.RIGHT, border=charx * 15 // 10)
      #self.gbs.Add (charx, self.chary // 2, wx.GBPosition(start_row + rows, 1))	# Spacer at the bottom
      hlp = \
      'You can use a MIDI keyboard or other MIDI device to control Quisk. '\
      'These Midi devices generate a control number and value for buttons, knobs and jog wheels. '\
      'You can assign the number to a Quisk button or slider. '\
      'Operate the control and look here to see the note or control number received by Quisk. '\
      'Then press a button below to assign the control.'
      txt, self.midi_edt, btn = self.AddTextEditHelp(2, "Midi control", '', hlp, border=2, span1=1, span2=1, no_edit=True)
      self.NextRow()
      hlp = 'Assign the Midi button to a Quisk button.'
      ctrl = self.AddPopupMenuHelp(MidiButton, 2, "Assign Midi control to", "Button", hlp)
      ctrl.handler = self.OnMidiMenu
      self.NextRow()
      hlp = \
      'A "knob" is a Midi control that rotates left and right one turn. It sends a value from 0 to 127. '\
      'Assign the Midi knob to a Quisk control.'
      ctrl = self.AddPopupMenuHelp(MidiKnob, 2, "Assign Midi control to", "Knob", hlp)
      ctrl.handler = self.OnMidiMenu
      self.NextRow()
      hlp = \
      'A "jog wheel" is a Midi control that rotates around and around. It sends continuous high or low values. '\
      'Assign the Midi jog wheel to a Quisk control.'
      self.midiJogWheel = self.AddPopupMenuHelp(MidiJogWheel, 2, "Assign Midi control to", "Jog Wheel", hlp)
      self.midiJogWheel.handler = self.OnMidiMenu
      self.NextRow()
      hlp = "To delete a row, select it and press this button."
      self.AddTextButtonHelp(2, "Delete Midi row", "Delete", self.OnMidiDelete, hlp)
      self.NextRow()
    if not Keys:
      self.AddColSpacer(2, 20)
      self.AddColSpacer(5, 20)
    self.FitInside()
    self.SetScrollRate(1, 1)
  def OnButtonChangeFavorites(self, event):
    if isinstance(event, (ChoiceCombo, ComboCtrl)):
      path = event.GetValue()
    else:
      direc, fname = os.path.split(getattr(conf, 'favorites_file_in_use'))
      dlg = wx.FileDialog(None, "Choose Favorites File", direc, fname, "*.txt", wx.FD_OPEN)
      if dlg.ShowModal() == wx.ID_OK:
        path = dlg.GetPath()
        self.favorites_combo.SetText(path)
        dlg.Destroy()
      else:
        dlg.Destroy()
        return
    path = path.strip()
    self.favorites_path = path
    local_conf.GetRadioDict(self.radio_name)["favorites_file_path"] = path
    local_conf.settings_changed = True
  def OnMidiDelete(self, event):
    ctrl = self.list_ctrl
    index = ctrl.GetFirstSelected()
    if index < 0:
      return
    txt_note = ctrl.GetItem(index, 0).GetText()
    ctrl.DeleteItem(index)
    del local_conf.MidiNoteDict[txt_note]
    local_conf.settings_changed = True
    if index > 0:
      index -= 1
    if ctrl.GetItemCount() > 0:
      ctrl.Select(index)
      ctrl.EnsureVisible(index)
  def MidiAddNote(self):
    note = self.midi_edt.GetValue()
    self.midi_edt.Clear()
    if not note:
      return -1
    txt_note = note.split()[0]
    if txt_note[2] == '8':
      txt_note = "0x9%s" % txt_note[3:]		# Change Note Off to store as Note On
    int_note = int(txt_note, base=0)
    ctrl = self.list_ctrl
    index = ctrl.FindItem(-1, txt_note)
    if index < 0:	# Item is not in the list. Add it.
      ctrl.Append((txt_note, ''))
      ctrl.SetItemData(ctrl.GetItemCount() - 1, int_note)
      ctrl.SortItems(lambda x, y: x-y)
      index = ctrl.FindItem(-1, txt_note)
      local_conf.MidiNoteDict[txt_note] = ''
      local_conf.settings_changed = True
    if index >= 0:
      ctrl.Select(index)
      ctrl.EnsureVisible(index)
    return index
  def OnMidiMenu(self, ctrl, jog_tag=None):
    if jog_tag:		# Change the jog tag on the current line
      index = self.list_ctrl.GetFirstSelected()
      if index < 0:
        return
      text = self.list_ctrl.GetItem(index, 1).GetText()
      if len(text) > 3 and text[-3] == ' ' and text[-2] in "+-" and text[-1] in '0123456789':	# Item is a jog wheel item
        text = text[0:-2] + jog_tag
      else:	# There is no jog tag on the current item
        return
    else:	# Assign the Midi message to a Quisk control
      index = self.MidiAddNote()
      if index < 0:
        return
      text = ctrl.value
    self.list_ctrl.SetItem(index, 1, text)
    txt_note = self.list_ctrl.GetItem(index, 0).GetText()
    local_conf.MidiNoteDict[txt_note] = text
    local_conf.settings_changed = True
  def OnMidiListSelected(self, event):
    event.Skip()
    self.midi_edt.Clear()
    index = self.list_ctrl.GetFirstSelected()
    text = self.list_ctrl.GetItem(index, 1).GetText()
    if len(text) > 3 and text[-3] == ' ' and text[-2] in "+-" and text[-1] in '0123456789':	# Item is a jog wheel item
      self.midiJogWheel.ChangeSelection(text[-2:])
  def OnNewMidiNote(self, message):
    txt_note = "0x%02X%02X %d" % tuple(message)
    self.midi_edt.SetValue(txt_note)
    if txt_note[2] == '8':	# Note off. We already saw the Note On message.
      return
    txt_note = txt_note.split()[0]
    index = self.list_ctrl.FindItem(-1, txt_note)
    if index >= 0:
      self.list_ctrl.Select(index)
      self.list_ctrl.EnsureVisible(index)
    else:
      index = self.list_ctrl.GetFirstSelected()
      if index >= 0:
        self.list_ctrl.Select(index, 0)
  def OnChangePassword(self, event):
    text = local_conf.globals.get('remote_radio_password', '')
    dlg = wx.TextEntryDialog(self, "Please enter a password for remote access",
          "Password Entry", text)
    if dlg.ShowModal() == wx.ID_OK:
      local_conf.globals["remote_radio_password"] = dlg.GetValue()
      local_conf.settings_changed = True
      local_conf.SaveState()

class RadioHardwareBase(BaseWindow):		# The Hardware page in the second-level notebook for each radio
  def __init__(self, parent, radio_name):
    BaseWindow.__init__(self, parent)
    self.radio_name = radio_name
    self.num_cols = 8
    self.PMcalDialog = None
    #self.MarkCols()
  def AlwaysMakeControls(self):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    radio_type = radio_dict['hardware_file_type']
    data_names = local_conf.GetReceiverData(radio_type)
    self.AddTextL(1, "These are the hardware settings for a radio of type %s" % radio_type, self.num_cols-1)
    for name, text, fmt, help_text, values in data_names:
      if name == 'hardware_file_name':
        self.hware_path = self.GetValue(name, radio_dict)
        row = self.row
        self.row = 3
        item, self.hware_combo, btn = self.AddTextComboHelp(1, text, self.hware_path, values, help_text, False, span_text=1, span_combo=4)
        self.hware_combo.handler = self.OnButtonChangeHardware
        item = self.AddPushButtonR(7, "Change..", self.OnButtonChangeHardware, border=0)
      elif name == 'widgets_file_name':
        self.widgets_path = self.GetValue(name, radio_dict)
        row = self.row
        self.row = 5
        item, self.widgets_combo, btn = self.AddTextComboHelp(1, text, self.widgets_path, values, help_text, False, span_text=1, span_combo=4)
        self.widgets_combo.handler = self.OnButtonChangeWidgets
        item = self.AddPushButtonR(7, "Change..", self.OnButtonChangeWidgets, border=0)
    self.NextRow(7)
    self.AddColSpacer(2, 20)
    self.AddColSpacer(5, 20)
    self.SetScrollRate(1, 1)
  def OnButtonChangeHardware(self, event):
    if isinstance(event, (ChoiceCombo, ComboCtrl)):
      path = event.GetValue()
    else:
      direc, fname = os.path.split(self.hware_path)
      dlg = wx.FileDialog(None, "Choose Hardware File", direc, fname, "*.py", wx.FD_OPEN)
      if dlg.ShowModal() == wx.ID_OK:
        path = dlg.GetPath()
        self.hware_combo.SetText(path)
        dlg.Destroy()
      else:
        dlg.Destroy()
        return
    path = path.strip()
    self.hware_path = path
    local_conf.GetRadioDict(self.radio_name)["hardware_file_name"] = path
    local_conf.settings_changed = True
  def OnButtonChangeWidgets(self, event):
    if isinstance(event, (ChoiceCombo, ComboCtrl)):
      path = event.GetValue()
    else:
      direc, fname = os.path.split(self.widgets_path)
      dlg = wx.FileDialog(None, "Choose Widgets File", direc, fname, "*.py", wx.FD_OPEN)
      if dlg.ShowModal() == wx.ID_OK:
        path = dlg.GetPath()
        self.widgets_combo.SetText(path)
        dlg.Destroy()
      else:
        dlg.Destroy()
        return
    path = path.strip()
    self.widgets_path = path
    local_conf.GetRadioDict(self.radio_name)["widgets_file_name"] = path
    local_conf.settings_changed = True

class RadioHardware(RadioHardwareBase):		# The Hardware page in the second-level notebook for each radio
  def __init__(self, parent, radio_name):
    RadioHardwareBase.__init__(self, parent, radio_name)
    self.AlwaysMakeControls()
    self.HermesBias0 = None
    self.HermesBias1 = None
    radio_dict = local_conf.GetRadioDict(radio_name)
    radio_type = radio_dict['hardware_file_type']
    data_names = local_conf.GetReceiverData(radio_type)
    col = 1
    border = 2
    hermes_board_id = 0
    if radio_type == "Hermes":
      try:
        hermes_board_id = application.Hardware.hermes_board_id
      except:
        pass
    if radio_name == Settings[1] and hasattr(application.Hardware, "ProgramGateware"):
      help_text = "Choose an RBF file and program the Gateware (FPGA software) over Ethernet."
      self.AddTextButtonHelp(1, "Gateware Update", "Program from RBF file..", application.Hardware.ProgramGateware, help_text)
      col = 1
      self.NextRow(self.row + 2)
    for name, text, fmt, help_text, values in data_names:
      if name in ('hardware_file_name', 'widgets_file_name'):
        pass
      elif name[0:4] == platform_ignore:
        pass
      elif name in ('Hermes_BandDictEnTx', ):
        pass
      elif 'Hl2_' in name and hermes_board_id != 6:
        pass
      elif fmt[0:4] in ('dict', 'list'):
        pass
      else:
        if name[0:6] == 'Hware_':		# value comes from the hardware file
          value = application.Hardware.GetValue(name)
        else:
          value = self.GetValue(name, radio_dict)
        no_edit = "choice" in fmt or fmt == 'boolean'
        if name == 'power_meter_calib_name':
          values = self.PowerMeterCalChoices()
          txt, cb, btn = self.AddTextComboHelp(col, text, value, values, help_text, no_edit, border=border)
          cb.handler = self.OnButtonChangePMcal
          self.power_meter_cal_choices = cb
        else:
          txt, cb, btn = self.AddTextComboHelp(col, text, value, values, help_text, no_edit, border=border)
          if name[0:6] == 'Hware_':
            cb.handler = application.Hardware.SetValue
          else:
            cb.handler = self.OnChange
        cb.quisk_data_name = name
        if col == 1:
          col = 4
          border = 0
        else:
          col = 1
          border = 2
          self.NextRow()
    if hermes_board_id == 6:
      if col == 4:
        self.NextRow()
      help_text = ('This controls the bias level for transistors in the final power amplifier.  Enter a level from 0 to 255.'
      '  These changes are temporary.  Press the "Write" button to write the value to the hardware and make it permanent.')
      ## Bias is 0 indexed to match schematic
      txt, self.HermesBias0, btn = self.AddTextSpinnerHelp(1, "Power amp bias 0", 0, 0, 255, help_text)
      txt, self.HermesBias1, btn = self.AddTextSpinnerHelp(4, "Power amp bias 1", 0, 0, 255, help_text)
      enbl = radio_dict["hermes_bias_adjust"] == "True"
      self.HermesBias0.Enable(enbl)
      self.HermesBias1.Enable(enbl)
      self.HermesBias0.Bind(wx.EVT_SPINCTRL, self.OnHermesChangeBias0)
      self.HermesBias1.Bind(wx.EVT_SPINCTRL, self.OnHermesChangeBias1)
      self.HermesWriteBiasButton = self.AddPushButton(7, "Write", self.OnButtonHermesWriteBias, border=0)
      self.HermesWriteBiasButton.Enable(enbl)
    self.FitInside()
    self.SetScrollRate(1, 1)
  def OnHermesChangeBias0(self, event):
    value = self.HermesBias0.GetValue()
    application.Hardware.ChangeBias0(value)
  def OnHermesChangeBias1(self, event):
    value = self.HermesBias1.GetValue()
    application.Hardware.ChangeBias1(value)
  def OnButtonHermesWriteBias(self, event):
    value0 = self.HermesBias0.GetValue()
    value1 = self.HermesBias1.GetValue()
    application.Hardware.WriteBias(value0, value1)
  def PowerMeterCalChoices(self):
    values = list(conf.power_meter_std_calibrations)		# known calibration names from the config file
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    values += list(radio_dict.get('power_meter_local_calibrations', {}))		# local calibrations
    values.sort()
    values.append('New')
    return values
  def OnButtonChangePMcal(self, ctrl):
    value = ctrl.GetValue()
    name = ctrl.quisk_data_name
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    local_cal = radio_dict.get('power_meter_local_calibrations', {})
    if value == 'New':
      if not self.PMcalDialog:
        self.PMcalDialog = QPowerMeterCalibration(self, list(local_cal))
    else:
      setattr(conf, name, value)
      radio_dict[name] = value
      local_conf.settings_changed = True
      application.Hardware.MakePowerCalibration()
  def ChangePMcalFinished(self, name, table):
    self.PMcalDialog = None
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    local_cal = radio_dict.get('power_meter_local_calibrations', {})
    if name is None:        # Cancel
      name = conf.power_meter_calib_name
      values = self.PowerMeterCalChoices()
    else:
      if table is None:		# delete name
        del local_cal[name]
        name = list(conf.power_meter_std_calibrations)[0]      # replacement name
      else:     # new entry
        local_cal[name] = table
      conf.power_meter_calib_name = name
      radio_dict['power_meter_calib_name'] = name
      radio_dict['power_meter_local_calibrations'] = local_cal
      local_conf.settings_changed = True
      values = self.PowerMeterCalChoices()
      self.power_meter_cal_choices.SetItems(values)
      application.Hardware.MakePowerCalibration()
    try:
      index = values.index(name)
    except:
      index = 0
    self.power_meter_cal_choices.SetSelection(index)

class RadioHardwareSoapySDR(RadioHardwareBase):	# The Hardware page in the second-level notebook for the SoapySDR radios
  name_text = {
'soapy_gain_mode_rx' : 'Rx gain mode',
'soapy_setAntenna_rx' : 'Rx antenna name',
'soapy_setBandwidth_rx' : 'Rx bandwidth kHz',
'soapy_setSampleRate_rx' : 'Rx sample rate kHz',
'soapy_device' : 'Device name',
'soapy_gain_mode_tx' : 'Tx gain mode',
'soapy_setAntenna_tx' : 'Tx antenna name',
'soapy_setBandwidth_tx' : 'Tx bandwidth kHz',
'soapy_setSampleRate_tx' : 'Tx sample rate kHz',
}

  help_text = {
'soapy_gain_mode_rx' : 'Choose "total" to set the total gain, "detailed" to set multiple gain elements individually, \
or "automatic" for automatic gain control. The "detailed" or "automatic" may not be available depending on your hardware.',

'soapy_setAntenna_rx' : 'Choose the antenna to use for receive.',

'soapy_device' : "SoapySDR provides an interface to various radio hardware. The device name specifies \
the hardware device. Create a new radio for each hardware you have. Changing the device \
name requires re-entering all the hardware settings because different hardware has \
different settings. Also, the hardware device must be turned on when you change the \
device name so that Quisk can read the available settings.",

'soapy_gain_mode_tx' : 'Choose "total" to set the total gain, "detailed" to set multiple gain elements individually, \
or "automatic" for automatic gain control. The "detailed" or "automatic" may not be available depending on your hardware.',

'soapy_setAntenna_tx' : 'Choose the antenna to use for transmit.',

}
  def __init__(self, parent, radio_name):
    RadioHardwareBase.__init__(self, parent, radio_name)
    self.no_device = "No device specified"
    if soapy:
      self.AlwaysMakeControls()
      self.MakeSoapyControls()
    else:
      radio_dict = local_conf.GetRadioDict(self.radio_name)
      radio_type = radio_dict['hardware_file_type']
      self.AddTextL(1, "These are the hardware settings for a radio of type %s" % radio_type, self.num_cols-1)
      self.NextRow()
      self.AddTextL(1, "The shared library from the SoapySDR project is not available.")
      self.NextRow()
      self.AddTextL(1, "The shared library is not installed or is not compatible (perhaps 32 versus 64 bit versions).")
      self.NextRow()
      return
    #self.MarkCols()
  def NextCol(self):
    if self.col == 1:
      self.col = 4
      self.border = 0
    else:
      self.col = 1
      self.border = 2
      self.NextRow()
  def MakeSoapyControls(self):
    self.gains_rx = []
    self.gains_tx = []
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    local_conf.InitSoapyNames(radio_dict)
    self.border = 2
    name = 'soapy_device'
    device = radio_dict.get(name, self.no_device)
    txt, self.edit_soapy_device, btn = self.AddTextEditHelp(1, self.name_text[name], device, self.help_text[name], span1=1, span2=4)
    self.AddPushButtonR(7, "Change..", self.OnButtonChangeSoapyDevice, border=0)
    self.NextRow()
    self.NextRow()
    self.col = 1
    if device == self.no_device:
      self.FitInside()
      return

    if radio_dict.get("soapy_file_version", 0) < soapy_software_version:
      text = "Please re-enter the device name. This will read additional parameters from the hardware."
      self.AddTextL(self.col, text, span=6)
      self.FitInside()
      return

    # Receive parameters
    name = 'soapy_setSampleRate_rx'
    help_text = 'Available sample rates: '
    rates = ['48', '50', '240', '250', '960', '1000']
    for dmin, dmax, dstep in radio_dict.get('soapy_getSampleRateRange_rx', ()):
      tmin = FormatKhz(dmin * 1E-3)
      if tmin not in rates:
        rates.append(tmin)
      if abs(dmin - dmax) < 0.5:
        help_text = help_text + '%s; ' % tmin
      elif dstep < 0.5:
        help_text = help_text + '%s to %s; ' % (tmin, FormatKhz(dmax * 1E-3))
      else:
        help_text = help_text + '%s to %s by %s; ' % (tmin, FormatKhz(dmax * 1E-3), FormatKhz(dstep * 1E-3))
    help_text = help_text[0:-2] + '.'
    if rates:
      rates.sort(key=SortKey)
      rate = radio_dict.get(name, '')
      txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], rate, rates, help_text, False, border=self.border)
      cb.handler = self.OnChange
      cb.quisk_data_name = name
      self.NextCol()

    len_gain_names = len(radio_dict.get('soapy_listGainsValues_rx', ()))
    name = 'soapy_gain_mode_rx'
    gain_mode = radio_dict[name]
    choices = ['total']
    if len_gain_names >= 3:
      choices.append('detailed')
    if radio_dict.get('soapy_hasGainMode_rx', 0):
      choices.append('automatic')
    if gain_mode not in choices:
      gain_mode = radio_dict[name] = 'total'
      local_conf.settings_changed = True
    txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], gain_mode, choices, self.help_text[name], True, border=self.border)
    cb.handler = self.OnChange
    cb.quisk_data_name = name
    self.NextCol()

    name = 'soapy_gain_values_rx'
    values = radio_dict[name]
    for name2, dmin, dmax, dstep in radio_dict.get('soapy_listGainsValues_rx', ()):
      if dstep < 1E-4:
        dstep = 0.5
      text = "Rx gain %s" % name2
      help_text = 'Rf gain min %f, max %f, step %f' % (dmin, dmax, dstep)
      value = values.get(name2, '0')
      value = float(value)
      txt, spn, btn = self.AddTextDblSpinnerHelp(self.col, text, value, dmin, dmax, dstep, help_text, border=self.border)
      spn.quisk_data_name = name
      spn.quisk_data_name2 = name2
      spn.Bind(wx.EVT_SPINCTRLDOUBLE, self.OnGain)
      self.gains_rx.append(spn)
      self.NextCol()
      if len_gain_names < 3:	# for 1 or 2 names, just show total gain item
        break
    self.FixGainButtons('soapy_gain_mode_rx')

    name = 'soapy_setAntenna_rx'
    antenna = radio_dict[name]
    antennas = radio_dict.get('soapy_listAntennas_rx', ())
    if antenna not in antennas:
      if antennas:
        antenna = antennas[0]
      else:
        antenna = ''
      radio_dict[name] = antenna
      local_conf.settings_changed = True
    if antennas:
      txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], antenna, antennas, self.help_text[name], True, border=self.border)
      cb.handler = self.OnChange
      cb.quisk_data_name = name
      self.NextCol()

    name = 'soapy_setBandwidth_rx'
    help_text = 'Available bandwidth: '
    bandwidths = []
    for dmin, dmax, dstep in radio_dict.get('soapy_getBandwidthRange_rx', ()):
      tmin = FormatKhz(dmin * 1E-3)
      bandwidths.append(tmin)
      if abs(dmin - dmax) < 0.5:
        help_text = help_text + '%s; ' % tmin
      elif dstep < 0.5:
        help_text = help_text + '%s to %s; ' % (tmin, FormatKhz(dmax * 1E-3))
      else:
        help_text = help_text + '%s to %s by %s; ' % (tmin, FormatKhz(dmax * 1E-3), FormatKhz(dstep * 1E-3))
    help_text = help_text[0:-2] + '.'
    if bandwidths:
      bandwidth = radio_dict.get(name, '')
      txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], bandwidth, bandwidths, help_text, False, border=self.border)
      cb.handler = self.OnChange
      cb.quisk_data_name = name
      self.NextCol()

    # Transmit parameters
    if self.col != 1:
      self.NextCol()
    name = 'soapy_enable_tx'
    enable = radio_dict.get(name, 'Disable')
    help_text = 'This will enable or disable the transmit function. If changed, you must restart Quisk.'
    txt, cb, btn = self.AddTextComboHelp(self.col, 'Tx enable', enable, ['Enable', 'Disable'], help_text, True, border=self.border)
    cb.handler = self.OnChange
    cb.quisk_data_name = name
    self.NextCol()

    name = 'soapy_setSampleRate_tx'
    help_text = 'Available sample rates: '
    rates = []
    for dmin, dmax, dstep in radio_dict.get('soapy_getSampleRateRange_tx', ()):
      tmin = FormatKhz(dmin * 1E-3)
      rates.append(tmin)
      if abs(dmin - dmax) < 0.5:
        help_text = help_text + '%s; ' % tmin
      elif dstep < 0.5:
        help_text = help_text + '%s to %s; ' % (tmin, FormatKhz(dmax * 1E-3))
      else:
        help_text = help_text + '%s to %s by %s; ' % (tmin, FormatKhz(dmax * 1E-3), FormatKhz(dstep * 1E-3))
    help_text = help_text[0:-2] + '.'
    if rates:
      rate = radio_dict.get(name, '')
      rates = ('48', '50', '96', '100', '192')
      txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], rate, rates, help_text, True, border=self.border)
      cb.handler = self.OnChange
      cb.quisk_data_name = name
      self.NextCol()

    len_gain_names = len(radio_dict.get('soapy_listGainsValues_tx', ()))
    name = 'soapy_gain_mode_tx'
    gain_mode = radio_dict[name]
    choices = ['total']
    if len_gain_names >= 3:
      choices.append('detailed')
    if radio_dict.get('soapy_hasGainMode_tx', 0):
      choices.append('automatic')
    if gain_mode not in choices:
      gain_mode = radio_dict[name] = 'total'
      local_conf.settings_changed = True
    txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], gain_mode, choices, self.help_text[name], True, border=self.border)
    cb.handler = self.OnChange
    cb.quisk_data_name = name
    self.NextCol()

    name = 'soapy_gain_values_tx'
    values = radio_dict[name]
    for name2, dmin, dmax, dstep in radio_dict.get('soapy_listGainsValues_tx', ()):
      if dstep < 1E-4:
        dstep = 0.5
      text = "Tx gain %s" % name2
      help_text = 'Rf gain min %f, max %f, step %f' % (dmin, dmax, dstep)
      value = values.get(name2, '0')
      value = float(value)
      txt, spn, btn = self.AddTextDblSpinnerHelp(self.col, text, value, dmin, dmax, dstep, help_text, border=self.border)
      spn.quisk_data_name = name
      spn.quisk_data_name2 = name2
      spn.Bind(wx.EVT_SPINCTRLDOUBLE, self.OnGain)
      self.gains_tx.append(spn)
      self.NextCol()
      if len_gain_names < 3:	# for 1 or 2 names, just show total gain item
        break
    self.FixGainButtons('soapy_gain_mode_tx')

    name = 'soapy_setAntenna_tx'
    antenna = radio_dict[name]
    antennas = radio_dict.get('soapy_listAntennas_tx', ())
    if antenna not in antennas:
      if antennas:
        antenna = antennas[0]
      else:
        antenna = ''
      radio_dict[name] = antenna
      local_conf.settings_changed = True
    if antennas:
      txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], antenna, antennas, self.help_text[name], True, border=self.border)
      cb.handler = self.OnChange
      cb.quisk_data_name = name
      self.NextCol()

    name = 'soapy_setBandwidth_tx'
    help_text = 'Available bandwidths: '
    bandwidths = []
    for dmin, dmax, dstep in radio_dict.get('soapy_getBandwidthRange_tx', ()):
      tmin = FormatKhz(dmin * 1E-3)
      bandwidths.append(tmin)
      if abs(dmin - dmax) < 0.5:
        help_text = help_text + '%s; ' % tmin
      elif dstep < 0.5:
        help_text = help_text + '%s to %s; ' % (tmin, FormatKhz(dmax * 1E-3))
      else:
        help_text = help_text + '%s to %s by %s; ' % (tmin, FormatKhz(dmax * 1E-3), FormatKhz(dstep * 1E-3))
    help_text = help_text[0:-2] + '.'
    if bandwidths:
      bandwidth = radio_dict.get(name, '')
      txt, cb, btn = self.AddTextComboHelp(self.col, self.name_text[name], bandwidth, bandwidths, help_text, False, border=self.border)
      cb.handler = self.OnChange
      cb.quisk_data_name = name
      self.NextCol()

    self.FitInside()
  def FixGainButtons(self, name):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    gain_mode = radio_dict[name]
    if name[-3:] == '_tx':
      controls = self.gains_tx
    else:
      controls = self.gains_rx
    for i in range(len(controls)):
      ctrl = controls[i]
      if gain_mode == "automatic":
        ctrl.Enable(False)
      elif gain_mode == "total":
        if i == 0:
          ctrl.Enable(True)
        else:
          ctrl.Enable(False)
      else:	# gain_mode is "detailed"
        if i == 0:
          ctrl.Enable(False)
        else:
          ctrl.Enable(True)
  def OnButtonChangeSoapyDevice(self, event):
    if not soapy:
      txt = "Soapy shared library (DLL) is not available."
      msg = wx.MessageDialog(None, txt, 'SoapySDR Error', wx.OK|wx.ICON_ERROR)
      msg.ShowModal()
      msg.Destroy()
      return
    try:
      choices = self.GetSoapyDevices()
    except:
      #traceback.print_exc()
      choices = []
    if not choices:
      choices = ['No devices were found.']
    device = self.edit_soapy_device.GetValue()
    width = application.main_frame.GetSize().width
    width = width * 50 // 100
    parent = self.edit_soapy_device.GetParent()
    dlg = ListEditDialog(parent, "Change Soapy Device", device, choices, width)
    ok = dlg.ShowModal()
    if ok != wx.ID_OK:
      dlg.Destroy()
      return
    device = dlg.GetValue()
    dlg.Destroy()
    if device == self.no_device:
      return
    if Settings[1] == self.radio_name:
      txt = "Changing the active radio requires a shutdown and restart. Proceed?"
      msg = wx.MessageDialog(None, txt, 'SoapySDR Change to Active Radio', wx.OK|wx.CANCEL|wx.ICON_INFORMATION)
      ok = msg.ShowModal()
      msg.Destroy()
      if ok == wx.ID_OK:
        soapy.close_device(1)
      else:
        return
    txt = soapy.open_device(device, 0, 0)
    if txt[0:8] == 'Capture ':
      radio_dict = local_conf.GetRadioDict(self.radio_name)
      radio_dict['soapy_device'] = device
      radio_dict['soapy_file_version'] = soapy_software_version
      self.edit_soapy_device.ChangeValue(device)
      # Record the new SoapySDR parameters for the new device. Do not change the old data values yet.
      for name in ('soapy_listAntennas_rx', 'soapy_hasGainMode_rx', 'soapy_listGainsValues_rx',
                   'soapy_listAntennas_tx', 'soapy_hasGainMode_tx', 'soapy_listGainsValues_tx',
	           'soapy_getFullDuplex_rx', 'soapy_getSampleRateRange_rx', 'soapy_getSampleRateRange_tx',
                   'soapy_getBandwidthRange_rx', 'soapy_getBandwidthRange_tx',
                  ):
        radio_dict[name] = soapy.get_parameter(name, 0)
      soapy.close_device(0)
      local_conf.settings_changed = True
      # Clear our sizer and re-create all the controls
      self.gbs.Clear(True)
      self.gbs.Add((self.charx, self.charx), (0, 0))
      self.row = 1
      RadioHardwareBase.AlwaysMakeControls(self)
      self.MakeSoapyControls()
      txt = "Please check the settings for the new hardware device."
      msg = wx.MessageDialog(None, txt, 'SoapySDR Change to Radio', wx.OK|wx.ICON_INFORMATION)
      msg.ShowModal()
      msg.Destroy()
    else:
      msg = wx.MessageDialog(None, txt, 'SoapySDR Device Error', wx.OK|wx.ICON_ERROR)
      msg.ShowModal()
      msg.Destroy()
  def GetSoapyDevices(self):
    choices = []
    for dct in soapy.get_device_list():
      text = ''
      try:
        driver = dct["driver"]
      except:
        pass
      else:
        text = 'driver=%s' % driver
      try:
        label = dct["label"]
      except:
        pass
      else:
        text = text + ', label=%s' % label
      choices.append(text)
    return choices
  def OnChange(self, ctrl):
    name = ctrl.quisk_data_name
    value = ctrl.GetValue()
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    radio_dict[name] = value
    local_conf.settings_changed = True
    # Immediate changes
    if name in ('soapy_gain_mode_rx', 'soapy_gain_mode_tx'):
      self.FixGainButtons(name)
    if soapy and self.radio_name == Settings[1]:	# changed for current radio
      application.Hardware.ImmediateChange(name, value)
  def OnGain(self, event):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    obj = event.GetEventObject()
    value = obj.GetValue()
    name = obj.quisk_data_name
    radio_dict[name][obj.quisk_data_name2] = value
    local_conf.settings_changed = True
    if soapy and self.radio_name == Settings[1]:	# changed for current radio
      application.Hardware.ChangeGain(name[-3:])

class RadioSound(BaseWindow):		# The Sound page in the second-level notebook for each radio
  """Configure the available sound devices."""
  sound_names = (		# same order as label_help
    ('playback_rate', '', '', '', 'name_of_sound_play'),
    ('mic_sample_rate', 'mic_channel_I', 'mic_channel_Q', '', 'microphone_name'),
    ('sample_rate', 'channel_i', 'channel_q', 'channel_delay', 'name_of_sound_capt'),
    ('mic_playback_rate', 'mic_play_chan_I', 'mic_play_chan_Q', 'tx_channel_delay', 'name_of_mic_play'),
    ('', '', '', '', 'sample_playback_name'),
    ('', '', '', '', 'digital_input_name'),
    ('', '', '', '', 'digital_output_name'),
    ('', '', '', '', 'digital_rx1_name'),
    ('', '', '', '', 'digital_rx2_name'),
    ('', '', '', '', 'digital_rx3_name'),
    ('', '', '', '', 'digital_rx4_name'),
    ('', '', '', '', 'digital_rx5_name'),
    ('', '', '', '', 'digital_rx6_name'),
    ('', '', '', '', 'digital_rx7_name'),
    ('', '', '', '', 'digital_rx8_name'),
    ('', '', '', '', 'digital_rx9_name'),
    )
  hRx0 = "This is almost the same as the radio sound, but it is sent to a digital mode program on a different sound device."\
" It is analog audio and it is only sent if one of the DGT- modes is selected."\
" The volume is set by the digital output level instead of the volume control so you can mute the speaker if desired."\
" It can receive a greater bandwidth than the 3000 Hz limit of SSB."
  hTx0 = "This is used instead of the microphone when one of the DGT- modes is selected."\
" The normal speech clipping and filtering is not used for DGT- modes."
  label_help = (	# Same order as sound_names
    (1, "Radio Sound Output",   "This is the radio sound going to the headphones or speakers."),
    (0, "Microphone Input",     "This is the monophonic microphone source.  Set the channel if the source is stereo."),
    (0, "I/Q Rx Sample Input",  "This is the sample source if it comes from a sound device, such as a SoftRock."),
    (1, "I/Q Tx Sample Output", "This is the transmit sample audio sent to a SoftRock."),
    (1, "Raw Digital Output",   "This sends the received I/Q data to another program as stereo."),
    (0, "Digital Tx0 Input",    hTx0),
    (1, "Digital Rx0 Output",   hRx0),
    (1, "Digital Rx1 Output",   "This is the sub-receiver 1 audio going to a digital mode program."),
    (1, "Digital Rx2 Output",   "This is the sub-receiver 2 audio going to a digital mode program."),
    (1, "Digital Rx3 Output",   "This is the sub-receiver 3 audio going to a digital mode program."),
    (1, "Digital Rx4 Output",   "This is the sub-receiver 4 audio going to a digital mode program."),
    (1, "Digital Rx5 Output",   "This is the sub-receiver 5 audio going to a digital mode program."),
    (1, "Digital Rx6 Output",   "This is the sub-receiver 6 audio going to a digital mode program."),
    (1, "Digital Rx7 Output",   "This is the sub-receiver 7 audio going to a digital mode program."),
    (1, "Digital Rx8 Output",   "This is the sub-receiver 8 audio going to a digital mode program."),
    (1, "Digital Rx9 Output",   "This is the sub-receiver 9 audio going to a digital mode program."),
    )
  def __init__(self, parent, radio_name):
    BaseWindow.__init__(self, parent)
    self.radio_name = radio_name
    self.MakeControls()
  def MakeControls(self):
    self.radio_dict = local_conf.GetRadioDict(self.radio_name)
    self.num_cols = 8
    for name, text, fmt, help_text, values in local_conf.GetSectionData('Sound'):
      if name in ('digital_output_level', 'file_play_level'):
        value = self.GetValue(name, self.radio_dict)
        no_edit = "choice" in fmt or fmt == 'boolean'
        txt, cb, btn = self.AddTextComboHelp(1, text, value, values, help_text, no_edit)
        cb.handler = self.OnChange
        cb.quisk_data_name = name
        self.NextRow()
    self.NextRow()
    # Add the grid for the sound settings
    sizer = wx.GridBagSizer(2, 2)
    sizer.SetEmptyCellSize((self.charx, self.charx))
    self.gbs.Add(sizer, (self.row, 0), span=(1, self.num_cols))
    gbs = self.gbs
    self.gbs = sizer
    self.row = 1
    help_chan = "For the usual stereo device enter 0 for the I channel and 1 for the Q channel. Reversing the 0 and 1 switches the left\
 and right channels. For a monophonic device, enter 0 for both I and Q. If you have more channels enter the channel number. Channels\
 are numbered from zero, so a four channel device has channels 0, 1, 2 and 3."
    self.AddTextC(1, "Stream")
    self.AddTextCHelp(2, "Rate",
"This is the sample rate for the device in Hertz." "Some devices have fixed rates that can not be changed.")
    self.AddTextCHelp(3, "Ch I", "This is the in-phase channel for devices with I/Q data. " + help_chan)
    self.AddTextCHelp(4, "Ch Q", "This is the quadrature channel for devices with I/Q data. " + help_chan)
    self.AddTextCHelp(5, "Delay", "Some older devices have a one sample channel delay between channels.  "
"This must be corrected for devices with I/Q data.  Enter the channel number to delay; either the I or Q channel number.  "
"For no delay, leave this blank.")
    self.AddTextCHelp(6, "Sound Device", "This is the name of the sound device.  For Windows, this is the Wasapi name.  "
"For Linux you can use the Alsa device, the PortAudio device or the PulseAudio device.  "
"The Alsa device are recommended because they have lower latency.  See the documentation for more information.")
    self.NextRow()
    choices = (("48000", "96000", "192000"), ("0", "1"), ("0", "1"), (" ", "0", "1"))
    r = 0
    if "SoftRock" in self.radio_dict['hardware_file_type']:
      softrock = True		# Samples come from sound card
    elif hasattr(application.Hardware, "use_softrock"):
      softrock = application.Hardware.use_softrock
    else:
      softrock = False
    last_row = 8
    for is_output, label, helptxt in self.label_help:
      self.AddTextLHelp(1, label, helptxt)
      # Add col 0
      value = self.ItemValue(r, 0)
      if value is None:
        value = ''
      data_name = self.sound_names[r][0]
      if r == 0:
        cb = self.AddComboCtrl(2, value, choices=("48000", "96000", "192000"), right=True)
      if r == 1:
        cb = self.AddComboCtrl(2, value, choices=("48000", "8000"), right=True, no_edit=True)
      if softrock:
        if r == 2:
          cb = self.AddComboCtrl(2, value, choices=("48000", "96000", "192000"), right=True)
        if r == 3:
          cb = self.AddComboCtrl(2, value, choices=("48000", "96000", "192000"), right=True)
      else:
        if r == 2:
          cb = self.AddComboCtrl(2, '', choices=("",), right=True)
          cb.Enable(False)
        if r == 3:
          cb = self.AddComboCtrl(2, '', choices=("",), right=True)
          cb.Enable(False)
      if r >= 4:
        cb = self.AddComboCtrl(2, "48000", choices=("48000",), right=True, no_edit=True)
        cb.Enable(False)
      cb.handler = self.OnChange
      cb.quisk_data_name = data_name
      # Add col 1, 2, 3
      for col in range(1, 4):
        value = self.ItemValue(r, col)
        data_name = self.sound_names[r][col]
        if value is None:
          cb = self.AddComboCtrl(col + 2, ' ', choices=[], right=True)
          cb.Enable(False)
        else:
          cb = self.AddComboCtrl(col + 2, value, choices=choices[col], right=True)
        cb.handler = self.OnChange
        cb.quisk_data_name = self.sound_names[r][col]
      # Add col 4
      if not softrock and r in (2, 3):
        cb = self.AddComboCtrl(6, self.ItemValue(r, 4), choices=[''])
      elif is_output:
        if label == "Digital Rx0 Output" and sys.platform != 'win32':
          play_names = application.dev_play[:]
          play_names += ["pulse: Use name QuiskDigitalOutput.monitor"]
        else:
          play_names = application.dev_play
        cb = self.AddComboCtrl(6, self.ItemValue(r, 4), choices=play_names)
      else:
        if label == "Digital Tx0 Input" and sys.platform != 'win32':
          capt_names = application.dev_capt[:]
          capt_names += ["pulse: Use name QuiskDigitalInput"]
        else:
          capt_names = application.dev_capt
        cb = self.AddComboCtrl(6, self.ItemValue(r, 4), choices=capt_names)
      cb.handler = self.OnChange
      cb.quisk_data_name = platform_accept + self.sound_names[r][4]
      self.NextRow()
      r += 1
      if r >= last_row:
        break
    self.gbs = gbs
    self.FitInside()
    self.SetScrollRate(1, 1)
  def ItemValue(self, row, col):
    data_name = self.sound_names[row][col]
    if col == 4:		# Device names
      data_name = platform_accept + data_name
      value = self.GetValue(data_name, self.radio_dict)
      return value
    elif data_name:
      value = self.GetValue(data_name, self.radio_dict)
      if col == 3:		# Delay
        if value == "-1":
          value = ''
      return value
    return None
  def OnChange(self, ctrl):
    data_name = ctrl.quisk_data_name
    value = ctrl.GetValue()
    #index = ctrl.ctrl.lbox.GetSelections()[0]
    #print (value, index, ctrl.choices[index])
    if data_name in ('channel_delay', 'tx_channel_delay'):
      value = value.strip()
      if not value:
        value = "-1"
    self.OnChange2(ctrl, value)

class RadioBands(BaseWindow):		# The Bands page in the second-level notebook for each radio
  def __init__(self, parent, radio_name):
    BaseWindow.__init__(self, parent)
    self.radio_name = radio_name
    self.parent = parent
    self.MakeControls()
  def MakeControls(self):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    radio_type = radio_dict['hardware_file_type']
    self.num_cols = 8
    #self.MarkCols()
    self.NextRow()
    self.AddTextCHelp(1, "Bands",
"This is a list of the bands that Quisk understands.  A check mark means that the band button is displayed.  A maximum of "
"14 bands may be displayed.")
    self.AddTextCHelp(2, "    Start MHz",
"This is the start of the band in megahertz.")
    self.AddTextCHelp(3, "    End MHz",
"This is the end of the band in megahertz.")
    heading_row = self.row
    self.NextRow()
    band_labels = radio_dict['bandLabels'][:]
    for i in range(len(band_labels)):
      if isinstance(band_labels[i], (list, tuple)):
        band_labels[i] = band_labels[i][0]
    band_edge = radio_dict['BandEdge']
    # band_list is a list of all known bands
    band_list = conf.BandList[:]
    band_list.append('Time')
    if local_conf.ReceiverHasName(radio_type, 'tx_level'):	# Must show and edit tx_level
      if 'tx_level' in radio_dict:
        tx_level = radio_dict['tx_level']
      else:
        tx_level = {}
        radio_dict['tx_level'] = {}
        local_conf.settings_changed = True
    else:
      tx_level = None
    try:
      transverter_offset = radio_dict['bandTransverterOffset']
    except:
      transverter_offset = {}
      radio_dict['bandTransverterOffset'] = transverter_offset     # Make sure the dictionary is in radio_dict
    try:
      hiqsdr_bus = radio_dict['HiQSDR_BandDict']
    except:
      hiqsdr_bus = None
    try:
      hermes_bus = radio_dict['Hermes_BandDict']
    except:
      hermes_bus = None
    self.band_checks = []
    # Add the Audio band.  This must be first to allow for column labels.
    cb = self.AddCheckBox(1, 'Audio', self.OnChangeBands)
    self.band_checks.append(cb)
    if 'Audio' in band_labels:
      cb.SetValue(True)
    self.NextRow()
    start_row = self.row
    # Add check box, start, end
    for band in band_list:
      cb = self.AddCheckBox(1, band, self.OnChangeBands)
      self.band_checks.append(cb)
      if band in band_labels:
        cb.SetValue(True)
      try:
        start, end = band_edge[band]
        start = "%.3f" % (start * 1E-6)
        end = "%.3f" % (end * 1E-6)
      except:
        try:
          start, end = local_conf.originalBandEdge[band]
          start = "%.3f" % (start * 1E-6)
          end = "%.3f" % (end * 1E-6)
        except:
          start = ''
          end = ''
      cb = self.AddComboCtrl(2, start, choices=(start, ), right=True)
      cb.handler = self.OnChangeBandStart
      cb.quisk_band = band
      cb = self.AddComboCtrl(3, end, choices=(end, ), right=True)
      cb.handler = self.OnChangeBandEnd
      cb.quisk_band = band
      self.NextRow()
    col = 3
    # Add tx_level
    if tx_level is not None:
      col += 1
      self.row = heading_row
      text, help_text = local_conf.GetReceiverItemTH(radio_type, 'tx_level')
      self.AddTextCHelp(col, "    %s" % text, help_text)
      self.row = start_row
      for band in band_list:
        try:
          level = tx_level[band]
          level = str(level)
        except:
          try:
            level = tx_level[None]
            tx_level[band] = level      # Fill in tx_level for each band
            level = str(level)
          except:
            tx_level[band] = 70
            level = '70'
        cb = self.AddComboCtrl(col, level, choices=(level, ), right=True)
        cb.handler = self.OnChangeDict
        cb.quisk_data_name = 'tx_level'
        cb.quisk_band = band
        self.NextRow()
    # Add transverter offset
    if isinstance(transverter_offset, dict):
      col += 1
      self.row = heading_row
      self.AddTextCHelp(col, "    Transverter Offset",
"If you use a transverter, you need to tune your hardware to a frequency lower than\
 the frequency displayed by Quisk.  For example, if you have a 2 meter transverter,\
 you may need to tune your hardware from 28 to 30 MHz to receive 144 to 146 MHz.\
 Enter the transverter offset in Hertz.  For this to work, your\
 hardware must support it.  Currently, the HiQSDR, SDR-IQ and SoftRock are supported.")
      self.row = start_row
      for band in band_list:
        try:
          offset = transverter_offset[band]
        except:
          offset = ''
        else:
          offset = str(offset)
        cb = self.AddComboCtrl(col, offset, choices=(offset, ), right=True)
        cb.handler = self.OnChangeDictBlank
        cb.quisk_data_name = 'bandTransverterOffset'
        cb.quisk_band = band
        self.NextRow()
    # Add hiqsdr_bus
    if hiqsdr_bus is not None:
      bus_text = 'The IO bus is used to select filters for each band.  Refer to the documentation for your filter board to see what number to enter.'
      col += 1
      self.row = heading_row
      self.AddTextCHelp(col, "    IO Bus", bus_text)
      self.row = start_row
      for band in band_list:
        try:
          bus = hiqsdr_bus[band]
        except:
          bus = ''
          bus_choice = ('11', )
        else:
          bus = str(bus)
          bus_choice = (bus, )
        cb = self.AddComboCtrl(col, bus, bus_choice, right=True)
        cb.handler = self.OnChangeDict
        cb.quisk_data_name = 'HiQSDR_BandDict'
        cb.quisk_band = band
        self.NextRow()
    # Add hermes_bus
    if hermes_bus is not None:
      rx_bus_text = 'The IO bus is used to select filters for each band. Check the bit for a "1", and uncheck the bit for a "0".\
 Bits are shown in binary number order. For example, decimal 9 is 0b1001, so check bits 3 and 0.\
 Changes are immediate (no need to restart).\
 Refer to the documentation for your filter board to see which bits to set. For the Hermes Lite 2 N2ADR filter set:\n\n\
160:  0000001\n\
80:    1000010\n\
60:    1000100\n\
40:    1000100\n\
30:    1001000\n\
20:    1001000\n\
17:    1010000\n\
15:    1010000\n\
12:    1100000\n\
10:    1100000\n\
\n\
If multiple receivers are in use, the Rx filter will be that of the highest frequency band.\
 The Rx bits are sent as the "Alex" filters (Protocol 1, address 9) and are sent to the J16 interface.'
      tx_bus_text = 'The Rx bits are used for both receive and transmit unless the "Enable" box is checked.\
 Then you can specify different filters for Rx and Tx.\n\n\
The Tx bits are sent as the "Alex" filters and are sent to the J16 interface.'
      col += 1
      self.row = heading_row
      self.AddTextCHelp(col, " Rx IO Bus", rx_bus_text)
      self.AddTextCHelp(col + 1, " Tx IO Bus", tx_bus_text)
      self.row += 1
      self.AddTextC(col, "6...Bits...0")
      btn = self.AddCheckBox(col + 1, "  Enable", self.ChangeIOTxEnable, flag=wx.ALIGN_CENTER|wx.ALIGN_CENTER_VERTICAL)
      value = self.GetValue("Hermes_BandDictEnTx", radio_dict)
      value = value == 'True'
      btn.SetValue(value)
      self.row = start_row
      try:
        hermes_tx_bus = radio_dict['Hermes_BandDictTx']
      except:
        hermes_tx_bus = {}
      for band in band_list:
        try:
          bus = int(hermes_bus[band])
        except:
          bus = 0
        self.AddBitField(col, 7, 'Hermes_BandDict', band, bus, self.ChangeIO)
        try:
          bus = int(hermes_tx_bus[band])
        except:
          bus = 0
        self.AddBitField(col + 1, 7, 'Hermes_BandDictTx', band, bus, self.ChangeIO)
        self.NextRow()
    self.FitInside()
    self.SetScrollRate(1, 1)
  def SortCmp(self, item1):
    # Numerical conversion of band name to  megahertz
    try:
      if item1[-2:] == 'cm':
        item1 = float(item1[0:-2]) * .01
        item1 = 300.0 / item1
      elif item1[-1] == 'k':
        item1 = float(item1[0:-1]) * .001
      else:
        item1 = float(item1)
        item1 = 300.0 / item1
    except:
      item1 = 50000.0
    return item1
  def OnChangeBands(self, ctrl):
    band_list = []
    count = 0
    for cb in self.band_checks:
      if cb.IsChecked():
        band = cb.GetLabel()
        count += 1
        if band == '60' and len(conf.freq60) > 1:
          band_list.append(('60', ) * len(conf.freq60))
        elif band == 'Time' and len(conf.bandTime) > 1:
          band_list.append(('Time', ) * len(conf.bandTime))
        else:
          band_list.append(band)
    if count > 14:
      dlg = wx.MessageDialog(None,
        "There are more than the maximum of 14 bands checked.  Please remove some checks.",
        'List of Bands', wx.OK|wx.ICON_ERROR)
      dlg.ShowModal()
      dlg.Destroy()
    else:
      radio_dict = local_conf.GetRadioDict(self.radio_name)
      radio_dict['bandLabels'] = band_list
      local_conf.settings_changed = True
  def OnChangeBandStart(self, ctrl):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    band_edge = radio_dict['BandEdge']
    band = ctrl.quisk_band
    start, end = band_edge.get(band, (0, 9999))
    value = ctrl.GetValue()
    if self.FormatOK(value, 'numb'):
      start = int(float(value) * 1E6 + 0.1)
      band_edge[band] = (start, end)
      local_conf.settings_changed = True
  def OnChangeBandEnd(self, ctrl):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    band_edge = radio_dict['BandEdge']
    band = ctrl.quisk_band
    start, end = band_edge.get(band, (0, 9999))
    value = ctrl.GetValue()
    if self.FormatOK(value, 'numb'):
      end = int(float(value) * 1E6 + 0.1)
      band_edge[band] = (start, end)
      local_conf.settings_changed = True
  def OnChangeDict(self, ctrl):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    dct = radio_dict[ctrl.quisk_data_name]
    band = ctrl.quisk_band
    value = ctrl.GetValue()
    if self.FormatOK(value, 'inte'):
      value = int(value)
      dct[band] = value
      local_conf.settings_changed = True
      if ctrl.quisk_data_name == 'tx_level' and hasattr(application.Hardware, "SetTxLevel"):
        application.Hardware.SetTxLevel()
  def OnChangeDictBlank(self, ctrl):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    dct = radio_dict[ctrl.quisk_data_name]
    band = ctrl.quisk_band
    value = ctrl.GetValue()
    value = value.strip()
    if not value:
      if band in dct:
        del dct[band]
        local_conf.settings_changed = True
    elif self.FormatOK(value, 'inte'):
      value = int(value)
      dct[band] = value
      local_conf.settings_changed = True
  def ChangeIO(self, control):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    dct = radio_dict[control.quisk_data_name]
    band = control.quisk_band
    dct[band] = control.value
    local_conf.settings_changed = True
    if hasattr(application.Hardware, "ChangeBandFilters"):
      application.Hardware.ChangeBandFilters()
  def ChangeIOTxEnable(self, event):
    name = "Hermes_BandDictEnTx"
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    if event.IsChecked():
      radio_dict[name] = "True"
      setattr(conf, name, True)
    else:
      radio_dict[name] = "False"
      setattr(conf, name, False)
    local_conf.settings_changed = True
    if hasattr(application.Hardware, "ImmediateChange"):
      application.Hardware.ImmediateChange(name)

class xxRadioFilters(BaseWindow):		# The Filters page in the second-level notebook for each radio
  def __init__(self, parent, radio_name):
    BaseWindow.__init__(self, parent)
    self.radio_name = radio_name
    self.MakeControls()
  def MakeControls(self):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    self.num_cols = 8
    self.NextRow()
    bus_text = 'These high-pass and low-pass filters are only available for radios that support the Hermes protocol.\
  Enter a frequency range and the control bits for that range. Leave the frequencies blank for unused ranges.\
  Place whole bands within the frequency ranges because filters are only changed when changing bands.\
  Check the bit for a "1", and uncheck the bit for a "0".\
  Bits are shown in binary number order.  For example, decimal 9 is 0b1001, so check bits 3 and 0.\
  Changes are immediate (no need to restart).\
  Refer to the documentation for your filter board to see which bits to set.\
  The Rx bits are used for both receive and transmit, unless the "Tx Enable" box is checked.\
  Then you can specify different filters for Rx and Tx.\
  If multiple receivers are in use, the filters will accommodate the highest and lowest frequencies of all receivers.'
    self.AddTextCHelp(1, 'Hermes Protocol: Alex High and Low Pass Filters', bus_text, span=self.num_cols)
    self.NextRow()
    self.AddTextC(1, 'Start MHz')
    self.AddTextC(2, 'End MHz')
    self.AddTextC(3, "Alex HPF Rx")
    btn = self.AddCheckBox(4, "Alex HPF Tx", self.ChangeEnable, flag=wx.ALIGN_CENTER|wx.ALIGN_CENTER_VERTICAL)
    btn.quisk_data_name = "AlexHPF_TxEn"
    value = self.GetValue("AlexHPF_TxEn", radio_dict)
    value = value == 'True'
    btn.SetValue(value)
    self.AddTextC(5, 'Start MHz')
    self.AddTextC(6, 'End MHz')
    self.AddTextC(7, "Alex LPF Rx")
    btn = self.AddCheckBox(8, "Alex LPF Tx", self.ChangeEnable, flag=wx.ALIGN_CENTER|wx.ALIGN_CENTER_VERTICAL)
    btn.quisk_data_name = "AlexLPF_TxEn"
    value = self.GetValue("AlexLPF_TxEn", radio_dict)
    value = value == 'True'
    btn.SetValue(value)
    self.NextRow()
    hp_filters = self.GetValue("AlexHPF", radio_dict)
    lp_filters = self.GetValue("AlexLPF", radio_dict)
    row = self.row
    for index in range(len(hp_filters)):
      f1, f2, rx, tx = hp_filters[index]	# f1 and f2 are strings; rx and tx are integers
      cb = self.AddTextCtrl(1, f1, self.OnChangeFreq)
      cb.quisk_data_name = "AlexHPF"
      cb.index = (index, 0)
      cb = self.AddTextCtrl(2, f2, self.OnChangeFreq)
      cb.quisk_data_name = "AlexHPF"
      cb.index = (index, 1)
      bf = self.AddBitField(3, 8, 'AlexHPF', None, rx, self.ChangeBits)
      bf.index = (index, 2)
      bf = self.AddBitField(4, 8, 'AlexHPF', None, tx, self.ChangeBits)
      bf.index = (index, 3)
      self.NextRow()
      index += 1
    self.row = row
    for index in range(len(lp_filters)):
      f1, f2, rx, tx = lp_filters[index]	# f1 and f2 are strings; rx and tx are integers
      cb = self.AddTextCtrl(5, f1, self.OnChangeFreq)
      cb.quisk_data_name = "AlexLPF"
      cb.index = (index, 0)
      cb = self.AddTextCtrl(6, f2, self.OnChangeFreq)
      cb.quisk_data_name = "AlexLPF"
      cb.index = (index, 1)
      bf = self.AddBitField(7, 8, 'AlexLPF', None, rx, self.ChangeBits)
      bf.index = (index, 2)
      bf = self.AddBitField(8, 8, 'AlexLPF', None, tx, self.ChangeBits)
      bf.index = (index, 3)
      self.NextRow()
      index += 1
    self.FitInside()
    self.SetScrollRate(1, 1)
  def OnChangeFreq(self, event):
    freq = event.GetString()
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    ctrl = event.GetEventObject()
    name = ctrl.quisk_data_name
    filters = self.GetValue(name, radio_dict)
    filters[ctrl.index[0]][ctrl.index[1]] = freq
    setattr(conf, name, filters)
    radio_dict[name] = filters
    local_conf.settings_changed = True
  def ChangeBits(self, control):
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    name = control.quisk_data_name
    filters = self.GetValue(name, radio_dict)
    filters[control.index[0]][control.index[1]] = control.value
    setattr(conf, name, filters)
    radio_dict[name] = filters
    local_conf.settings_changed = True
  def ChangeEnable(self, event):
    btn = event.GetEventObject()
    name = btn.quisk_data_name
    radio_dict = local_conf.GetRadioDict(self.radio_name)
    if event.IsChecked():
      radio_dict[name] = "True"
      setattr(conf, name, True)
    else:
      radio_dict[name] = "False"
      setattr(conf, name, False)
    local_conf.settings_changed = True
    
class BandPlanDlg(wx.Dialog, ControlMixin):
  # BandPlan in the application and the config file is a list of [integer hertz, color] and the color is None for "End".
  # BandPlan in Settings[4] (global settings) is a list of [string freq in MHz, mode].
  # BandPlanColors is always a list of [mode, color] and the mode "End" is not in the list.
  # The color is a string "#aa88cc". The mode is a string "DxData", etc.
  def __init__(self, parent):
    txt = 'Color Bars on the Frequency X-axis Mark the Band Plan'
    wx.Dialog.__init__(self, None, -1, txt, size=(-1, 100))
    ControlMixin.__init__(self, parent)
    self.Bind(wx.EVT_SHOW, self.OnBug)
    bg_color = parent.GetBackgroundColour()
    self.SetBackgroundColour(bg_color)
    self.parent = parent
    self.radio_name = "_Global_"
    self.select_index = 0
    if "BandPlanColors" in Settings[4] and "BandPlan" in Settings[4]:
      colors = Settings[4]["BandPlanColors"]
    else:
      colors = conf.BandPlanColors
    self.modes = []
    self.color_dict = {None:"End"}
    self.mode_dict = {"End":None}
    for mode, color in colors:
      self.modes.append(mode)
      self.color_dict[color] = mode
      self.mode_dict[mode] = color
    self.modes.append("End")
    self.MakeControls()
  def OnBug(self, event):	# Bug 16088
    self.gbs.Fit(self)
    self.Unbind(wx.EVT_SHOW, handler=self.OnBug)
  def MakeControls(self):
    self.num_cols = 7
    #self.MarkCols()
    self.BgColor1 = wx.Colour(0xf8f8f8)
    self.BgColor2 = wx.Colour(0xe0e0e0)
    charx = self.charx
    chary = self.chary
    self.gbs.Add (charx, chary // 2, wx.GBPosition(self.row, 6))	# Spacer at the right ends control expansion
    start_row = self.row
    self.list_ctrl = list_ctrl = wx.ListCtrl(self, size=(charx * 36, application.screen_height * 6 // 10),
                     style=wx.LC_REPORT | wx.LC_SINGLE_SEL | wx.BORDER_SIMPLE | wx.LC_NO_HEADER)
    list_ctrl.InsertColumn(0, '', width=charx*16)
    list_ctrl.InsertColumn(1, '', width=charx*20)
    list_ctrl.SetBackgroundColour(self.BgColor1)
    list_ctrl.Bind(wx.EVT_LIST_ITEM_SELECTED, self.OnSelected)
    hlp = '\
The graph X-axis has colored bars to mark band segments; CW, phone etc. \
Colors start at each frequency and continue until the next frequency or "End". \
To add a row, enter the frequency that starts the segment. Then press the Enter key. \
It is easier to add all the frequencies for a band first and then correct the modes later.'
    txt, edt, btn = self.AddTextEditHelp(3, "New frequency MHz", "", hlp, no_edit=False, border=0)
    self.Bind(wx.EVT_TEXT_ENTER, self.OnNewFreq, source=edt)
    self.NextRow()
    hlp = 'This changes the mode on the selected row. \
The mode begins at the start frequency and ends at the next frequency.'
    txt, self.ComboMode, btn = self.AddTextComboHelp(3, "Mode at start frequency", "AM", self.modes, hlp, no_edit=True, border=0)
    self.ComboMode.handler = self.OnChangeMode
    self.NextRow()
    hlp = 'To delete a row, click the row and press this button.'
    self.AddTextButtonHelp(3, "Delete frequency", "Delete", self.OnDelete, hlp, border=0)
    self.NextRow()
    hlp = 'Press "Save" to save your changes. Press "Cancel" to discard your changes.'
    self.AddText2ButtonHelp(3, "Save changes", "Save", self.OnSave, "Cancel", self.OnCancel, hlp, border=0)
    self.NextRow()
    self.gbs.Add (charx, chary, wx.GBPosition(self.row, 3))	# Spacer before color table
    self.NextRow()
    width = 0
    buttons = []
    colors = []
    for mode in self.modes:
      if mode != "End":
        colors.append(self.mode_dict[mode])
        btn = QuiskPushbutton(self, self.OnColorButton, mode)
        btn.SetColorGray()
        buttons.append(btn)
        width = max(width, btn.GetSize().Width)
    h = self.quisk_height + 2
    for btn in buttons:		# Make all buttons the same size
      btn.SetSizeHints(width, h, width, h)
    h = self.quisk_height
    for i in range(0, len(buttons), 2):		# Must be an even number
      btn1 = buttons[i]
      btn2 = buttons[i + 1]
      color1 = wx.StaticText(self, -1, '')
      color1.SetSizeHints(h, h, -1, h)
      color1.SetBackgroundColour(wx.Colour(colors[i]))
      btn1.color_ctrl = color1
      color2 = wx.StaticText(self, -1, '')
      color2.SetSizeHints(h, h, -1, h)
      color2.SetBackgroundColour(wx.Colour(colors[i + 1]))
      btn2.color_ctrl = color2
      bsizer = wx.BoxSizer(wx.HORIZONTAL)
      bsizer.Add(btn1, proportion=0)
      bsizer.Add(color1, proportion=1, border=3, flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT)
      bsizer.Add(color2, proportion=1, border=3, flag=wx.ALIGN_CENTER_VERTICAL|wx.LEFT|wx.RIGHT)
      bsizer.Add(btn2, proportion=0)
      self.gbs.Add(bsizer, (self.row, 3), span=(1, 3), flag=wx.EXPAND)
      self.NextRow()
    self.NextRow()
    self.gbs.Add(list_ctrl, (start_row, 1), span=(self.row, 1))
    self.gbs.Add (charx, chary // 2, wx.GBPosition(start_row + self.row, 1))	# Spacer at the bottom
    self.DrawPlan()
    self.gbs.Fit(self)
  def DrawPlan(self):
    index = 0
    sel = 0
    if "BandPlanColors" in Settings[4] and "BandPlan" in Settings[4]:
      for freq, mode in Settings[4]["BandPlan"]:
        self.list_ctrl.InsertItem(index, freq)
        self.list_ctrl.SetItem(index, 1, mode)
        if freq == "3.500":
          sel = index
        index += 1
    else:
      for freq, color in conf.BandPlan:
        mode = self.color_dict.get(color, "Other")
        self.list_ctrl.InsertItem(index, FormatMHz(freq * 1E-6))
        self.list_ctrl.SetItem(index, 1, mode)
        if freq == 3500000:
          sel = index
        index += 1
    self.MakeBackground()
    self.list_ctrl.Select(sel)
    self.list_ctrl.EnsureVisible(sel)
  def OnSelected(self, event):
    self.select_index = event.GetIndex()
    mode = self.list_ctrl.GetItem(self.select_index, 1).GetText()
    j = self.modes.index(mode)
    self.ComboMode.SetSelection(j)
  def OnChangeMode(self, ctrl):
    self.list_ctrl.EnsureVisible(self.select_index)
    mode = self.ComboMode.GetValue()
    self.list_ctrl.SetItem(self.select_index, 1, mode)
    self.MakeBackground()
  def MakeBackground(self):
    ctrl = self.list_ctrl
    color = self.BgColor1
    for index in range(ctrl.GetItemCount()):
      mode = self.list_ctrl.GetItem(index, 1).GetText()
      self.list_ctrl.SetItemBackgroundColour(index, color)
      if mode == "End":
        if color is self.BgColor1:
          color = self.BgColor2
        else:
          color = self.BgColor1
  def OnNewFreq(self, event):
    freq = event.GetString()
    win = event.GetEventObject()
    win.Clear()
    try:
      freq = float(freq)
      hertz = int(freq * 1E6 + 0.1)
    except:
      return
    i1, i2 = self.SearchBandPlan(hertz)
    if i1 != i2:	# Frequency is not in the list. Add it at i2.
      mode = self.ComboMode.GetValue()
      self.list_ctrl.InsertItem(i2, FormatMHz(hertz * 1E-6))
      self.list_ctrl.SetItem(i2, 1, mode)
      self.MakeBackground()
    self.list_ctrl.Select(i2)
    self.list_ctrl.EnsureVisible(i2)
  def SearchBandPlan(self, hertz):
    # Binary search for the bracket frequency
    ctrl = self.list_ctrl
    i1 = 0
    i2 = length = ctrl.GetItemCount()
    index = (i1 + i2) // 2
    for i in range(length):
      freq = ctrl.GetItem(index, 0).GetText()
      freq = int(float(freq) * 1E6 + 0.1)
      diff = freq - hertz
      if diff < 0:
        i1 = index
      elif diff > 0:
        i2 = index
      else:		# equal to an item in the list
        return index, index
      if i2 - i1 <= 1:
        break
      index = (i1 + i2) // 2
    return i1, i2
  def OnDelete(self, event):
    self.list_ctrl.DeleteItem(self.select_index)
    self.MakeBackground()
    if self.select_index > 0:
      self.select_index -= 1
    self.list_ctrl.Select(self.select_index)
    self.list_ctrl.EnsureVisible(self.select_index)
  def OnColorButton(self, event):
    btn = event.GetEventObject()
    mode = btn.GetLabel()
    color = wx.Colour(self.mode_dict[mode])
    data = wx.ColourData()
    data.SetColour(color)
    dlg = wx.ColourDialog(self, data)
    dlg.GetColourData().SetChooseFull(True)
    if dlg.ShowModal() == wx.ID_OK:
      color = dlg.GetColourData()
      color = color.GetColour()
      btn.color_ctrl.SetBackgroundColour(color)
      btn.color_ctrl.Refresh()
      color = color.GetAsString(wx.C2S_HTML_SYNTAX)
      self.mode_dict[mode] = color
      self.color_dict[color] = mode
    dlg.Destroy()
  def OnSave(self, event):
    mode_color = []
    for mode in self.modes:
      if mode != "End":
        mode_color.append([mode, self.mode_dict[mode]])
    Settings[4]["BandPlanColors"] = mode_color
    freq_mode = []
    plan = []
    ctrl = self.list_ctrl
    for index in range(ctrl.GetItemCount()):
      freq = ctrl.GetItem(index, 0).GetText()
      mode = ctrl.GetItem(index, 1).GetText()
      freq_mode.append([freq, mode])
      hertz = int(float(freq) * 1E6 + 0.1)
      color = self.mode_dict[mode]
      plan.append([hertz, color])
    Settings[4]["BandPlan"] = freq_mode
    application.BandPlan = plan
    application.BandPlanC2T = {}
    for mode, color in mode_color:
      application.BandPlanC2T[color] = mode
    local_conf.settings_changed = True
    self.EndModal(wx.ID_OK)
  def OnCancel(self, event):
    self.EndModal(wx.ID_CANCEL)
    
class WsjtxDlg(wx.Dialog, ControlMixin):
  def __init__(self, parent):
    txt = 'Configure WSJT-X'
    wx.Dialog.__init__(self, None, -1, txt)
    ControlMixin.__init__(self, parent)
    self.Bind(wx.EVT_SHOW, self.OnBug)
    bg_color = parent.GetBackgroundColour()
    self.SetBackgroundColour(bg_color)
    self.parent = parent
    self.radio_name = "_Global_"
    self.MakeControls()
  def OnBug(self, event):	# Bug 16088
    self.gbs.Fit(self)
    self.Unbind(wx.EVT_SHOW, handler=self.OnBug)
  def MakeControls(self):
    self.num_cols = 10
    #self.MarkCols()
    charx = self.charx
    chary = self.chary
    self.gbs.Add (charx * 3, chary * 1, wx.GBPosition(self.row, 9))	# Spacer at the top right
    self.NextRow()
    # Path to WSJT-X
    path = local_conf.globals.get('path_to_wsjtx', '')
    help_text = "Leave blank for the usual installation path to WSJT-X. If WSJT-X is not in the usual place, enter the path."
    txt, self.edit_path, btn = self.AddTextEditHelp(1, "Path to Wsjt-x", path, help_text, no_edit=False, span2=5, border=0)
    self.Bind(wx.EVT_TEXT_ENTER, self.OnEditPath, source=self.edit_path)
    item = self.AddPushButtonR(8, "Change..", self.OnChangePath, border=0)
    self.NextRow()
    self.NextRow()
    # Configuration name option
    value = local_conf.globals.get('config_wsjtx', '')
    hlp = '\
This is the "--config" option used to specify a configuration when WSJT-X starts. It is normally left blank.'
    txt, edt, btn = self.AddTextEditHelp(1, "Config name option", value, hlp, no_edit=False, border=0)
    edt.SetSizeHints(charx * 20, -1, -1, -1)
    self.Bind(wx.EVT_TEXT_ENTER, self.OnConfigName, source=edt)
    self.gbs.Add (charx * 3, chary // 2, wx.GBPosition(self.row, 4))	# Middle spacer
    # Rig name option
    value = local_conf.globals.get('rig_name_wsjtx', 'quisk')
    hlp = '\
When WSJT-X starts, it uses a rig name to keep different instances separate. \
This is the "--rig-name" option, and it defaults to "quisk".'
    txt, edt, btn = self.AddTextEditHelp(5, "Rig name option", value, hlp, no_edit=False, border=0)
    self.Bind(wx.EVT_TEXT_ENTER, self.OnConfigRigName, source=edt)
    self.NextRow()
    self.NextRow()
    # End of controls
    self.gbs.Add (charx, chary * 2, wx.GBPosition(self.row, 1))	# Spacer at the bottom
    self.gbs.Fit(self)
  def OnConfigName(self, event):
    value = event.GetString()
    value = value.strip()
    local_conf.globals['config_wsjtx'] = value
    local_conf.settings_changed = True
  def OnConfigRigName(self, event):
    value = event.GetString()
    value = value.strip()
    local_conf.globals['rig_name_wsjtx'] = value
    local_conf.settings_changed = True
  def OnChangePath(self, event):
    path = self.edit_path.GetValue()
    path = path.strip()
    if not path:
      if sys.platform == 'win32':
        path = "C:\\WSJT\\wsjtx\\bin\\wsjtx.exe"
      else:
        path = "/usr/bin/wsjtx"
    direc, fname = os.path.split(path)
    dlg = wx.FileDialog(application.main_frame, "Path to WSJT-X", direc, fname, "", wx.FD_OPEN|wx.FD_FILE_MUST_EXIST)
    if dlg.ShowModal() == wx.ID_OK:
      path = dlg.GetPath()
      path = path.strip()
      self.edit_path.SetValue(path)
      local_conf.globals['path_to_wsjtx'] = path
      local_conf.settings_changed = True
    dlg.Destroy()
  def OnEditPath(self, event):
    path = self.edit_path.GetValue()
    path = path.strip()
    local_conf.globals['path_to_wsjtx'] = path
    local_conf.settings_changed = True
