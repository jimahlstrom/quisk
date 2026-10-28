# This provides access to a remote radio.  See ac2yd/remote_common.py and .pdf files for documentation.

from hermes.quisk_hardware import Hardware as Radio
from ac2yd.remote_common import Remot

class Hardware(Remot, Radio):
  def __init__(self, app, conf):
    Radio.__init__(self, app, conf)
    Remot.__init__(self, app, conf)
    self.send_IO_Board = True
  def open(self):
    Remot.open(self)
    return "Server: " + Radio.open(self)
  def close(self):
    Remot.close(self)
    Radio.close(self)
  def HeartBeat(self):
    Remot.HeartBeat(self)
    Radio.HeartBeat(self)
    if self.app.remote_control_slave:
      widg = self.app.bottom_widgets
      t = "HL2_TEMP;%s;%s;%s;%s\n" % (
          widg.text_temperature.GetLabel(), widg.text_pa_current.GetLabel(),
          widg.text_fwd_power.GetLabel(), widg.text_swr.GetLabel())
      self.RemoteCtlSend(t)
      if self.send_IO_Board and self.have_IO_Board is not None:
        self.send_IO_Board = False
        if self.have_IO_Board:
          self.RemoteCtlSend("HL2_IOBoard;1\n")
        else:
          self.RemoteCtlSend("HL2_IOBoard;0\n")
    
