#!/usr/bin/python

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

##################################################
# AFEDRI Class module
# Title: afedri.py
# Author: k3it

# Adopted to work with Quisk
# by 4Z5LV
# Last Changes: Sat Feb  02 2013
# Version: 2.2

# Adapted to work with Python3
# by N2ADR
# Last Changes: January 2020
##################################################

from socket import *
import wave
import sys
import struct
import time
import datetime
import string
import math


class afedri(object):
        """
        class definition for the Afedri SDR-NET
        """
        def __init__(self,sdr_address="0.0.0.0", sdr_port=50000):
                if sdr_address == "0.0.0.0":
                        __sdr_address,self.sdr_port = self.__discover_afedri()
                        if __sdr_address is None:
                                self.s = None
                                return
                else:
                        __sdr_address = sdr_address
                        self.sdr_port = sdr_port
                        
                self.s = socket(AF_INET, SOCK_STREAM)
                self.s.settimeout(2)
                try:
                        self.s.connect((__sdr_address,self.sdr_port))
                        #print ("Established control connection with AFEDRI")
                except:
                        print ("Error connecting to SDR")
                        ##sys.exit()
                        self.s.close()
                        self.s = None
        def close(self):
                if self.s:
                        self.s.close()
                        self.s = None
        def set_center_freq(self,target_freq):
                if not self.s: return 1
                __next_freq = target_freq
                __next_freq = struct.pack("<q",__next_freq)
                __set_freq_cmd = b"\x0a\x00\x20\x00\x00" + __next_freq[:5]
                self.s.send(__set_freq_cmd)
                __data = self.s.recv(10)
                __freq = __data[5:] + b"\x00\x00\x00"
                __freq = struct.unpack("<q",__freq)[0]
                return  __freq


        def set_samp_rate(self,target_samprate):
                if not self.s: return 1
                #__samp_rate = target_samprate
                __samp_rate = struct.pack("<L",target_samprate)
                __set_rate_cmd = b"\x09\x00\xB8\x00\x00" + __samp_rate[:4]
                self.s.send(__set_rate_cmd)
                __data = self.s.recv(9)
                __samp = __data[5:] + b"\x00\x00\x00\x00"
                __samp = struct.unpack("<q",__samp)[0]
                return  __samp


        def set_gain(self,target_gain):
                if not self.s: return 1
                __gain = target_gain
                # special afedri calculation for the gain byte
                __gain = ((__gain+10)/3 << 3) + 1
                __set_gain_cmd = b"\x06\x00\x38\x00\x00" + struct.pack("B",__gain)
                self.s.send(__set_gain_cmd)
                __data = self.s.recv(6)
                __rf_gain = -10 + 3 * (struct.unpack("B",__data[5:6])[0]>>3)
                return __rf_gain

        def set_gain_indx(self,indx):
                if not self.s: return 1
                __gain = (indx  << 3) + 1
                # special afedri calculation for the gain byte
                #__gain = ((__gain+10)/3 << 3) + 1
                __set_gain_cmd = b"\x06\x00\x38\x00\x00" + struct.pack("B",__gain)
                self.s.send(__set_gain_cmd)
                __data = self.s.recv(6)
                __rf_gain = -10 + 3 * (struct.unpack("B",__data[5:6])[0]>>3)
                return __rf_gain

        def get_gain(self):
                """
                NOT IMPLEMENTED IN AFEDRI?. DON'T USE
                """
                if not self.s: return 1
                __get_gain_cmd = b"\x05\x20\x38\x00\x00"
                self.s.send(__get_gain_cmd)
                __data = self.s.recv(6)
                __rf_gain = -10 + 3 * (struct.unpack("B",__data[5:])[0]>>3)
                return __rf_gain

        def get_fe_clock(self):
                if not self.s: return 1
                __get_lword_cmd = b"\x09\xE0\x02\x55\x00\x00\x00\x00\x00"
                __get_hword_cmd = b"\x09\xE0\x02\x55\x01\x00\x00\x00\x00"
                self.s.send(__get_lword_cmd)
                __data_l = self.s.recv(9)
                self.s.send(__get_hword_cmd)
                __data_h = self.s.recv(9)
                __fe_clock = struct.unpack("<H",__data_l[4:6])[0] + (struct.unpack("<H",__data_h[4:6])[0]<<16)
                return __fe_clock

        def start_capture(self):
                #start 16-bit contiguous capture, complex numbers
                if not self.s: return 1
                __start_cmd=b"\x08\x00\x18\x00\x80\x02\x00\x00"
                self.s.send(__start_cmd)
                __data = self.s.recv(8)
                return __data

        def get_sdr_name(self):
                #Request SDR's Name string 	command = array.array('B',[0x4, 0x20,1,0])
                if not self.s: return 1
                __start_cmd=b"\x04\x20\x01\x00"
                self.s.send(__start_cmd)
                __data = self.s.recv(16)
                __data = __data.decode('utf-8')
                return __data

        def stop_capture(self):
                if not self.s: return 1
                __stop_cmd=b"\x08\x00\x18\x00\x00\x01\x00\x00"
                self.s.send(__stop_cmd)
                __data = self.s.recv(8)
                return __data

        def __discover_afedri(self):
                # attempt to find AFEDRI SDR on the network
                # using AE4JY Simple Network Discovery Protocol
                
                __DISCOVER_SERVER_PORT=48321      # PC client Tx port, SDR Server Rx Port 
                __DISCOVER_CLIENT_PORT=48322      # PC client Rx port, SDR Server Tx Port 

                __data=b"\x38\x00\x5a\xa5"         # magic discovery packet
                __data=__data.ljust(56,b"\x00")    # pad with zeroes
                
                self.s = socket(AF_INET, SOCK_DGRAM)
                self.s.bind(('', 0))
                self.s.setsockopt(SOL_SOCKET, SO_BROADCAST, 1)
                
                self.sin = socket(AF_INET, SOCK_DGRAM)
                self.sin.bind(('', __DISCOVER_CLIENT_PORT))

                #exception if no response to broadcast
                self.sin.settimeout(1)


                self.s.sendto(__data, ('<broadcast>',__DISCOVER_SERVER_PORT))
                try:
                        __msg=self.sin.recv(256,0)
                        __devname=__msg[5:20]
                        __devname=__devname.decode('utf-8')
                        __sn=__msg[21:36]
                        __sn=__sn.decode('utf-8')
                        __ip=inet_ntoa(__msg[40:36:-1])
                        __port=struct.unpack("<H",__msg[53:55])[0]
                        self.s.close()
                        self.sin.close()
                        print ("found ", __devname, __sn, __ip, __port)
                        return (__ip,__port)
                except timeout:
                        print ("No response from AFEDRI on the LAN")
                        ##sys.exit()
                        return None, None
               

        def __del__(self):
                self.stop_capture()
                if self.s: self.s.close()

"""
# verify and correct sampling rate according to the main clock speed
# Alex Trushkin code 4z5lv:
fe_main_clock_freq = a.get_fe_clock()
tmp_div = fe_main_clock_freq / (4 * samp_rate)
floor_div = math.floor(tmp_div)
if (tmp_div - floor_div >= 0.5):
        floor_div += 1
if floor_div < 15:
        floor_div = 15
        #print ("Warning: Max supported sampling rate is", math.floor(fe_main_clock_freq / (4 * floor_div)))
elif floor_div > 625:
        floor_div = 625
        #print ("Warning: Min supported sampling rate is", math.floor(fe_main_clock_freq / (4 * floor_div)))
                                                                    
dSR =  fe_main_clock_freq / (4 * floor_div)
floor_SR = math.floor(dSR)
if (dSR - floor_SR >= 0.5):
        floor_SR += 1
if floor_SR != samp_rate:
        print ("Warning: invalid sample rate selected for the AFEDRI main clock (", fe_main_clock_freq, "Hz )")
        print ("         setting to the next valid value", samp_rate, " => ", floor_SR)
        samp_rate = floor_SR
"""        
