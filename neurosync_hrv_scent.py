"""
NeuroSync · HRV → 气味控制  概念原型（CONCEPT / PROTOTYPE）
================================================================
这是一个“想法验证”脚本，不是产品，也没有接任何真实传感器。
它用模拟的心跳间期（RR 间期）演示一条控制链路：

    心跳间期序列 → 计算 HRV 指标（RMSSD）→ 判断当前状态 → 选择气味模式 + 出香强度

原方案（2025-12-17《神经嗅觉调节器》）设想的硬件是：
ESP32 + MAX30102（PPG 测心率）+ MPU6050（运动）+ 微流控雾化 + 200 mAh 电池，项圈形态。
这套硬件没有做出来；本脚本只在电脑上跑，用来把控制逻辑讲清楚。

声明：
- 这里的阈值是为了演示随手设定的，没有经过任何验证。
- 本原型不用于、也不声称任何医疗、治疗或缓解症状的用途。

运行：python3 hrv_scent_prototype.py
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass

# ---------------- 可调参数（演示用，未经验证） ----------------
WINDOW_BEATS = 30          # 用最近多少个心跳计算 HRV
RMSSD_LOW_MS = 20.0        # 低于它：视为“紧绷”
RMSSD_HIGH_MS = 45.0       # 高于它：视为“放松”
MOTION_THRESHOLD_G = 0.3   # 运动强度高于它：认为在活动，HRV 不可信，暂停判断

# 气味模式（与 neurosync_limbic_agent.py 里的配方名对应）
SCENT_LIBRARY = {
    "CALM": "雪松 + 薰衣草",
    "COMFORT": "檀香",
    "FOCUS": "迷迭香",
}


@dataclass
class Decision:
    state: str
    scent: str | None
    duty: float            # 出香强度 0.0 ~ 1.0（对应 PWM 占空比）
    rmssd_ms: float | None


def rmssd(rr_ms: list[float]) -> float:
    """RMSSD：相邻心跳间期差值的均方根，常用的短时 HRV 指标。"""
    diffs = [b - a for a, b in zip(rr_ms, rr_ms[1:])]
    return math.sqrt(sum(d * d for d in diffs) / len(diffs))


def decide(rr_ms: list[float], motion_g: float) -> Decision:
    if motion_g > MOTION_THRESHOLD_G:
        return Decision("活动中（暂停判断）", None, 0.0, None)
    if len(rr_ms) < WINDOW_BEATS:
        return Decision("数据不足", None, 0.0, None)

    value = rmssd(rr_ms[-WINDOW_BEATS:])
    if value < RMSSD_LOW_MS:
        return Decision("紧绷", "CALM", 0.6, value)
    if value > RMSSD_HIGH_MS:
        return Decision("放松", None, 0.0, value)
    return Decision("平稳", "COMFORT", 0.3, value)


def duty_to_u16(duty: float) -> int:
    """换算成 MicroPython PWM.duty_u16() 的取值。"""
    return int(max(0.0, min(1.0, duty)) * 65535)


def simulate_rr(mean_ms: float, jitter_ms: float, n: int) -> list[float]:
    return [random.gauss(mean_ms, jitter_ms) for _ in range(n)]


if __name__ == "__main__":
    random.seed(1)
    scenarios = {
        "模拟A：心跳快、变化小": (simulate_rr(650, 8, 40), 0.05),
        "模拟B：心跳中等": (simulate_rr(800, 25, 40), 0.05),
        "模拟C：心跳慢、变化大": (simulate_rr(950, 60, 40), 0.05),
        "模拟D：正在走动": (simulate_rr(700, 30, 40), 0.8),
    }
    for name, (rr, motion) in scenarios.items():
        d = decide(rr, motion)
        hrv = f"{d.rmssd_ms:.1f} ms" if d.rmssd_ms is not None else "—"
        scent = SCENT_LIBRARY.get(d.scent, "不出香") if d.scent else "不出香"
        print(f"{name:<18} RMSSD={hrv:<9} 状态={d.state:<10} → {scent}，"
              f"强度 {d.duty:.0%}（duty_u16={duty_to_u16(d.duty)}）")
