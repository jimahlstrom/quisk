from __future__ import absolute_import
# This code was contributed by Christof, DJ4CM.  Many Thanks!!

import threading
import time
import telnetlib
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
    self.do_init = 1
    threading.Thread.__init__(self)
    self.doQuit = threading.Event()
    self.dxSpots = []
    self.doQuit.clear()
    
  def run(self):
    self.telnetInit()
    self.telnetConnect()
    while not self.doQuit.isSet():
      try:
          self.telnetRead()
      except:
        self.tn.close()
        time.sleep(20)
        if not self.doQuit.isSet():
          self.telnetConnect()
    self.tn.close()
      
  def setListener (self, listener):  
    self.listener = listener
        
  def telnetInit(self):
    self.tn = telnetlib.Telnet()
      
  def telnetConnect(self):    
    for i in range(10):
      try:
        self.tn.open(conf.dxClHost, conf.dxClPort, 10)
        self.tn.read_until(b"login:", 10)
        self.tn.write(conf.user_call_sign.encode('utf-8', errors='ignore') + b"\n")		# user_call_sign may be Unicode
        break
      except:
        time.sleep(0.5)
    if conf.dxClPassword:
      self.tn.read_until(b"Password: ")
      self.tn.write(conf.dxClPassword.encode('utf-8', errors='ignore') + b"\n")

  def telnetRead(self):
    message = self.tn.read_until(b'\n', 60).decode(encoding='utf-8', errors='replace')
    if self.doQuit.isSet() == False:
      dxEntry = DxEntry();
      if dxEntry.parseMessage(message):
        for i, listElement in enumerate(self.dxSpots):
          if (listElement.equal(dxEntry)):
            listElement.join (dxEntry)
            return
          if listElement.isExpired():
            del (self.dxSpots[i])
        self.dxSpots.append(dxEntry)
        if self.listener:
          self.listener()
        
  def getHost(self):
    return self.tn.host + ':' + str(self.tn.port)
        
  def stop(self):
    self.doQuit.set()
