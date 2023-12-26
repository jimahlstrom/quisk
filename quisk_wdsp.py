# This module provides Python access to the WDSP SDR library.

import sys, ctypes, ctypes.util, queue, traceback
import _quisk as QS

class Cwdsp:
  def __init__(self, app):
    self.Lib = None
    self.version = 0
    if ctypes.sizeof(ctypes.c_voidp) != 8 or sys.version_info.major < 3:	# Must be 64-bit Python3
      return
    self.Log = Log = app.std_out_err.Logfile
    Log("Start of wdsp")
    if sys.platform == 'win32':
      try:
        self.Lib = ctypes.WinDLL(".\\libwdsp.dll")
        Log ("Windows: Found private wdsp")
      except:
        name = ctypes.util.find_library("wdsp")
        if name:
          try:
            self.Lib = ctypes.WinDLL(name)
            Log ("Windows: Found public wdsp")
          except:
            pass
    else:
      try:
        self.Lib = ctypes.CDLL("./libwdsp.so")
        Log ("Found private wdsp")
      except:
        name = ctypes.util.find_library("wdsp")
        if name:
          try:
            self.Lib = ctypes.CDLL(name)
            Log ("Found public wdsp")
          except:
            pass
    if not self.Lib:
      Log("Wdsp was not found")
      return
    try:
      func = self.Lib.GetWDSPVersion
    except:
      print("Failed to open WDSP: No version information")
      self.Lib = None
      return
    try:
      self.version = func()
    except:
      print("Failed to open WDSP: Call to version() failed")
      self.Lib = None
      return
    Log ("Wdsp version %d" % self.version)
    self.queue = queue.SimpleQueue()
    try:
      func = self.Lib.fexchange0
      vpt = ctypes.cast(func, ctypes.c_void_p)
      fpt = vpt.value
    except:
      print("Failed to find fexchange0")
      self.Lib = None
      return
    QS.wdsp_set_parameter(0, fexchange0=fpt)
    Log ("Library wdsp is active")
  def open(self, channel):
    Lib = self.Lib
    if not Lib:
      return
    self.Log("Open channel %d" % channel)
    wisdom1 = QS.read_fftw_wisdom()
    try:
      in_size = 256
      dsp_size = 256
      QS.wdsp_set_parameter(channel, in_size=in_size)
      Lib.OpenChannel (channel, in_size, dsp_size, 48000, 48000, 48000, 0, 1,
        ctypes.c_double(0.010), ctypes.c_double(0.025), ctypes.c_double(0.0), ctypes.c_double(0.010), 0)
      Lib.SetRXAShiftRun (channel, 0)
      Lib.RXANBPSetRun (channel, 0)
      Lib.SetRXAAMSQRun (channel, 0)
      Lib.SetRXAMode (channel, 1)	# USB
      Lib.RXASetPassband (channel, ctypes.c_double(300.0), ctypes.c_double(3000.0))
      Lib.RXASetNC (channel, dsp_size)
      Lib.RXASetMP (channel, 0)
      Lib.SetRXAAGCMode(channel, 0)
      Lib.SetRXAAGCFixed(channel, ctypes.c_double(0.0))
      Lib.SetRXAPanelRun(channel, 0)
      Lib.SetRXAEMNRRun(channel, 0)
    except:
      traceback.print_exc()
      self.Lib = None
      self.version = 0
      return
    wisdom2 = QS.read_fftw_wisdom()
    if wisdom1 != wisdom2:
      QS.write_fftw_wisdom()
  def __getattr__(self, name):
    if name.startswith('__') and name.endswith('__'):
      raise AttributeError(name)
    if not self.Lib:
      return None
    try:
      func = self.Lib.__getattr__(name)
    except:
      print ("WDSP: Unknown function", name)
      func = None
    else:
      setattr(self, name, func)
    return func
  def put(self, *args):	# Called by the GUI thread
    if not self.Lib:
      return
    self.queue.put(args)
  def control(self):	# Called by the sound thread
    if not self.Lib:
      return
    if self.queue.empty():
      return
    try:
      item = self.queue.get_nowait()
    except:
      return
    if not item[0]:
      return
    args = []
    for arg in item[1:]:
      if isinstance(arg, int):
        args.append(arg)
      elif isinstance(arg, float):
        args.append(ctypes.c_double(arg))
      elif isinstance(arg, str):
        args.append(ctypes.c_char_p(arg.encode()))
      else:
        print ("WDSP: Unknown type of argument")
        return
    item[0](*tuple(args))
