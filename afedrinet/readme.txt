Hello All!
I want to publish here some information about possibility to use Quisk application on MAC computers running MAC OSX.
It is for experienced MAC OS users that know what is Terminal and command shell tools and how to compile executable from source code:

1. You should download Quisk package from author's web page
2. Xcode SDK and command tools
3. Install macports to manage ports that required to be used with Quisk
4. You should install the next packages:
      python27, py27-wxpython-2.8, fftw-3.0 , portaudio, pulseaudio  and probably some additional
5. Be ready that packages installation will take long time. Select macports python as default python executable:
>> sudo port select --set python python27
6. After you will extract Quisk files from archive to separate directory you must enter Quisk directory and compile Quisk for your system running next command:
>> make macports
7. After installation to use Quisk with AFEDRi SDR, you have to download additional packages: afedriusb , afedrinet  extract contains of archive to separated folders afedriusb and afedrinet in main Quisk directory.
8. For network connected AFEDRI you must enter afedrinet folder and compile SDR support library running afe_library script, before you can run this script you must modify path to python library and include folders in this script, for example on my MAC it looks like:
###############################################################
gcc -o afedrinet_io.so --shared afedrinet_io.c ../is_key_down.c ../import_quisk_api.c  -I"../" -I"/opt/local/Library/Frameworks/Python.framework/Versions/2.7/include/python2.7/"  -L"/opt/local/Library/Frameworks/Python.framework/Versions/2.7/lib/" -lpython2.7
###################################################################
9. For USB connection you must download the  sdr_commander v1.22
   Extract archive to separate folder and compile source code.
  After successful compilation you will get executable sdr_commander, you  must
   copy it to quick/afedriusb folder
10.
   10. Edit qusik_conf.py file to define correct sound device (card) name, copy this file as .quisk_conf.py to user's home directory.
   For example for portaudio devices it will look like this one:

   #################################################################
      from afedriusb import quisk_hardware
      sample_rate = 185185
      name_of_sound_capt = "portaudio#2"
      name_of_sound_play = "portaudiodefault"
      latency_millisecs = 150
      default_rf_gain = 14
      default_screen = 'WFall'
      playback_rate = 48000
      default 48000
   ##########################################################

