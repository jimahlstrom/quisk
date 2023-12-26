# This is the hardware file for the FiFiSDR radio. Thanks to Joe, LA6GRA.

#import sys, struct, time, traceback, math
import struct, traceback
from softrock import hardware_usb as SoftRock

GET_FIFI_EXTRA = 0xAB
SET_FIFI_EXTRA = 0xAC

EXTRA_READ_SVN_VERSION  = 0
EXTRA_READ_FW_VERSION   = 1
EXTRA_WRITE_PREAMP      = 19
EXTRA_READ_PREAMP       = 19

try:
  import usb
  import usb.core, usb.util
except:
  if sys.platform == 'win32':
    dlg = wx.MessageDialog(None, "The Python pyusb module is required but not installed. Do you want me to install it?",
      "Install Python pyusb", style = wx.YES|wx.NO)
    if dlg.ShowModal() == wx.ID_YES:
      import subprocess
      subprocess.call([sys.executable, "-m", "pip", "install", "pyusb"])
      try:
        import usb
        import usb.core, usb.util
      except:
        dlg = wx.MessageDialog(None, "Installation of Python pyusb failed. Please install it by hand.",
           "Installation failed", style=wx.OK)
        dlg.ShowModal()
  else:
    dlg = wx.MessageDialog(None, "The Python pyusb module is required but not installed. Please install package python-usb.",
      "Install Python pyusb", style = wx.OK)
    dlg.ShowModal()

DEBUG = 1

IN =  usb.util.build_request_type(usb.util.CTRL_IN,  usb.util.CTRL_TYPE_VENDOR, usb.util.CTRL_RECIPIENT_DEVICE)
OUT = usb.util.build_request_type(usb.util.CTRL_OUT, usb.util.CTRL_TYPE_VENDOR, usb.util.CTRL_RECIPIENT_DEVICE)

UBYTE2 = struct.Struct('<H')
UBYTE4 = struct.Struct('<L')	# Thanks to Sivan Toledo

class Hardware(SoftRock.Hardware):
  def __init__(self, app, conf):
    SoftRock.Hardware.__init__(self, app, conf)
    self.rf_gain_labels = ("-6 dB", "0 dB")

  def open(self):			# Called once to open the Hardware
    global usb
    # find our device
    usb_dev = usb.core.find(idVendor=self.conf.usb_vendor_id, idProduct=self.conf.usb_product_id)
    if usb_dev is None:
      text = 'USB device not found VendorID 0x%X ProductID 0x%X' % (
          self.conf.usb_vendor_id, self.conf.usb_product_id)
    else:
      #try:		# This exception occurs for the Peabody SDR.  Thanks to ON7VQ for figuring out the problem,
        #usb_dev.set_configuration()        # and to David, AE9RB, for the fix.
      #except:
        #if DEBUG: traceback.print_exc()

      try:
        ret = usb_dev.ctrl_transfer(IN, 0x00, 0x0E00, 0, 2)
      except:
        if DEBUG: traceback.print_exc()
        #text = "No permission to access the SoftRock USB interface"
        text = "No permission to access the FiFi-SDR USB interface"
        usb_dev = None
        try:	# Second try. Thanks to Ben Cahill.
          if DEBUG: print("Second try to open USB with libusb0 backend.")
          import usb.backend.libusb0
          backend = usb.backend.libusb0.get_backend()
          usb_dev = usb.core.find(idVendor=self.conf.usb_vendor_id, idProduct=self.conf.usb_product_id, backend=backend)
          try:
            usb_dev.set_configuration()
          except:
            if DEBUG: traceback.print_exc()
          try:
            ret = usb_dev.ctrl_transfer(IN, 0x00, 0x0E00, 0, 2)
          except:
            if DEBUG: traceback.print_exc()
            usb_dev = None
        except:
          usb_dev = None

    if usb_dev is not None:
        self.usb_dev = usb_dev		# success
        if len(ret) == 2:
          ver = "%d.%d" % (ret[1], ret[0])
        else:
          ver = 'unknown'
        sound = self.conf.name_of_sound_capt
        if DEBUG:
          print("Sound = %s" % (sound))
        if len(sound) > 50:
          sound = sound[0:30] + '|||' + sound[-17:]
        if DEBUG:
          print("Sound = %s" % (sound))
        
        try:
          #res = self.handle.controlMsg(requestType = DEVICE2HOST,
                                      #request = GET_FIFI_EXTRA,
                                      #buffer = 4,
                                      #value=0,
                                      #index=EXTRA_READ_SVN_VERSION,
                                      #timeout=100)
          res = self.usb_dev.ctrl_transfer(IN, GET_FIFI_EXTRA, 0, EXTRA_READ_SVN_VERSION, 4)                
        except:
          if DEBUG:
            traceback.print_exc()
       
        
        svn = (((((res[3]<<8) + res[2])<<8) + res[1])<<8) + res[0]
        if DEBUG:
          print ("FiFi_SVN = %d" % svn)

        try:
          res = self.usb_dev.ctrl_transfer(IN, GET_FIFI_EXTRA, 0, EXTRA_READ_FW_VERSION, 20)                
        except:
          if DEBUG:
            traceback.print_exc()
        fifi_ver = res
        if DEBUG:
          print ("FiFi_ver = %s" % res)
          #fifi_ver_str = ''.join([chr(i) for i in res])   # får ikkje med null terminering
          fifi_ver_str = ''
          for i in fifi_ver:
            if not i:
              break
            fifi_ver_str += chr(i)
          print ("FiFi_ver = %s" % fifi_ver_str)

        #text = 'Capture from SoftRock USB on %s, Firmware %s' % (sound, ver)
        text = "Capture from FiFi-SDR USB (%d, %s), on %s, (SR ver. %s)" % (svn, fifi_ver_str, sound, ver)
        if DEBUG:
          print ('Quisk_title_line = "%s"' % text)

    #self.application.bottom_widgets.info_text.SetLabel(text)
    if DEBUG and usb_dev:
      print ('Startup freq', self.GetStartupFreq())
      print ('Run freq', self.GetFreq())
      print ('Address 0x%X' % usb_dev.ctrl_transfer(IN, 0x41, 0, 0, 1)[0])
      sm = usb_dev.ctrl_transfer(IN, 0x3B, 0, 0, 2)
      sm = UBYTE2.unpack(sm)[0]
      print ('Smooth tune', sm)
    return text

  def OnButtonRfGain(self, event):
    btn = event.GetEventObject()
    value = btn.index
    # value == 0: -6dB
    # value == 1:  0dB
    msg = bytearray()
    msg.append(value)
    self.usb_dev.ctrl_transfer(OUT, SET_FIFI_EXTRA, self.si570_i2c_address + 0x700, EXTRA_WRITE_PREAMP, msg)
