from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import os,sys
from ctypes import *
os.environ['PATH'] = os.path.dirname(__file__) + ';' + os.environ['PATH']
from sdr_control import *
#import sdr_control

#class self(control)
cnt = Control()
cnt.OpenHW()
cnt.SetHWLO(28050000)
cnt.SetHWSR(740740)
cnt.CloseHW()
