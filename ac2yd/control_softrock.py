# This provides access to a remote radio.  See ac2yd/remote_common.py and .pdf files for documentation.

from ac2yd.control_common import ControlCommon

class Hardware(ControlCommon):
  def __init__(self, app, conf):
    ControlCommon.__init__(self, app, conf)
  def RadioInit(self):	# Send initial parameters not covered by CommonInit()
    pass
