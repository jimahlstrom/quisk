# -*- coding: utf-8 -*-


from __future__ import print_function
import serial, time, logging
import re
import _quisk as QS
import quisk_conf_defaults as conf
import sys, wx, traceback


# ── ПОСЛЕДОВАТЕЛЬНЫЙ ПОРТ ДЛЯ CAT ───────────────────────────────────
sdr1000_serial_port = "/dev/ttyUSB0"  # Windows: "COM3"  /  Linux: "/dev/ttyUSB0"
sdr1000_serial_rate = 57600          # PowerSDR CAT: до 115200

# ── ЛОГ ────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-5s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("SDR1000")

Mode2Index = {'CWL':0, 'CWU':1, 'LSB':2, 'USB':3, 'AM':4,
      'FM':5, 'EXT':6, 'DGT-U':7, 'DGT-L':8, 'DGT-IQ':9,
      'IMD':10, 'FDV-U':11, 'FDV-L':12, 'DGT-FM':13}

# ── КЛАСС УПРАВЛЕНИЯ ЖЕЛЕЗОМ ────────────────────────────────────────
from quisk_hardware_model import Hardware as BaseHardware

class Hardware(BaseHardware):
    """
    Quisk Hardware-класс для FlexRadio SDR-1000.
    Использует CAT-команды протокола PowerSDR (ZZ + Kenwood).
    """
    # Буфер для приема данных с com-порта
    serialbuf = '';

    # Буфер для отправки команд на com-порт
    cmdbuf = '';

    # Буфер отправки IF
    statusbuf ='';
    statussending = False;

    # Сопоставление размера шагов
    STEP_MAP =  {
        '1':   '0000', '10':  '0001', '50':  '1000', '100':  '0010',
        '250': '1001', '500': '1010', '1k':  '0011', '5k':   '1011',
        '9k':  '1100', '10k': '0100', '100k':'0101', '1M':   '0110',
        '10M': '0111',
    }

    STEP_VAL = {
        '1':    1,
        '10':   10,
        '50':   50,
        '100':  100,
        '250':  250,
        '500':  500,
        '1k':   1000,
        '5k':   5000,
        '9k':   9000,
        '10k':  10000,
        '100k': 100000,
        '1M':   1000000,
        '10M':  10000000,
    }

    # STEP_ENA = ['1', '10', '50', '100', '250', '500', '1k', '5k', '10k', '100k' ]
    STEP_ENA = [ '10', '50', '100', '250', '500', '1k', '5k', '10k', '100k' ]

    BAND_ENA = ['80', '40', '30', '20', '17', '15', '12', '10']

    watchdog = 0
    watchdog_limit = 100

    # Сопоставление режимов Quisk → код MD (PowerSDR/Kenwood)
    # TODO
    
    MODE_MAP = {
        'LSB':  '1',  'USB':  '2',  'CWU':   '3',  'FM':  '4',
        'AM':   '5',  'DGT-L':  '6',  'CWL':  '7',  
        # Quisk-специфичные → стандартные
        'DGT-U':'2', 'FMN':  '4',  'AMN': '5',
        'DGT-FM':  '4',  'FDV-U':'2',  'FDV-L':'1',
        'FSK' : '9', 'FSKR' : '2', 
    }

    def __init__(self, app, conf):
        BaseHardware.__init__(self, app, conf)
        self.ser = None
        self.current_freq = 0
        self.current_vfo = 0
        self.current_mode = 'USB'
        self.current_band = '80'
        self.current_step = '50'
        self.rf_gain_labels = ('RF -20', 'RF -10', 'RF 0', 'RF +10' )
        self.current_pre = 0
        self.current_ritfreq = 0
        self.rit_active = False
        self.ptt_active = False

    # ── Открытие/закрытие ────────────────────────────────────────────

    def open(self):
        """Вызывается один раз при старте Quisk."""

        try:
            self.ser = serial.Serial(
                sdr1000_serial_port,
                sdr1000_serial_rate,
                timeout=0, xonxoff=True,
            )
            log.debug("SDR-1000: COM-порт открыт %s @ %d",
                     sdr1000_serial_port, sdr1000_serial_rate)
        except serial.SerialException as e:
            log.error("SDR-1000: не удалось открыть порт: %s", e)
            self.ser = None
            return "SDR-1000: ошибка порта %s" % sdr1000_serial_port

        msg = "SDR-1000 подключен (%s, %s)" % (
            sdr1000_serial_port, "PowerSDR")
        return msg

    def close(self):
        """Вызывается один раз при завершении Quisk."""
        if self.ptt_active:
            self.ser.cmdbuf += 'RX;'     # снять передачу

        if self.ser and self.ser.is_open:
            self.cmdbuf = ''
            self.ser.flush
            self.ser.close()
            log.debug("SDR-1000: порт закрыт")

    # ── Низкоуровневый CAT-обмен ────────────────────────────────────

    def _cat(self):
        """
        Вызываться будет периодически для чтения порта
        """
        if not self.ser or not self.ser.is_open:
            return ""

        try:
            #self.ser.write(frame)
            if self.ser.in_waiting > 0:
                sdata = self.ser.read(size=self.ser.in_waiting)
                self.serialbuf += sdata.decode('ascii')
            else:
                self.watchdog += 1
            if self.watchdog > self.watchdog_limit:
                log.debug("Watchdog! Resetting arduino")
                self.ser.dtr = 0
                time.sleep(0.1)
                self.ser.dtr = 1
                self.ser.flush
                self.ser.close()
                time.sleep(0.1)
                try:
                    self.ser = serial.Serial(
                    sdr1000_serial_port,
                    sdr1000_serial_rate,
                    timeout=0, xonxoff=True,
                    )
                    log.debug("SDR-1000: COM-порт открыт %s @ %d",
                    sdr1000_serial_port, sdr1000_serial_rate)
                except serial.SerialException as e:
                    log.error("SDR-1000: не удалось открыть порт: %s", e)
                    self.ser = None
                    return "SDR-1000: ошибка порта %s" % sdr1000_serial_port
                self.ser.flushInput()
                self.ser.flushOutput()
                self.watchdog = 0


            ans = ''
            while ';' in self.serialbuf:
                cmds = self.serialbuf.partition(';')

                cmd = cmds[0]
                self.serialbuf = cmds[-1]
                # log.info("Splitted to %s and %s", cmd, self.serialbuf)
                if len(cmd) > 0:
                    answer = ''
                    cmddone = 0
                    match cmd:
                        case 'IF':
                            # Запрос полного статуса
                            # Возврат в statusbuf
                            self.statusbuf = self.ReturnInfo()
                            log.debug("Status refresh, buf: %s", self.serialbuf)
                            cmddone = 1
                        case 'ZZSM0':
                            # Запрос показаний S-метра
                            ans = self.ReturnSmeterA()
                            log.debug("S-meter refresh, buf: %s", ans)
                            cmddone = 1
                        case 'PS1':
                            # Включение
                            ans = 'PS1;'
                            log.debug("PowerON, buf: %s", ans)
                            cmddone = 1
                        case 'ZZPA':
                            # Запрос показаний PRE ! TODO смена режима
                            ans = self.ReturnPre()
                            log.debug("PRE answer, buf: %s", ans)
                            cmddone = 1
                        case 'UP':
                            # Запрос на увеличение частоты
                            self.application.ChangeHwFrequency(9000,
                                self.current_vfo+self.STEP_VAL[self.current_step])
                            cmddone = 1
                        case 'DN':
                            # Запрос на уменьшение частоты
                            self.application.ChangeHwFrequency(9000,
                                self.current_vfo-self.STEP_VAL[self.current_step])
                            cmddone = 1
                        case 'ZZSU':
                            # Запрос на увеличение шага
                            self.StepInc()
                            cmddone = 1
                        case 'ZZSD':
                            # Запрос на уменьшение шага
                            self.StepDec()
                            cmddone = 1
                        case 'BU':
                            # Запрос смены диапазона
                            self.BandUp()
                            cmddone = 1
                        case 'BD':
                            # Запрос смены диапазона
                            self.BandDn()
                            cmddone = 1
                        case 'TX':
                            # Transmit
                            QS.set_PTT(1)
                            self.ptt_active = True
                            log.info("PTT ON")
                            cmddone = 1
                        case 'RX':
                            # Recieve
                            QS.set_PTT(0)
                            self.ptt_active = False
                            log.info("PTT OFF")
                            cmddone = 1
                            
                        case 'ZZQR':
                            # Запрос восстановление QuickSave
                            cmddone = 1

                    if len(cmd) > 4 and not cmddone:
                        log.debug("Try to match cmd: %s, buf: %s", cmd[0:4], self.serialbuf)
                        match cmd[0:4]:
                            case 'ZZPA':
                                # Запрос изменения PRE
                                self.SetPre(cmd)
                                ans = self.ReturnPre()
                                log.debug("PRE set, buf: %s", ans)
                                cmddone = 1
                            case 'ZZMD':
                                # Запрос изменения режима
                                self.SetMode(cmd)
                                log.debug("Mode set, buf: %s", cmd)
                                cmddone = 1
                            case 'FA00':
                                # Запрос изменения частоты
                                self.SetFreq(cmd)
                                log.debug("Freq set, buf: %s", cmd)
                                cmddone = 1
                    if not cmddone:
                        log.info("unknown cmd: %s, buf: %s", cmd, self.serialbuf)
                        ans = '?;'
                    if len(ans) > 0:
                        log.debug("Sending answer, buf: %s", ans)
                        self.ser.write(ans.encode('ascii'))

            if len(self.serialbuf) == 0:
                if len(self.cmdbuf) > 0:
                    cmds = self.cmdbuf.partition(';')
                    cmd = cmds[0]
                    self.cmdbuf = cmds[-1]
                    log.debug("Sending cmd %s, buf: %s", cmd, self.cmdbuf)
                    self.ser.write(cmd.encode('ascii'))
            if (len(self.statusbuf) > 0) and not self.statussending:
                self.statussending = True
                log.debug("Sending status, buf: %s", self.statusbuf)
                self.ser.write(self.statusbuf.encode('ascii'))
                self.statusbuf = ''
                self.statussending = False

        # End of CAT
        except Exception as e:
            log.error("SDR-1000 CAT error: %s", e)
            return ""

    @staticmethod
    def _fmt_freq(hz):
        """Частота → 11 цифр: 14200000 → '00014200000'."""
        return "%011d" % int(hz)

    # ── Quisk API: частота ──────────────────────────────────────────

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        """Quisk вызывает при смене частоты. Возвращает (tune, vfo)."""
        if tune and tune != self.current_freq:
            self.current_freq = tune
            self.current_vfo = tune-9000
            if re.search('FA\\d{11};', self.cmdbuf,0):
                log.debug("--> RE MATCH")
                re.sub('FA\\d{11};', "FA" + self._fmt_freq(tune)+";", self.cmdbuf, 1)
            # self.cmdbuf += ("FA" + self._fmt_freq(tune)+";")
            else:
                self.cmdbuf += ("FA" + self._fmt_freq(tune)+";")
            log.debug("VFO A → %d Hz (%.6f MHz)", self.current_vfo, self.current_freq / 1e6)
        return self.current_freq, self.current_vfo

    def ReturnInfo(self):
        ritstr = self.application.ritFreq
        if ritstr >= 0:
            ritsign = "+"
        else:
            ritsign = ""
        answer = "IF%011d%4s%s%05d%01d0000%01d%1s0000000;" % (
            self.current_freq, self.STEP_MAP[self.current_step],
            ritsign,ritstr,self.application.ritButton.GetValue(),
            self.ptt_active,self.MODE_MAP[self.current_mode])
        # log.info(answer)
        return answer

    def ReturnSmeterA(self):
        i = round((self.application.smeter_db + 67) * 2)
        if i < 0:
            i = 0
        elif i > 260:
            i = 260
        answer = "ZZSM0%03d;" % ( i )
        # log.info(answer)
        return answer

    def ReturnPre(self):
        answer = "ZZPA%01d;" % ( self.current_pre )
        return answer

    def SetPre(self, precmd):
        if len(precmd) == 5:
            match precmd:
                case 'ZZPA0': i = 0
                case 'ZZPA1': i = 1
                case 'ZZPA2': i = 2
                case 'ZZPA3': i = 3
                case _: i = 0
            self.current_pre = i

    def SetFreq(self, freqcmd):
        if len(freqcmd) == 13:
            newfreq = int(freqcmd[2:])
            log.info("Request to change freq: %d", newfreq)
            self.current_vfo=newfreq-9000
            self.application.ChangeHwFrequency(9000, self.current_vfo)
    
    def OnButtonRfGain(self, event):
        btn = event.GetEventObject()
        n = btn.index
        self.current_pre = n
        self.cmdbuf += ("ZZPA%1d;" % self.current_pre)
        
    def SetMode(self, modecmd):
        if len(modecmd) == 6:
            log.info("Request to change mode: %s", modecmd[4:])
            match modecmd[4:]:
                
                # 00 = LSB  =>  LSB
                # 01 = USB  =>  USB
                # 02 = DSB  =>  USB
                # 03 = CWL  =>  CW
                # 04 = CWU  =>  CW
                # 05 = FMN  =>  FMN
                # 06 = AM   =>  AM
                # 07 = DIGU =>  FSKR
                # 08 = SPEC =>  AMN
                # 09 = DIGL =>  FSK
                # 10 = SAM  =>  USB
                # 11 = DRM  =>  USB
                
                case '00': newmode = 'LSB'
                case '01': newmode = 'USB'
                case '02': newmode = 'USB'
                case '03': newmode = 'CWL'
                case '04': newmode = 'CWU'
                case '05': newmode = 'FM'
                case '06': newmode = 'AM'
                case '07': newmode = 'DGT-U'
                case '08': newmode = 'AM'
                case '09': newmode = 'DGT-L'
                case '10': newmode = 'USB'
                case '11': newmode = 'USB'
                case _: newmode = 'USB'
            self.current_mode = newmode
            self.application.mode = self.current_mode
            self.application.MakeFilterButtons(self.application.Mode2Filters(self.current_mode))
            QS.set_rx_mode(Mode2Index.get(self.current_mode, 3))
            if self.current_mode == 'CWL':
                self.application.SetRit(conf.cwTone)
            elif self.current_mode == 'CWU':
                self.application.SetRit(-conf.cwTone)
            else:
                self.application.SetRit(0)
            if self.current_mode in ('CWL', 'CWU'):
                self.application.SetFilterByMode('CW')
            elif self.current_mode in ('LSB', 'USB'):
                self.application.SetFilterByMode('SSB')
            elif self.current_mode == 'AM':
                self.application.SetFilterByMode('AM')
            elif self.current_mode[0:2] == 'FM':
                self.application.SetFilterByMode('FM')
            elif self.current_mode[0:4] == 'DGT-':
                self.application.SetFilterByMode('DGT')
            self.application.sliderSquelch.DeleteSliderWindow()
            
            if self.current_mode == 'FM':
                self.application.sliderSquelch.SetSlider(self.application.levelSquelch)
            else:
                self.application.sliderSquelch.SetSlider(self.application.levelSquelchSSB)
            self.application.OnBtnSquelch()
            if self.current_mode not in ('FDV-L', 'FDV-U'):
                self.application.graph.SetDisplayMsg()
                self.application.waterfall.SetDisplayMsg()
            self.application.SetTxAudio()
            
    def StepInc(self):
        i = self.STEP_ENA.index(self.current_step)
        log.debug("Found index of %s, idx: %d", self.current_step, i)
        if i < (len(self.STEP_ENA)-1):
            self.current_step = self.STEP_ENA[i + 1]
            log.debug("Step changed to %s, idx: %d", self.current_step, i+1)
        else: 
            log.debug("Step not changed, remain %s", self.current_step)


    def StepDec(self):
        i = self.STEP_ENA.index(self.current_step)
        log.info("Found index of %s, idx: %d", self.current_step, i)
        if i > 0:
            self.current_step = self.STEP_ENA[i - 1]
            log.debug("Step changed to %s, idx: %d", self.current_step, i-1)
        else: 
            log.debug("Step not changed, remain %s", self.current_step)

    def BandUp(self):
        i = self.BAND_ENA.index(self.current_band)
        log.debug("Found index of %s, idx: %d", self.current_band, i)
        if i < (len(self.BAND_ENA)-1):
            self.current_band = self.BAND_ENA[i + 1]
        else:
            self.current_band = self.BAND_ENA[0]
        log.info("Band changed to %s, idx: %d", self.current_band, i)
        vfo, tune, mode = self.application.bandState[self.current_band]
        self.application.ChangeHwFrequency(9000, vfo+tune)

    def BandDn(self):
        i = self.BAND_ENA.index(self.current_band)
        log.debug("Found index of %s, idx: %d", self.current_band, i)
        if i > 0:
            self.current_band = self.BAND_ENA[i - 1]
        else:
            self.current_band = self.BAND_ENA[len(self.BAND_ENA)-1]
        log.info("Band changed to %s, idx: %d", self.current_band, i)
        vfo, tune, mode = self.application.bandState[self.current_band]
        self.application.ChangeHwFrequency(9000, vfo+tune)

    def ReturnFrequency(self):
        """Опрос ~10 Гц. Возвращает (tune, vfo) или (None, None)."""
        # SDR-1000 не сообщает частоту сам, поэтому возвращаем None
        return None, None

    # ── Quisk API: режим ─────────────────────────────────────────────

    def ChangeMode(self, mode):
        """Quisk вызывает при смене режима. mode: 'USB', 'CW', и т.д."""
        # Нормализуем DGT-* → USB
        # if mode.startswith('DGT'):
        #     mode = 'USB'
        code = self.MODE_MAP.get(mode)
        if not code:
            log.warning("Неизвестный режим Quisk: %s → по умолчанию USB", mode)
            code = '2'
        self.cmdbuf += ("MD" + code+';')
        self.current_mode = mode
        log.debug("Режим → %s (MD%s)", mode, code)

    # ── Quisk API: диапазон ─────────────────────────────────────────

    def ChangeBand(self, band):
        """Quisk вызывает при смене диапазона. band: '160', '80', …"""
        BaseHardware.ChangeBand(self, band)
        log.info("Диапазон → %s м", band)

    # ── Quisk API: PTT ──────────────────────────────────────────────

    def OnButtonPTT(self, event=None):
        """Quisk вызывает при нажатии/отпускании PTT."""
        if event:
            self.ptt_active = bool(event.GetEventObject().GetValue())
        #self._cat("ZZTX%d" % (1 if self.ptt_active else 0))
        log.debug("PTT %s", "ON" if self.ptt_active else "OFF")

    # ── Quisk API: HeartBeat (~10 Гц) ───────────────────────────────

    def HeartBeat(self):
        """Периодический опрос — можно использовать для синхронизации."""
        self._cat()

    def FastHeartBeat(self):    # Called frequently by the GUI thread
        pass

    # ── Quisk API: TUNE ─────────────────────────────────────────────

    def OnBtnFDX(self, is_fdx):
        """Кнопка FDX (полный дуплекс)."""
        log.debug("FDX → %s", is_fdx)
        
