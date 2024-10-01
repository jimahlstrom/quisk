# Please do not change this hardware control module for Quisk.
# You should use it as a base class for your own hardware modules.

# A custom hardware module should subclass this module; start it with:
#    from quisk_hardware_model import Hardware as BaseHardware
#    class Hardware(BaseHardware):
#      def __init__(self, app, conf):
#        BaseHardware.__init__(self, app, conf)
#          ###   your module starts here

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import _quisk as QS

class Hardware:
  def __init__(self, app, conf):
    self.application = app			# Application instance (to provide attributes)
    self.conf = conf				# Config file module
    self.rf_gain_labels = ()			# Do not add the Rf Gain button
    self.correct_smeter = conf.correct_smeter	# Default correction for S-meter
    self.use_sidetone = conf.use_sidetone	# Copy from the config file
    self.transverter_offset = 0			# Calculate the transverter offset in Hertz for each band
    self.hermes_ip = ''				# Should not be necessary
  def pre_open(self):		# Quisk calls this once before open() is called
    pass
  def open(self):		# Quisk calls this once to open the Hardware
    # Return an informative message for the config screen.
    # This method must return a string showing whether the open succeeded or failed.
    t = "Capture from sound card %s." % self.conf.name_of_sound_capt
    return t
  def post_open(self):		# Quisk calls this once after open() and after sound is started
    pass
  def close(self):			# Quisk calls this once to close the Hardware
    pass
  def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
    # Change and return the tuning and VFO frequency in Hertz.  The VFO frequency is the
    # frequency in the center of the display; that is, the RF frequency corresponding to an
    # audio frequency of zero Hertz.  The tuning frequency is the RF frequency indicated by
    # the tuning line on the display, and is equivalent to the transmit frequency.  The quisk
    # receive frequency is the tuning frequency plus the RIT (receive incremental tuning).
    # If your hardware will not change to the requested frequencies, return different
    # frequencies.
    # The source is a string indicating the source of the change:
    #   BtnBand       A band button
    #   BtnUpDown     The band Up/Down buttons
    #   FreqEntry     The user entered a frequency in the box
    #   MouseBtn1     Left mouse button press
    #   MouseBtn3     Right mouse button press
    #   MouseMotion   The user is dragging with the left button
    #   MouseWheel    The mouse wheel up/down
    #   NewDecim      The decimation changed
    # For "BtnBand", the string band is in the band argument.
    # For the mouse events, the handler event is in the event argument.
    return tune, vfo
  def ReturnFrequency(self):
    # Return the current tuning and VFO frequency.  If neither have changed,
    # you can return (None, None).  This is called at about 10 Hz by the main.
    # return (tune, vfo)	# return changed frequencies
    return None, None		# frequencies have not changed
  def ReturnVfoFloat(self):
    # Return the accurate VFO frequency as a floating point number.
    # You can return None to indicate that the integer VFO frequency is valid.
    return None
  def ChangeMode(self, mode):		# Change the tx/rx mode
    # mode is a string: "USB", "AM", etc.
    pass
  def ChangeBand(self, band):
    # band is a string: "60", "40", "WWV", etc.
    try:
      self.transverter_offset = self.conf.bandTransverterOffset[band]
    except:
      self.transverter_offset = 0
  def OnButtonPTT(self, event):
    pass
  def OnBtnFDX(self, is_fdx):   # Status of FDX button, 0 or 1
    pass
  def HeartBeat(self):	# Called at about 10 Hz by the GUI thread
    pass
  def FastHeartBeat(self):	# Called frequently by the GUI thread
    pass
  # The "VarDecim" methods are used to change the hardware decimation rate.
  # If VarDecimGetChoices() returns any False value, no other methods are called.
  def VarDecimGetChoices(self):	# Return a list/tuple of strings for the decimation control.
    return False	# Return a False value for no decimation changes possible.
  def VarDecimGetLabel(self):	# Return a text label for the decimation control.
    return ''
  def VarDecimGetIndex(self):	# Return the index 0, 1, ... of the current decimation.
    return 0		# This is called before open() to initialize the control.
  def VarDecimSet(self, index=None):	# Called when the control is operated.
    # Change the decimation here, and return the sample rate.  The index is 0, 1, 2, ....
    # Called with index == None before open() to set the initial sample rate.
    # Note:  The last used sample rate is available as self.application.vardecim_set if
    #        the persistent state option is True.  If the value is unavailable for
    #        any reason, self.application.vardecim_set is None.
    return 48000
  def VarDecimRange(self):  # Return the lowest and highest sample rate.
    return (48000, 960000)
  #
  # The following methods are used to return I/Q samples from the hardware file to Quisk.
  # None of these methods are called unless you call InitSamples().
  # For an example of their use, see quisk_hardware_sdriq.py.
  # Quisk calls all methods in the hardware file from the GUI thread except for StartSamples(),
  #   GetRxSamples() and StopSamples() which are called from the sound thread.
  # The sound thread starts with StartSamples() and is not running during the calls to pre_open() and open().
  def InitSamples(self, int_size, endian):	# Rx sample initialization; you must call this from your hardware __init__().
    # int_size is the number of bytes in each I or Q sample: 1, 2, 3, or 4
    # endian is the order of bytes in the sample: 0 == little endian; 1 == big endian
    # This can be called again to change the format. For example, a different number of bytes for different sample rates.
    QS.set_params(rx_bytes=int_size, rx_endian=endian)
    self.application.samples_from_python = True
  def InitBscope(self, int_size, endian, clock, length):	# Bandscope initialization; accept raw samples from the ADC
    # You may call this once from your hardware __init__() after calling InitSamples(). The bandscope format can not be changed.
    # int_size is the number of bytes in each sample: 1, 2, 3, or 4
    # endian is the order of bytes in the sample: 0 == little endian; 1 == big endian
    # clock is the integer ADC sample rate in Hertz
    # length is the number of samples in each block of ADC samples, and equals the FFT size.
    QS.set_params(bscope_bytes=int_size, bscope_endian=endian, bscope_size=length)
    self.application.bandscope_clock = clock
  #def PollCwKey(self):  # Optional. Called frequently by the sound thread to check the CW key status.
  #  pass        # Do not define if not needed.
  #def PollGuiControl(self):  # Optional. Called frequently by the GUI thread to change GUI settings (PTT etc.)
  #  pass        # Do not define if not needed.
  def StartSamples(self):	# Quisk calls this from the sound thread to start sending samples.
    # If you return a string, it replaces the string returned from hardware open()
    pass
  def StopSamples(self):	# Quisk calls this from the sound thread to stop sending samples.
    pass
  def GetRxSamples(self):	# Quisk calls this frequently from the sound thread. Poll your hardware for samples.
    # Return any available samples by calling AddRxSamples() and perhaps AddBscopeSamples() from within this method.
    pass
  def AddRxSamples(self, samples):	# Call this from within GetRxSamples() to record the Rx samples.
    # "samples" is int_size of integer I data followed by int_size of integer Q data, repeated.
    # For Python 3, "samples" must be a byte array or bytes; use s = bytearray(2), or s = b"\x55\x44" or similar.
    # For Python 2, "samples" must be a byte array or bytes or a string.
    # The byte length must represent a whole number of samples. No partial records.
    QS.add_rx_samples(samples)
  def AddBscopeSamples(self, samples):	# Call this from within GetRxSamples() to record the bandscope samples.
    # "samples" is the whole block of integer samples from the ADC.
    # For Python 3, "samples" must be a byte array or bytes; use s = bytearray(2), or s = b"\x55\x44" or similar.
    # For Python 2, "samples" must be a byte array or bytes or a string.
    # The number of bytes in "samples" must equal the block length times the bytes per sample.
    QS.add_bscope_samples(samples)
  def GotClip(self):		# Call this to indicate that samples were received with the clip (overrange) indicator true.
    QS.set_params(clip=1)
  def GotReadError(self, print_msg, msg):	# Call this to indicate an error in receiving the Rx samples.
    if print_msg:
      print(msg)
    QS.set_params(read_error=1)
  # If you import wx, there a few useful functions available. To set a busy cursor do this:
  #     try:
  #       wx.BeginBusyCursor()
  #       wx.Yield()
  #       self.ReallyTimeConsumingOperation()
  #     finally:
  #       wx.EndBusyCursor()
  # To update the GUI during a long running operation, you can use wx.Yield() or wx.SafeYield().

