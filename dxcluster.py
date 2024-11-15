# This code was contributed by Christof, DJ4CM.  Many Thanks!!
# Modified November 2024 by N2ADR to remove deprecated telnetlib module.

import threading
import socket
import time
import quisk_conf_defaults as conf

class DxEntry:
  def __init__(self):
    self.info = []
    
  def getFreq(self):
    return self.freq
    
  def getDX(self):
    return self.dx
  
  def getSpotter(self, index):
    return self.info[index][0]
    
  def getTime(self, index):
    return self.info[index][1]
  
  def getLocation(self, index):
    return self.info[index][2]
  
  def getComment(self, index):
    return self.info[index][3]
  
  def getLen(self):
    return len(self.info)
  
  def equal(self, element):
    if element.getDX() == self.dx:
      return True
    else:
      return False
    
  def join (self, element):
    for i in range (0, len(element.info)):
      self.info.insert(0, element.info[i])
    length = len(self.info)
    # limit to max history
    if length > 3:
      del (self.info[length-1])
    self.timestamp = max (self.timestamp, element.timestamp)  
    
  def isExpired(self):
    return time.time()-self.timestamp > conf.dxClExpireTime * 60
    
  def parseMessage(self, message):  
    words = message.split()
    sTime = ''
    locator = ''
    comment = ''
    if len(words) > 3 and words[0].lower() == 'dx' and words[1].lower() == 'de':
      spotter = words[2].strip(':')
      self.freq = int(float(words[3])*1000)
      self.dx = words[4]
      for index in range (5, len(words)):
        word = words[index]
        try:
          if sTime != '':
            locator = word.strip('\07')
          #search time
          if word[0:3].isdigit() and word[4].isalpha():
            sTime = word.strip('\07')
            sTime = sTime[0:2]+':'+sTime[2:4]+ ' UTC'
          if sTime == '':
            if comment != '':
              comment += ' '
            comment += word
        except:
          pass
      self.info.insert(0, (spotter, sTime, locator, comment))
      self.timestamp = time.time()
      #print(self.dx, self.freq, spotter, sTime, locator, comment)
      return True
    return False   
  
class DxCluster(threading.Thread):
  def __init__(self):
    self.error = 'Starting'
    self.dxSpots = []
    threading.Thread.__init__(self)
    self.doQuit = threading.Event()
    self.doQuit.clear()
    self.dxLock = threading.Lock()
    self.addr = conf.dxClHost + ':' + str(conf.dxClPort)
    self.msg_no_spots = "No DX Cluster data from " + self.addr
    self.msg_one_spot = '1 DX spot received from ' + self.addr
    self.msg_spots =    ' DX spots received from ' + self.addr
    
  def run(self):
    self.telnetConnect()
    if self.error:
      self.sock.close()
      return
    while not self.doQuit.isSet():
      try:
        by = self.sock.recv(1024)
      except TimeoutError:
        continue
      except:
        by = b''
      if by:
        with self.dxLock:
          self.bytes += by
      else:
        self.sock.close()
        self.error = "Restarting " + self.addr
        time.sleep(2)
        if not self.doQuit.isSet():
          self.telnetConnect()
          if self.error:
            self.sock.close()
            return
    self.sock.close()
        
  def telnetConnect(self):    
    self.bytes = bytearray(0)
    self.error = 'Starting'
    self.sock = socket.socket()
    self.sock.settimeout(20)
    try:
      self.sock.connect( (conf.dxClHost, conf.dxClPort) )
    except:
      self.error = "Failed to connect to " + self.addr
      return
    self.sock.settimeout(1)
    for i in range(10):
      try:
        self.bytes += self.sock.recv(1024)
      except:
        pass
      if b"login:" in self.bytes:
        break
    else:
      self.error = 'No "login:" prompt from ' + self.addr
      return
    self.sock.sendall(conf.user_call_sign.encode('utf-8', errors='ignore') + b"\r\n")
    if conf.dxClPassword:
      for i in range(10):
        try:
          self.bytes += self.sock.recv(1024)
        except:
          pass
        if b"Password:" in self.bytes:
          break
      else:
        self.error = 'No "Password:" prompt from ' + self.addr
        return
      self.sock.sendall(conf.dxClPassword.encode('utf-8', errors='ignore') + b"\r\n")
    self.error = ''
    self.bytes = bytearray(0)

  def Poll(self):
    with self.dxLock:
      index = self.bytes.find(b"\n")
      if index >= 0:
        message = self.bytes[0:index + 1]
        self.bytes = self.bytes[index + 1:]
      else:
        return
    message = message.decode(encoding='utf-8', errors='replace')
    dxEntry = DxEntry()
    if dxEntry.parseMessage(message):
      for i, listElement in enumerate(self.dxSpots):
        if (listElement.equal(dxEntry)):
          listElement.join (dxEntry)
          return True
        if listElement.isExpired():
          del (self.dxSpots[i])
      self.dxSpots.append(dxEntry)
      return True
        
  def stop(self):
    self.doQuit.set()

  def dxStatus(self):
    if self.error:
      return self.error
    nSpots = len(self.dxSpots)
    if nSpots == 0:
      return self.msg_no_spots
    elif nSpots == 1:
      return self.msg_one_spot
    return str(nSpots) + self.msg_spots
