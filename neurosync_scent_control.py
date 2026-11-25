"""
NeuroSync · 出香 PWM 控制（MicroPython，ESP32）—— 概念原型
基于原始 scent_control.py，修正了原文件第 12 行的缩进错误（原文件无法运行），并统一了变量名。
引脚 18 是原方案的设定，和本仓库主固件（ESP32-C3 挂件，GPIO6）无关。
"""
from machine import Pin, PWM

SCENT_PIN = 18      # 出香模块驱动脚（原方案）
FREQUENCY = 500     # PWM 频率 Hz
DUTY_8BIT = 128     # 出香强度 0~255，按需调整

pwm = PWM(Pin(SCENT_PIN, Pin.OUT))
pwm.freq(FREQUENCY)
pwm.duty_u16(DUTY_8BIT * 65535 // 255)
