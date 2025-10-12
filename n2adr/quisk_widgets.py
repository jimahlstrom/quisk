# This module is used to add extra widgets to the QUISK screen.

import wx, time
import _quisk as QS

from hermes.quisk_widgets import BottomWidgets as BaseWidgets

class BottomWidgets(BaseWidgets):	# Add extra widgets to the bottom of the screen
  def __init__(self, app, hardware, conf, frame, gbs, vertBox):
    BaseWidgets.__init__(self, app, hardware, conf, frame, gbs, vertBox)
