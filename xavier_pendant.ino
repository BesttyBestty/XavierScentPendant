/*
 * Xavier · 沈星回智能气味挂件 —— 主固件 v2.0
 * ============================================================
 * 主控：ESP32-C3 Super Mini（Arduino-ESP32 核心 3.x）
 * 硬件：TTP223 触摸(GPIO5) + SI2302 MOS 管(GPIO6) + MCH 陶瓷加热片
 *       + TP4056 充电 + Qi 无线充接收 + 3.7V 锂电池
 *
 * 一次触摸的完整生命周期（状态机）：
 *
 *   ┌──────────────┐  摸到(GPIO5 高电平唤醒)  ┌──────────┐
 *   │ 待机·深度睡眠 │ ───────────────────────▶ │ 加热阶段 │  固定 HEAT_TIME_MS，
 *   │ (触摸唤醒)    │                          └────┬─────┘  期间不理会触摸
 *   └──────▲───────┘                               │ 时间到 / 电压过低
 *          │ 手指已松开                             ▼
 *   ┌──────┴───────┐  定时器唤醒             ┌──────────┐
 *   │ 等松开·深睡眠 │ ◀── 手指还没松开 ─────── │ 强制冷却 │  定时器深睡眠
 *   │ (低电平唤醒)  │                          │ (只认定时器)│  COOLDOWN_TIME_MS
 *   └──────────────┘                          └──────────┘
 *
 * 设计要点：
 *  1. 每一个阶段之间都是“深度睡眠 + 重启”，不跑 loop()，待机电流最小。
 *     当前状态存在 RTC 内存里，深睡眠不会丢。
 *  2. 加热时长由芯片自己的计时决定，不依赖 TTP223 是直通模式还是自锁模式（v1.5 的思路）。
 *  3. 冷却阶段只开定时器唤醒，触摸完全不生效：加热片有时间散热，香膏不会被连续触发过度挥发。
 *  4. 消抖 / 边沿检测：只有触摸脚“稳定低电平 TOUCH_STABLE_MS”之后才重新武装高电平唤醒；
 *     手指还按着（或自锁模式锁在高电平）时，改为武装“低电平唤醒”等它松开。
 *     这样“摸一下 = 只加热一轮”，不会出现 v1.3 那种唤醒-睡眠死循环，也不会像 v1.5 那样
 *     自锁模式下无限循环加热。
 *  5. 低电量保护：加热前电压低于 BATTERY_LOW_MV 就不加热并闪灯；加热中带载电压
 *     掉到 BATTERY_CUTOFF_MV 以下立即停。
 *  6. 深睡眠期间用 GPIO 保持功能把加热脚锁在低电平，配合栅极下拉电阻，双重保证不误加热。
 *
 * MCH 陶瓷加热片自身有恒温特性，MOS 管只做通断，不需要 PWM。
 * 仍建议在加热支路串一颗 PPTC 自恢复保险丝，软件保护不能完全替代硬件保护。
 *
 * 所有参数都在 config.h。
 */

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "config.h"

// ------------------------------------------------------------
// 日志
// ------------------------------------------------------------
#if FEATURE_SERIAL_LOG
  #define LOG(...)    Serial.printf(__VA_ARGS__)
#else
  #define LOG(...)    do {} while (0)
#endif

// ------------------------------------------------------------
// 状态（存在 RTC 慢速内存里，深度睡眠后仍然保留）
// ------------------------------------------------------------
enum class PendantState : uint8_t {
  IdleArmed,     // 待机：武装了“触摸脚高电平唤醒”，等一次真实触摸
  WaitRelease,   // 等松开：武装了“触摸脚低电平唤醒”，手指松开后再回到待机
  Cooldown,      // 冷却：只武装定时器唤醒，触摸无效
};

RTC_DATA_ATTR static PendantState g_state = PendantState::IdleArmed;
RTC_DATA_ATTR static uint32_t     g_cycleCount = 0;   // 累计加热次数（掉电清零，仅供调试）

// ------------------------------------------------------------
// 输出控制
// ------------------------------------------------------------
static void heaterSet(bool on) {
  digitalWrite(PIN_HEATER, on ? HIGH : LOW);
#if FEATURE_FAN
  digitalWrite(PIN_FAN, on ? HIGH : LOW);
#endif
}

static void ledSet(bool on) {
#if FEATURE_LED
  #if LED_ACTIVE_LOW
    digitalWrite(PIN_LED, on ? LOW : HIGH);
  #else
    digitalWrite(PIN_LED, on ? HIGH : LOW);
  #endif
#else
  (void)on;
#endif
}

static void ledBlink(uint8_t times, uint32_t onMs = 150, uint32_t offMs = 150) {
#if FEATURE_LED
  for (uint8_t i = 0; i < times; ++i) {
    ledSet(true);
    delay(onMs);
    ledSet(false);
    delay(offMs);
  }
#else
  (void)times; (void)onMs; (void)offMs;
#endif
}

// 上电第一件事：解除上次睡眠时的引脚保持，并把所有输出拉到安全状态（关）。
static void initOutputs() {
  gpio_hold_dis((gpio_num_t)PIN_HEATER);
  pinMode(PIN_HEATER, OUTPUT);
  digitalWrite(PIN_HEATER, LOW);

#if FEATURE_FAN
  gpio_hold_dis((gpio_num_t)PIN_FAN);
  pinMode(PIN_FAN, OUTPUT);
  digitalWrite(PIN_FAN, LOW);
#endif

#if FEATURE_LED
  pinMode(PIN_LED, OUTPUT);
  ledSet(false);
#endif

  pinMode(PIN_TOUCH, INPUT);   // TTP223 是推挽输出，不需要上下拉
}

// ------------------------------------------------------------
// 电池电压
// ------------------------------------------------------------
// 返回电池电压（mV）。返回 0 表示“没有启用检测”或“没接分压电阻”，调用方应跳过保护逻辑。
static uint32_t readBatteryMv() {
#if FEATURE_BATTERY_CHECK
  uint32_t sum = 0;
  for (uint8_t i = 0; i < BATTERY_SAMPLES; ++i) {
    sum += analogReadMilliVolts(PIN_BATTERY_ADC);   // 已用芯片出厂校准值换算成 mV
    delay(2);
  }
  const uint32_t pinMv = sum / BATTERY_SAMPLES;
  const uint32_t batMv = (uint32_t)(pinMv * BATTERY_DIVIDER_RATIO);
  if (batMv < BATTERY_ABSENT_MV) {
    return 0;   // 读数不合理：多半是分压电阻没焊，跳过检测
  }
  return batMv;
#else
  return 0;
#endif
}

// ------------------------------------------------------------
// 触摸脚消抖：在 timeoutMs 内，触摸脚是否“连续 TOUCH_STABLE_MS 保持低电平”
// ------------------------------------------------------------
static bool waitTouchReleased(uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint32_t lowSince = 0;
  bool low = false;

  while (millis() - start < timeoutMs) {
    if (digitalRead(PIN_TOUCH) == LOW) {
      if (!low) { low = true; lowSince = millis(); }
      if (millis() - lowSince >= TOUCH_STABLE_MS) return true;
    } else {
      low = false;   // 又变高了，重新计时
    }
    delay(5);
  }
  return false;
}

// ------------------------------------------------------------
// 进入深度睡眠（三种方式）
// ------------------------------------------------------------
static void deepSleepNow() {
  heaterSet(false);
  ledSet(false);

  // 把加热（和风扇）脚锁在低电平，深睡眠期间也保持，防止 MOS 栅极悬空误导通
  gpio_hold_en((gpio_num_t)PIN_HEATER);
#if FEATURE_FAN
  gpio_hold_en((gpio_num_t)PIN_FAN);
#endif
  gpio_deep_sleep_hold_en();

#if FEATURE_SERIAL_LOG
  Serial.flush();
#endif
  esp_deep_sleep_start();
  while (true) {}   // 不会执行到这里
}

// 待机：等一次真实触摸（高电平唤醒）
static void sleepIdleArmed() {
  g_state = PendantState::IdleArmed;
  LOG("[待机] 武装触摸唤醒，进入深度睡眠\n");
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TOUCH, ESP_GPIO_WAKEUP_GPIO_HIGH);
  deepSleepNow();
}

// 等松开：手指还按着，等触摸脚变低再醒（低电平唤醒）
static void sleepWaitRelease() {
  g_state = PendantState::WaitRelease;
  LOG("[等松开] 触摸脚仍为高电平，等它变低再回到待机\n");
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TOUCH, ESP_GPIO_WAKEUP_GPIO_LOW);
  deepSleepNow();
}

// 冷却：只开定时器唤醒，触摸不生效
static void sleepCooldown() {
  g_state = PendantState::Cooldown;
  LOG("[冷却] %lu ms，期间触摸无效\n", (unsigned long)COOLDOWN_TIME_MS);
  esp_sleep_enable_timer_wakeup((uint64_t)COOLDOWN_TIME_MS * 1000ULL);
  deepSleepNow();
}

// 回到待机前的统一入口：先确认手指已松开（消抖），再决定武装哪种唤醒
static void goIdle() {
  if (waitTouchReleased(RELEASE_WAIT_MS)) {
    sleepIdleArmed();
  } else {
    sleepWaitRelease();
  }
}

// ------------------------------------------------------------
// 加热阶段
// ------------------------------------------------------------
static void runHeatPhase() {
  // 1) 加热前检查电量
  const uint32_t mvBefore = readBatteryMv();
  if (mvBefore != 0 && mvBefore < BATTERY_LOW_MV) {
    LOG("[低电量] %lu mV < %lu mV，本次不加热，请放上无线充电座\n",
        (unsigned long)mvBefore, (unsigned long)BATTERY_LOW_MV);
    ledBlink(LOW_BATTERY_BLINKS);
    goIdle();
  }

  // 2) 固定时长加热，期间不理会触摸脚
  ++g_cycleCount;
  LOG("[加热] 第 %lu 次，%lu ms，电池 %lu mV\n",
      (unsigned long)g_cycleCount, (unsigned long)HEAT_TIME_MS, (unsigned long)mvBefore);

  const uint32_t start = millis();
  uint32_t lastPoll = start;
  heaterSet(true);

  while (millis() - start < HEAT_TIME_MS) {
    if (millis() - lastPoll >= BATTERY_POLL_MS) {
      lastPoll = millis();
      const uint32_t mv = readBatteryMv();
      if (mv != 0 && mv < BATTERY_CUTOFF_MV) {
        LOG("[低电量] 加热中电压掉到 %lu mV，提前停止\n", (unsigned long)mv);
        break;
      }
    }
    delay(20);
  }

  heaterSet(false);
  LOG("[加热] 结束，实际 %lu ms\n", (unsigned long)(millis() - start));

  // 3) 无论正常结束还是被低电量打断，都进入强制冷却
  sleepCooldown();
}

// ------------------------------------------------------------
// 每次唤醒都从这里开始：根据“上一个状态 + 唤醒原因”决定下一步
// ------------------------------------------------------------
void setup() {
  initOutputs();   // 最先执行：确保加热片是关的

#if FEATURE_SERIAL_LOG
  Serial.begin(115200);
#endif

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool touchWake = (cause == ESP_SLEEP_WAKEUP_GPIO) &&
                         (esp_sleep_get_gpio_wakeup_status() & (1ULL << PIN_TOUCH));
  const bool timerWake = (cause == ESP_SLEEP_WAKEUP_TIMER);

  switch (g_state) {
    case PendantState::IdleArmed:
      if (touchWake) {
        runHeatPhase();            // 真实触摸 → 加热 → 冷却
      }
      break;

    case PendantState::Cooldown:
      if (timerWake) {
        LOG("[冷却] 结束\n");
        goIdle();                  // 冷却完 → 确认松开 → 待机
      }
      break;

    case PendantState::WaitRelease:
      if (touchWake) {
        goIdle();                  // 触摸脚变低了 → 再消抖确认一次 → 待机
      }
      break;
  }

  // 首次上电、按了复位键、或者出现意外的唤醒原因：一律回到安全的待机
  LOG("[启动] 上电/复位（唤醒原因 %d），进入待机\n", (int)cause);
  goIdle();
}

void loop() {
  // 不会执行到这里：每个阶段结束都会进入深度睡眠，下次唤醒从 setup() 重新开始
}
