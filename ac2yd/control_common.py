# THIS IS THE ENTIRE "RADIO" FOR QUISK RUNNING AS A CONTROL HEAD
# No real radio hardware is attached to the control_head computer.
#
# This software is Copyright (C) 2021-2022 by Ben Cahill and 2006-2022 by James C. Ahlstrom.,
# and is licensed for use under the GNU General Public License (GPL).
# See http://www.opensource.org.
# Note that there is NO WARRANTY AT ALL.  USE AT YOUR OWN RISK!!
#
# This file, control_common.py, along with a radio-specific file, e.g.
# control_softrock.py, control_hermes.py, or similar, allows a radio-less (control_head) Quisk,
# running on this computer, to connect to a remote (remote_radio) instance of Quisk
# that runs on a separate computer.
#
# The remote_radio Quisk controls an attached real radio, and uses the hardware file
# remote_common.py, along with a radio-specific file, e.g. remote_softrock.py,
# remote_hermes.py, or similar, to communicate with this control_head Quisk
# computer via a network connection.
#
# The remote_radio computer should be set up with a static IP address, so that you know
# where to point the control_head.  The control head computer may, however, use dynamic
# addressing; the remote radio computer will read the control head address when the
# remote control connection is made.
#
# The main control interface between control_head and remote_radio is via a TCP port;
# this uses very low bandwidth.  All functional control, including CW keying, is done via this port.
#
# There are 2 additional ports, both UDP, using low to moderate bandwidth:
# -- Receive graph/waterfall data from the remote_radio
# -- Receive radio sound from the remote_radio and send mic samples
# These use sequential port numbers based on the TCP port number self.remote_ctl_base_port.
# If you need to change the default base port number, you can edit the line in this file
# that looks like (without the #):
#
#   self.remote_ctl_base_port = 4585		# Remote Control base TCP port
#   self.graph_data_port = self.remote_ctl_base_port + 1		# UDP port for graph data
#   self.remote_radio_sound_port = self.remote_ctl_base_port + 2	# UDP port for radio sound and mic
#
# Make sure to edit the corresponding line in remote_common.py to match ports!!
#
# You should be able to use the Quisk control_head along with any/all means of control that
# you normally use to control Quisk, including serial ports, MIDI, and hamlib/rigctl interfaces.
#
# The remote_radio Quisk/computer is assumed to track the local control_head Quisk/computer;
# no attempt is made to verify the remote_radio Quisk's tuning frequency, mode, etc.
# Snap-to Rx tuning for CW works on the control_head Quisk by virtue of graph/waterfall data
# received from the remote_radio Quisk.
#
# To test CW key timing, set DEBUG_CW_SEND_DITS = 1.  This issues "perfect" bursts of dits,
# configurable in terms of dit length, dit space, number of dits per burst, and pause between
# bursts (phrases).  Search in this file for "DEBUG_CW_SEND_DITS" to find configurable variables.
# To get log output from remote_radio end, in quisk_hardware_remote_radio.py, set DEBUG_CW_JITTER = 1.

from __future__ import print_function
from __future__ import absolute_import

DEBUG_CW_JITTER = 0
DEBUG_CW_SEND_DITS = 0
DEBUG = 0

import socket, time, traceback, hmac, threading, select
import _quisk as QS	# Access Quisk C functions via PyMethodDef QuiskMethods[] in quisk.c
import wx

from quisk_hardware_model import Hardware as BaseHardware

class ControlCommon(BaseHardware):	# This is the Hardware class for the control head
  def __init__(self, app, conf):
    BaseHardware.__init__(self, app, conf)
    self.app = app				# Access Quisk class App (Python) functions
    app.remote_control_head = True
    self.remote_ctl_base_port = 4585		# Base of ports for remote connection (maybe edit this)
    self.remote_ctl_socket = None
    self.remote_ctl_connected = False
    self.remote_ctl_timestamp = None
    self.graph_data_port = self.remote_ctl_base_port + 1
    self.remote_radio_sound_port = self.remote_ctl_base_port + 2
    self.thread_lock = threading.Lock()
    self.remote_radio_ip = socket.gethostbyname(self.conf.remote_radio_ip)	# Allow either host name or IP address
    self.first_heartbeat = True

    self.cw_keydown = 0
    self.cw_phrase_begin_ts = None	# timestamp of beginning of cw phrase
    self.cw_phrase_end_ts = None
    self.cw_phrase_break_duration_secs = 1.0 # cw timestamps will reset to 0
    self.cw_poll_started_ts = None
    self.cw_poll_started = False

    if DEBUG_CW_SEND_DITS:
      self.dit_width = 100		# msec (configurable)
      self.space_width = 100		# msec (configurable)
      self.phrase_gap = 1000		# msec (configurable)
      self.num_dits_in_phrase = 5	# number (configurable)
      self.num_dits_cur_count = 0
      self.key_was_down = False
      self.send_cw_dits = False
      self.cw_test_next_ts = None
      self.cw_test_next_msec = None
      self.cw_phrase_start_ts = None

    self.smeter_text = ''
    self.received = ''
    self.closing = False
    QS.set_sparams(remote_control_head=1, remote_control_slave=0)

  def open(self):
    ret = BaseHardware.open(self)
    self.remote_ctl_timestamp = time.time()
    passw = self.app.local_conf.globals.get("remote_radio_password", "")
    passw = passw.strip()
    if passw:
      del passw
      return "Not yet connected to " + self.conf.remote_radio_ip
    else:
      return "Not yet connected to %s -- Missing Password Here" % self.conf.remote_radio_ip

  def close(self):
    print('Closing Remote Control connection')
    self.closing = True
    t = f'QUIT\n'		# Tell Remote Radio we are quitting
    self.RemoteCtlSend(t)
    self.RemoteCtlClose()
    return BaseHardware.close(self)

  def RemoteCtlClose(self):
    if self.remote_ctl_socket:
      self.remote_ctl_socket.close()
    else:
      print('  Remote Control TCP socket already closed')
    self.remote_ctl_socket = None
    self.remote_ctl_connected = False
    QS.stop_control_head_remote_sound()
    self.app.main_frame.SetConfigText("Disconnected from remote radio " + self.conf.remote_radio_ip)
    self.first_heartbeat = True

  def RemoteCtlConnect(self):
    if self.remote_ctl_connected:
      return True
    if not self.remote_ctl_socket:
      self.remote_ctl_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
      self.remote_ctl_socket.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 184)	# DSCP "Expedite" (46)
      if DEBUG: print("Default timeout for remote_ctl_socket = ", self.remote_ctl_socket.gettimeout())
      self.remote_ctl_socket.settimeout(0.1)	# Allow some time for connection response
      if DEBUG: print("Our set timeout for remote_ctl_socket = ", self.remote_ctl_socket.gettimeout())
    try:
      self.remote_ctl_socket.connect((self.remote_radio_ip, self.remote_ctl_base_port))
    except OSError as err:
      if str(err).startswith('[WinError 10056]'):
        # Connected, in spite of "error" --
        # This can occur in Windows; Windows networking infrastructure continues attempting to connect,
        #   even after the connect() call times out (as set in settimeout(), above).
        #   If Windows connects after the timeout, then, the next time we call connect(),
        #   it returns WinError 10056, "A connect request was made on an already connected socket".
        #   Let's accept this "error" as success!
        if DEBUG: print('connect() returned WinError 10056; Windows already connected! Good!')
        pass
      else:
        # Not yet connected; errors may be expected/normal, or unexpected.
        if str(err).startswith('time'):
          # Timeout "error" is normal when we are waiting for Remote Radio server to become available.
          if DEBUG: print('connect() returned timeout; still waiting for remote radio server')
        elif str(err).startswith('[WinError 10022]'):
          # This can occur in Windows; Windows networking infrastructure continues attempting to connect,
          #   even after the connect() call times out (as set in settimeout(), above).
          #   If, by the next time we call connect(), it has not yet connected, but is still trying to do so
          #   as a continuation of the prior call to connect(), it returns WinError 10022,
          #   "An invalid argument was supplied", which is a little misleading, but okay for us.
          #   We have not yet connected, but this is an "expected behavior".
          if DEBUG: print('connect() returned WinError 10022; invalid argument; still waiting for remote radio server')
        elif str(err).startswith('[Errno 103]'):
          # This can occur in Linux; still attempting to connect
          if DEBUG: print('connect() returned Errno 103; software connection abort; still waiting for remote radio server')
        elif str(err).startswith('[Errno 111]'):
          # This can occur in Linux; still attempting to connect
          if DEBUG: print('connect() returned Errno 111; connection refused; still waiting for remote radio server')
        else:
          # Unexpected error.  Print error info, regardless of DEBUG status.
          print("Remote Control socket.connect() error: {0}".format(err))
        return False      # Failure to connect
    self.remote_ctl_connected = True
    self.remote_ctl_socket.settimeout(0.0)	# Now that we're connected, don't wait if nothing there
    if DEBUG: print("Remote Control connected")
    self.app.main_frame.SetConfigText("Connecting to remote radio " + self.conf.remote_radio_ip)
    # We have a TCP connection, and the remote will send a challenge token. If we give a valid response
    # we will receive "TOKEN_OK" and we can start communication.
    return True         # Success

  def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
    # TODO:  Try to get any modifications of freq or vfo from remote Quisk (?)
    t = f'FREQ;{tune};{vfo};{source};{band};{self.app.rxFreq};{self.VarDecimGetIndex()}\n'
    #print (t)
    self.RemoteCtlSend(t)
    #BMC if DEBUG: print('Change', source, tune, vfo, band)
    return tune, vfo

  def OnSpot(self, level):
    pass

  def SendCwDits(self):
    ts = time.time()
    # Use CW key to start/stop stream of dit phrases
    key_down = QS.is_cwkey_down()
    if not key_down:
      self.key_was_down = False
    else:
      if not self.key_was_down:		# Leading edge detector for key down
        self.key_was_down = True
        if not self.send_cw_dits:
          self.send_cw_dits = True	# Start sending dits
          self.cw_phrase_start_ts = ts		# Beginning of phrase
          self.cw_test_next_ts = ts
          self.cw_test_next_msec = 0		# Start phrase at 0 msec
          self.num_dits_cur_count = 0
        else:
          self.send_cw_dits = False	# Stop sending dits
    if self.send_cw_dits and ts >= self.cw_test_next_ts:
      # Send one on/off pair of CW commands (with msec timestamps since start of CW phrase)
      t = f'CW;1;{self.cw_test_next_msec}\n'
      self.RemoteCtlSend(t)
      self.cw_test_next_msec += self.dit_width
      t = f'CW;0;{self.cw_test_next_msec}\n'
      self.RemoteCtlSend(t)
      self.cw_test_next_msec += self.space_width
      self.cw_test_next_ts = self.cw_phrase_start_ts + (float(self.cw_test_next_msec) / 1000)
      self.num_dits_cur_count += 1
      if self.num_dits_cur_count >= self.num_dits_in_phrase:
        # Set up for next phrase
        self.cw_test_next_ts = ts + (float(self.phrase_gap) / 1000)
        self.cw_phrase_start_ts = self.cw_test_next_ts	# Re-start beginning of phrase
        self.cw_test_next_msec = 0		# Start phrase at 0 msec
        self.num_dits_cur_count = 0

  def ThreadPrinter(self, *args, **kw):
    # Call this to print from (possibly) the sound thread, which must not be slowed down by a print.
    # example:  print (x, y, end=' ') ---->  self.ThreadPrinter(x, y, end=' ')
    with self.thread_lock:	# Call thread_lock only once; twice will deadlock.
      # If print request is from within sound thread, pass the print request to the GUI thread.
      if threading.current_thread().name == "QuiskSound":
        wx.CallAfter(print, *args, **kw)
      else:
        print(*args, **kw)

  def PollCwKey(self):		# Called by the sound thread
    if DEBUG_CW_SEND_DITS:
      self.SendCwDits()
      return
    # Check Quisk key state, send to Remote Radio if change.
    # NOTE:  Timestamps enable Remote Radio to overcome WiFi/network jitter
    ts = time.time()
    if not self.cw_phrase_end_ts:
      self.cw_phrase_end_ts = ts
      self.cw_poll_started_ts = ts

    key_down = QS.is_cwkey_down()
    if not self.cw_poll_started:
      # Detect Quisk startup with CW key down
      if ts - self.cw_poll_started_ts > 0.1:
        # Check for key down only within first 1/10 second of running
        self.cw_poll_started = True
      elif key_down == 1:
        # Quisk startup with key down
        t = f'Quisk is starting with CW key down!  Tx is on, and Rx is blocked until you release CW key.'
        self.ThreadPrinter(t)
        #dlg = wx.MessageDialog(self.app.main_frame, t, "Quisk start, CW key down", style=wx.OK)
        #wx.CallAfter(dlg.ShowModal)
        self.cw_phrase_begin_ts = ts
        self.cw_poll_started = True
    if key_down != self.cw_keydown:
      if key_down == 1 and (ts - self.cw_phrase_end_ts) > self.cw_phrase_break_duration_secs:
        # First CW key-down since a while ago, re-start timestamp sequence for new CW phrase
        self.cw_phrase_begin_ts = ts
      cw_event_ts_msecs = int((ts - self.cw_phrase_begin_ts) * 1000)	# float secs to int msecs
      self.cw_keydown = key_down
      t = f'CW;{key_down};{cw_event_ts_msecs}\n'
      self.RemoteCtlSend(t)
      self.cw_phrase_end_ts = ts # End-of-cw-phrase-detection
      if DEBUG_CW_JITTER: self.ThreadPrinter(f'{ts:10.4f} {key_down}, {cw_event_ts_msecs}')

  def HeartBeat(self):	# Called at about 10 Hz by the main
    if self.closing:	# Don't try to connect if we are closing
      return
    ts = time.time()
    if (ts - self.remote_ctl_timestamp) > 1.0 or self.first_heartbeat:
      self.remote_ctl_timestamp = ts
      if self.remote_ctl_connected:
        # Send keep-alive heartbeat command
        t = f'HEARTBEAT\n'
        self.RemoteCtlSend(t)
      else:
        # Else continually try to connect
        if DEBUG: print('Heartbeat Connect Attempt')
        self.RemoteCtlConnect()
        self.first_heartbeat = False
    self.RemoteCtlRead()

  def RemoteCtlSend(self, text):
    # RemoteCtlSend() may be called from sound thread or GUI thread!
    # self.thread lock (also used in self.ThreadPrinter) protects against thread collisions.
    if not self.remote_ctl_connected:
      if DEBUG: self.ThreadPrinter('Cannot send if not TCP connected:', text)
      return
    if DEBUG: self.ThreadPrinter('Send: ', text, end=' ')
    with self.thread_lock: # Do not call ThreadPrinter() from another thread lock!
      try:
        self.remote_ctl_socket.sendall(text.encode('utf-8', errors='ignore'))
      except OSError as err:
        errtxt = err
        pass
      else:
        return
    self.ThreadPrinter("Closing remote control socket; error in RemoteCtlSend(): {0}".format(errtxt))
    self.RemoteCtlClose()

  def GetSmeter(self):
    return self.smeter_text

  def RemoteCtlRead(self):
    if not self.remote_ctl_connected:
      return
    try:	# Read any data from the socket
      text = self.remote_ctl_socket.recv(1024).decode('utf-8', errors='replace')
    except socket.timeout:	# This does not work
      pass
    except socket.error:	# Nothing to read
      pass
    else:			# We got some characters
      self.received += text
    while '\n' in self.received:	# A complete response ending with newline is available
      reply, self.received = self.received.split('\n', 1)	# Split off the reply, save any further characters
      reply = reply.strip()		# Here is our reply
      if DEBUG: print('Rcvd: ', reply)
      if reply[0] in 'Qq':
        print('Closing Remote Control socket: Q (Quit) from remote radio')
        self.RemoteCtlClose()
        return
      elif reply[0] in 'Mm':
        # S-meter text from remote_radio
        self.smeter_text = reply[2:]
        #print ("Receive smeter", reply[2:])
      elif reply[0:6] == "TOKEN;":
        passw = self.app.local_conf.globals.get("remote_radio_password", "")
        passw = passw.strip()
        if passw:
          passw = passw.encode('utf-8')
          H = hmac.new(passw, reply[6:].encode('utf-8'), 'sha3_256')
          del passw
          self.RemoteCtlSend("TOKEN;%s;%d\n" % (H.hexdigest(), self.app.data_width))
        else:
          print ("Error: Missing password on control head")
      elif reply[0:8] == "TOKEN_OK":
        self.app.main_frame.SetConfigText("Connected to remote radio " + self.conf.remote_radio_ip)
        QS.start_control_head_remote_sound(self.remote_radio_ip, self.remote_radio_sound_port, self.graph_data_port)
        self.CommonInit()	# Send initial parameters common to all radios
        self.RadioInit()	# Send initial parameters peculiar to a given radio
      elif reply[0:9] == "TOKEN_BAD":
        self.app.main_frame.SetConfigText("Error: Remote radio %s: Security challenge failed" % self.conf.remote_radio_ip)
      elif reply[0:13] == "TOKEN_MISSING":
        self.app.main_frame.SetConfigText("Error: Remote radio %s has no password" % self.conf.remote_radio_ip)
      elif reply[0:9] == "HL2_TEMP;":
        setattr(self, "HL2_TEMP", reply[9:])
      elif reply[:3] == 'ERR':
        print('Remote Radio returned ' + reply)
      else:
        print ("Control head received unrecognized command", reply)

  def CommonInit(self):	# Send initial frequencies, band, sample rate, etc. to remote
    app = self.app
    # Frequency and decimation
    self.ChangeFrequency(app.txFreq + app.VFO, app.VFO, "NewDecim")
    # Band
    self.RemoteCtlSend("%s;1\n" % app.lastBand)	
    # Mode
    btn = app.modeButns.GetSelectedButton()
    if btn:
      self.RemoteCtlSend("%s;%d\n" % (btn.idName, btn.GetIndex()))
    # Filter and adjustable bandwidth
    name = "Filter 6Slider"
    value = app.midiControls[name][0].button.slider_value
    self.RemoteCtlSend("%s;%d\n" % (name, value))
    btn = app.filterButns.GetSelectedButton()
    if btn:
      self.RemoteCtlSend("%s;%d\n" % (btn.idName, btn.GetIndex()))
    # AGC and Squelch levels, split offset
    self.RemoteCtlSend("Split;0\n")
    btn = app.BtnAGC
    self.RemoteCtlSend("AGCSQLCH;%d;%d;%d;%d;%d\n" % (btn.slider_value_off, btn.slider_value_on,
           app.levelSquelch, app.levelSquelchSSB, app.split_offset))
    idName = "SqlchSlider"
    value = app.midiControls[idName][0].button.slider_value
    self.RemoteCtlSend("%s;%d\n" % (idName, value))
    # Spot slider
    idName = "SpotSlider"
    value = app.midiControls[idName][0].button.slider_value
    self.RemoteCtlSend("%s;%d\n" % (idName, value))
    # Various buttons
    for idName in ("Mute", "NR2", "AGC", "Sqlch", "NB 1", "Notch", "Test 1", "Spot", "FDX", "PTT", "VOX"):
      self.RemoteCtlSend("%s;%d\n" % (idName, app.idName2Button[idName].GetIndex()))
    # Menus
    for menu in (app.NB_menu, app.split_menu, app.freedv_menu, app.smeter_menu):
      if menu:
        for nid in menu.id2data:
          menu_item = menu.FindItemById(nid)
          kind = menu_item.GetKind()
          if kind == wx.ITEM_RADIO:
            if menu_item.IsChecked():
              self.RemoteCtlSend('MENU;%s;%s;1\n' % (menu.menu_name, menu_item.GetItemLabelText()))
          elif kind == wx.ITEM_CHECK:
            checked = menu_item.IsChecked()
            self.RemoteCtlSend('MENU;%s;%s;%d\n' % (menu.menu_name, menu_item.GetItemLabelText(), int(checked)))
