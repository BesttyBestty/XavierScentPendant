# 固件历史版本（v1.0 → v1.5）

主固件是根目录的 `xavier_pendant.ino`（v2.0）。下面是它之前的每一版，代码原样保留，只是收进这一个文件里，避免根目录出现多个 .ino 导致编译冲突。想运行某一版，把代码块复制到一个新的 Arduino 草图里即可。

| 版本 | 变化 | 已知问题 |
|---|---|---|
| v1.0 | 触摸切换开/关 + 30 s 自动关；加热片 + 风扇（GPIO7） | 一直轮询，不休眠 |
| v1.1 | 去掉风扇，只用加热片 | 同上 |
| v1.1b | 加入深度睡眠、冷却时间、电池电压检测（GPIO3） | 用了 `esp_sleep_enable_ext0_wakeup`，ESP32-C3 不支持 EXT0 唤醒 |
| v1.3 | 改用 `esp_deep_sleep_enable_gpio_wakeup`；睡前等手指松开 | 无限等待，TTP223 自锁模式下会卡死 |
| v1.4 | 等松开改为最多等 2 s | 自锁模式下会被立刻重新唤醒，持续循环加热 |
| v1.5 | 两段式：固定加热 → 定时器强制冷却，不再依赖触摸芯片模式 | 注释写 30 s、代码是 15 s；自锁模式下冷却后仍会循环加热；无低电量保护 |

v2.0 在 v1.5 的两段式基础上，补回了 v1.1b 的低电量保护，用“等松开再武装”解决了自锁模式下的循环加热，加热时长统一为 15 s（代码与注释一致）。

---

## v1.0 · 加热片 + 风扇（摘自组装指南）

**已知问题：** 一直轮询，不休眠。

```cpp
/*
 * Xavier · 沈星回智能香薰挂件
 * 主控：ESP32-C3 Super Mini
 * 版本：v1.0
 */

#define TOUCH_PIN 5   // TTP223 I/O → GPIO5
#define HEAT_PIN  6   // 加热片 SI2302 → GPIO6
#define FAN_PIN   7   // 风扇 SI2302 → GPIO7

bool isOn = false;
unsigned long lastTouchTime = 0;
const int DEBOUNCE = 500;
const int AUTO_OFF = 30000; // 30秒自动关闭
unsigned long startTime = 0;

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  pinMode(HEAT_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(HEAT_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);
}

void loop() {
  unsigned long now = millis();

  // 触摸检测
  if (digitalRead(TOUCH_PIN) == HIGH &&
      now - lastTouchTime > DEBOUNCE) {
    lastTouchTime = now;
    isOn = !isOn;

    if (isOn) {
      digitalWrite(HEAT_PIN, HIGH);
      digitalWrite(FAN_PIN, HIGH);
      startTime = now;
    } else {
      digitalWrite(HEAT_PIN, LOW);
      digitalWrite(FAN_PIN, LOW);
    }
  }

  // 30秒自动关闭
  if (isOn && now - startTime > AUTO_OFF) {
    digitalWrite(HEAT_PIN, LOW);
    digitalWrite(FAN_PIN, LOW);
    isOn = false;
  }
}
```

---

## v1.1 · 去掉风扇

**已知问题：** 一直轮询，不休眠。

```cpp
/*
 * Xavier · 沈星回智能香薰挂件
 * 主控：ESP32-C3 Super Mini
 * 版本：v1.1（去除风扇）
 */

#define TOUCH_PIN 5   // TTP223 I/O → GPIO5
#define HEAT_PIN  6   // 加热片 SI2302 → GPIO6

bool isOn = false;
unsigned long lastTouchTime = 0;
const int DEBOUNCE = 500;
const int AUTO_OFF = 30000; // 30秒自动关闭
unsigned long startTime = 0;

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  pinMode(HEAT_PIN, OUTPUT);
  digitalWrite(HEAT_PIN, LOW);
}

void loop() {
  unsigned long now = millis();

  // 触摸检测
  if (digitalRead(TOUCH_PIN) == HIGH &&
      now - lastTouchTime > DEBOUNCE) {
    lastTouchTime = now;
    isOn = !isOn;

    if (isOn) {
      digitalWrite(HEAT_PIN, HIGH);
      startTime = now;
    } else {
      digitalWrite(HEAT_PIN, LOW);
    }
  }

  // 30秒自动关闭
  if (isOn && now - startTime > AUTO_OFF) {
    digitalWrite(HEAT_PIN, LOW);
    isOn = false;
  }
}
```

---

## v1.1b · 深度睡眠 + 冷却 + 低电量检测

**已知问题：** ESP32-C3 不支持 EXT0 唤醒（esp_sleep_enable_ext0_wakeup），这一版在 C3 上无法按预期唤醒。

```cpp
/*
  Xavier · 沈星回智能香薰挂件 - ESP32-C3 主控代码 v1.1
  ------------------------------------------------
  基于实际硬件方案调整:
    - 触摸: TTP223模块(数字IO,非ESP32原生电容触摸)
    - 加热: MCH陶瓷恒温片(5V,自身恒温50°C,MOS管只做通断,不需要PWM功率控制),
      靠加热片自身挥发扩香,不额外配风扇
    - 电源: TP4056 + Qi无线充 + 3.7V 400mAh 锂电池

  相比v1.0的改动:
    1. 去除风扇,纯靠加热片自然挥发扩香,减少一路MOS驱动和一个器件。
    2. 加入深度睡眠(GPIO5外部中断唤醒),400mAh电池只靠30秒轮询撑不了太久,
       不用的时候必须真正断电休眠,而不是一直loop轮询digitalRead。
    3. 加入使用冷却时间,防止连续触摸导致香薰块过度挥发、加热片来不及散热就又通电。
    4. 加入电池电压检测,低电量时跳过加热,提醒该无线充了,同时保护电池。
    5. 沿用你测试过的核心逻辑:触摸切换开/关 + 30秒自动关闭。

  MCH恒温片说明:
    - 恒温50°C是陶瓷片自身PTC特性决定的,不是靠外部电路调节,所以GPIO6直接通断即可,
      不需要PWM,也不需要额外测温——这是这颗器件本身自带的安全冗余,比之前方案更简单可靠。
    - 但仍然建议物理上串一颗PPTC自恢复保险丝在加热支路,做最后一道硬件保护,软件保护始终
      不能100%替代硬件保护。
*/

#include <Arduino.h>
#include <esp_sleep.h>

// ------------------- 引脚定义(对应组装指南接线) -------------------
#define TOUCH_PIN   GPIO_NUM_5   // TTP223 I/O
#define HEAT_PIN    6            // 加热片 MOS Gate
#define BATTERY_ADC_PIN  3       // 电池电压检测(需在TP4056 OUT+后加分压电阻接到这个ADC引脚)

// ------------------- 使用参数 -------------------
const unsigned long DEBOUNCE_MS   = 500;    // 触摸消抖
const unsigned long AUTO_OFF_MS   = 30000;  // 30秒自动关闭(沿用你测试过的值)
const unsigned long COOLDOWN_MS   = 20000;  // 关闭后冷却时间,冷却期内触摸只反馈不重新加热
const float BATTERY_LOW_VOLTAGE   = 3.5;    // 低电量阈值,低于此值跳过加热保护电池

RTC_DATA_ATTR unsigned long lastOffTime = 0; // 深度睡眠后仍保留,用于冷却计时
bool isOn = false;
unsigned long startTime = 0;

// ------------------- 电池检测 -------------------
float readBatteryVoltage() {
  int raw = analogRead(BATTERY_ADC_PIN);
  // 假设分压比1:2,ADC参考3.3V,12bit;需按你实际分压电阻重新标定
  return (raw / 4095.0) * 3.3 * 2.0;
}

// ------------------- 开启香薰 -------------------
void turnOn() {
  unsigned long now = millis();

  if (now - lastOffTime < COOLDOWN_MS && lastOffTime != 0) {
    Serial.println("冷却中,本次触摸不加热");
    return;
  }

  float voltage = readBatteryVoltage();
  if (voltage < BATTERY_LOW_VOLTAGE) {
    Serial.printf("电量过低(%.2fV),跳过加热,请无线充电\n", voltage);
    return;
  }

  Serial.println("开启:加热片");
  digitalWrite(HEAT_PIN, HIGH);
  isOn = true;
  startTime = now;
}

// ------------------- 关闭香薰 -------------------
void turnOff() {
  Serial.println("关闭:加热片");
  digitalWrite(HEAT_PIN, LOW);
  isOn = false;
  lastOffTime = millis();
}

// ------------------- 深度睡眠 -------------------
void goToSleep() {
  Serial.println("进入深度睡眠,等待触摸唤醒...");
  Serial.flush();

  digitalWrite(HEAT_PIN, LOW);

  // TTP223直接模式下触摸时输出HIGH,用GPIO5作为外部唤醒源
  esp_sleep_enable_ext0_wakeup(TOUCH_PIN, 1);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(HEAT_PIN, OUTPUT);
  pinMode(TOUCH_PIN, INPUT);
  digitalWrite(HEAT_PIN, LOW);

  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("触摸唤醒");
    turnOn();
  } else {
    Serial.println("上电启动(非触摸唤醒),默认待机等待触摸");
  }
}

void loop() {
  unsigned long now = millis();

  // 运行中:检测再次触摸(切换关闭)或30秒自动关闭
  if (isOn) {
    if (digitalRead(TOUCH_PIN) == HIGH) {
      delay(DEBOUNCE_MS);
      if (digitalRead(TOUCH_PIN) == HIGH) {
        turnOff();
      }
    }

    if (isOn && now - startTime > AUTO_OFF_MS) {
      turnOff();
    }

    delay(50); // 运行期间保持轮询,不休眠
    return;
  }

  // 已关闭 且 不在30秒运行窗口内 → 回到深度睡眠等待下一次触摸
  goToSleep();
}
```

---

## v1.3 · GPIO 唤醒 + 睡前等松开

**已知问题：** 无限等待手指松开，TTP223 自锁模式下会卡死。

```cpp
/*
 * Xavier · 沈星回智能香薰挂件
 * 主控：ESP32-C3 Super Mini
 * 版本：v1.2（加入深度睡眠，续航优化）
 *
 * 逻辑：
 * - 平时深度睡眠，GPIO5(TTP223)被摸到 → 唤醒
 * - 唤醒后加热30秒（期间再摸一下可提前结束）
 * - 结束后重新进入深度睡眠
 * - C3没有RTC IO唤醒硬件，必须用 esp_deep_sleep_enable_gpio_wakeup
 *   （不能用 EXT0/EXT1）
 *
 * v1.3 修复：进入深睡眠前必须等 GPIO5 真正回到低电平，
 * 否则手指还没松开就睡眠，唤醒条件依然是高电平，
 * 会立刻被重新唤醒，陷入"唤醒-睡眠"死循环，
 * 加热片会一直闪烁式工作，比不加深睡眠还费电。
 */

#include "esp_sleep.h"

#define TOUCH_PIN 5   // TTP223 I/O → GPIO5
#define HEAT_PIN  6   // 加热片 SI2302 → GPIO6

const unsigned long AUTO_OFF  = 30000; // 单次最长工作30秒
const unsigned long DEBOUNCE  = 500;   // 去抖延时

void waitForTouchRelease() {
  // 必须等手指真正离开电极（GPIO5回到低电平）才能睡眠
  // 否则唤醒条件（高电平）仍然满足，会被立刻重新唤醒，
  // 陷入唤醒-睡眠死循环
  unsigned long stableStart = 0;
  const unsigned long STABLE_TIME = 100; // 连续100ms为低电平才算真正松开

  while (true) {
    if (digitalRead(TOUCH_PIN) == LOW) {
      if (stableStart == 0) stableStart = millis();
      if (millis() - stableStart >= STABLE_TIME) break;
    } else {
      stableStart = 0; // 还在摸，重新计时
    }
    delay(10);
  }
}

void goToSleep() {
  digitalWrite(HEAT_PIN, LOW);
  waitForTouchRelease();
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  pinMode(HEAT_PIN, OUTPUT);
  digitalWrite(HEAT_PIN, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    // 被摸醒 → 开始一次加热周期
    delay(DEBOUNCE); // 简单去抖，等手指离开触摸区
    digitalWrite(HEAT_PIN, HIGH);

    unsigned long start = millis();
    while (millis() - start < AUTO_OFF) {
      if (digitalRead(TOUCH_PIN) == HIGH) {
        // 加热期间再摸一次 → 提前结束
        delay(DEBOUNCE);
        break;
      }
      delay(50);
    }

    goToSleep(); // 加热结束（或被提前取消），回到深睡眠
  } else {
    // 首次上电 / 复位 → 直接进入睡眠，等待触摸
    goToSleep();
  }
}

void loop() {
  // 不会执行到这里，深睡眠期间CPU完全挂起
}
```

---

## v1.4 · 限时等待松开

**已知问题：** 自锁模式下超时后立刻被重新唤醒，持续循环加热。

```cpp
/*
 * Xavier · 沈星回智能香薰挂件
 * 主控：ESP32-C3 Super Mini
 * 版本：v1.2（加入深度睡眠，续航优化）
 *
 * 逻辑：
 * - 平时深度睡眠，GPIO5(TTP223)被摸到 → 唤醒
 * - 唤醒后加热30秒（期间再摸一下可提前结束）
 * - 结束后重新进入深度睡眠
 * - C3没有RTC IO唤醒硬件，必须用 esp_deep_sleep_enable_gpio_wakeup
 *   （不能用 EXT0/EXT1）
 *
 * v1.3 修复：进入深睡眠前等 GPIO5 回到低电平，
 * 避免手指未松开就睡眠导致"唤醒-睡眠"死循环。
 *
 * v1.4 修复：v1.3 的等待是无限等待，如果 TTP223 是
 * 自锁模式（摸一下锁高电平，不会自动变低），会导致
 * 程序永久卡死出不来。现在改成"限时等待"（最多等2秒），
 * 超时就直接睡眠，不管是哪种模式都不会卡死：
 * - 直通模式：正常在2秒内检测到低电平，睡眠电流最省
 * - 自锁模式：等不到低电平，超时后直接睡眠，
 *   由于信号仍是高电平，会立刻被重新唤醒再加热一轮，
 *   相当于持续加热，直到用户再摸一次让电极真正变低为止
 *   （这就是自锁模式本身"摸一下开、摸一下关"的行为，
 *   不算bug，是这颗触摸芯片的固有逻辑）
 */

#include "esp_sleep.h"

#define TOUCH_PIN 5   // TTP223 I/O → GPIO5
#define HEAT_PIN  6   // 加热片 SI2302 → GPIO6

const unsigned long AUTO_OFF  = 30000; // 单次最长工作30秒
const unsigned long DEBOUNCE  = 500;   // 去抖延时

void waitForTouchRelease() {
  // 限时等待手指松开（GPIO5回到低电平），最多等2秒
  // 直通模式：正常情况下几十毫秒内就会变低，提前跳出
  // 自锁模式：2秒后还是高电平就直接放弃等待，不再卡死程序
  unsigned long waitStart = millis();
  unsigned long stableStart = 0;
  const unsigned long STABLE_TIME = 100;  // 连续100ms低电平才算真正松开
  const unsigned long MAX_WAIT = 2000;    // 超过2秒直接放弃

  while (millis() - waitStart < MAX_WAIT) {
    if (digitalRead(TOUCH_PIN) == LOW) {
      if (stableStart == 0) stableStart = millis();
      if (millis() - stableStart >= STABLE_TIME) return; // 确认松开，正常退出
    } else {
      stableStart = 0; // 还在摸/锁着，重新计时
    }
    delay(10);
  }
  // 超时：不管有没有松开，都放弃等待，直接去睡眠
  // （如果是自锁模式，会立刻被重新唤醒，相当于持续加热，
  //  这是正常的自锁行为，不是卡死）
}

void goToSleep() {
  digitalWrite(HEAT_PIN, LOW);
  waitForTouchRelease();
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  pinMode(HEAT_PIN, OUTPUT);
  digitalWrite(HEAT_PIN, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    // 被摸醒 → 开始一次加热周期
    delay(DEBOUNCE); // 简单去抖，等手指离开触摸区
    digitalWrite(HEAT_PIN, HIGH);

    unsigned long start = millis();
    while (millis() - start < AUTO_OFF) {
      if (digitalRead(TOUCH_PIN) == HIGH) {
        // 加热期间再摸一次 → 提前结束
        delay(DEBOUNCE);
        break;
      }
      delay(50);
    }

    goToSleep(); // 加热结束（或被提前取消），回到深睡眠
  } else {
    // 首次上电 / 复位 → 直接进入睡眠，等待触摸
    goToSleep();
  }
}

void loop() {
  // 不会执行到这里，深睡眠期间CPU完全挂起
}
```

---

## v1.5 · 加热 → 强制冷却两段式

**已知问题：** 注释写 30 s、代码常量是 15 s；自锁模式下冷却结束后仍会循环加热；没有低电量保护。

```cpp
/*
 * Xavier · 沈星回智能香薰挂件
 * 主控：ESP32-C3 Super Mini
 * 版本：v1.5（加热-冷却两段式，彻底不依赖TTP223工作模式）
 *
 * 核心思路：
 * 之前几版一直在纠结TTP223到底是"直通模式"还是"自锁模式"，
 * 想通过软件去猜测/等待电极状态，结果越改越复杂还容易卡死。
 *
 * 这版彻底放弃猜测触摸芯片的行为，把"要不要停止加热"这件事
 * 完全交给ESP32自己的硬件定时器来决定，跟TTP223是什么脾气
 * 没有任何关系：
 *
 * 阶段1【加热】：被摸醒 → 强制加热30秒（HEAT_TIME），
 *              中途完全不再检查触摸引脚，谁也拦不住，时间到必停
 * 阶段2【冷却】：加热结束后强制冷却20秒（COOLDOWN），
 *              这段时间只用定时器唤醒，不武装触摸唤醒，
 *              就算电极还锁死在高电平也完全不会被提前唤醒
 * 冷却结束后 → 重新武装触摸唤醒，回到待机，等下一次真实触摸
 *
 * 效果：不管TTP223是直通还是自锁模式，摸一下 = 固定烧30秒，
 * 然后必定自己停，中间有20秒的"冷静期"不会被反复触发。
 */

#include "esp_sleep.h"

#define TOUCH_PIN 5   // TTP223 I/O → GPIO5
#define HEAT_PIN  6   // 加热片 SI2302 → GPIO6

const unsigned long HEAT_TIME_MS  = 15000;        // 加热时长：15秒，硬性不受触摸影响
const uint64_t       COOLDOWN_US  = 20000000ULL;  // 冷却时长：20秒，纯定时器驱动

// 用RTC内存记录"接下来该干什么"，深睡眠不会丢失这个值
enum NextAction { ACTION_WAIT_TOUCH, ACTION_COOLDOWN_DONE };
RTC_DATA_ATTR NextAction pendingAction = ACTION_WAIT_TOUCH;

// 待机：只武装触摸唤醒，等真实触摸
void sleepWaitTouch() {
  digitalWrite(HEAT_PIN, LOW);
  pendingAction = ACTION_WAIT_TOUCH;
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// 冷却：只武装定时器唤醒，完全屏蔽触摸引脚
void sleepCooldown() {
  digitalWrite(HEAT_PIN, LOW);
  pendingAction = ACTION_COOLDOWN_DONE;
  esp_sleep_enable_timer_wakeup(COOLDOWN_US);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  pinMode(HEAT_PIN, OUTPUT);
  digitalWrite(HEAT_PIN, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_GPIO && pendingAction == ACTION_WAIT_TOUCH) {
    // 真实触摸唤醒 → 强制加热30秒，中途不再理会触摸引脚
    digitalWrite(HEAT_PIN, HIGH);
    delay(HEAT_TIME_MS);
    digitalWrite(HEAT_PIN, LOW);
    sleepCooldown(); // 进入20秒冷却，不管电极是否还锁着

  } else if (cause == ESP_SLEEP_WAKEUP_TIMER && pendingAction == ACTION_COOLDOWN_DONE) {
    // 冷却时间到 → 重新武装触摸唤醒，回到待机
    sleepWaitTouch();

  } else {
    // 首次上电 / 复位，或者意外情况 → 一律回到待机
    sleepWaitTouch();
  }
}

void loop() {
  // 不会执行到这里，深睡眠期间CPU完全挂起
}
```
