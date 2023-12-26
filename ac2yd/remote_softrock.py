# This provides access to a remote radio.  See ac2yd/remote_common.py and .pdf files for documentation.

from softrock.hardware_usb import Hardware as Radio
from ac2yd.remote_common import Remot

class Hardware(Remot, Radio):
  def __init__(self, app, conf):
    Radio.__init__(self, app, conf)
    Remot.__init__(self, app, conf)
  def open(self):
    Remot.open(self)
    return "Server: " + Radio.open(self)
  def close(self):
    Remot.close(self)
    Radio.close(self)
  def HeartBeat(self):
    Remot.HeartBeat(self)
    Radio.HeartBeat(self)
    
