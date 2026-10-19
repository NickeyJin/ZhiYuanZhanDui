# MyFirstCar —— STM32F103C8T6 四轮小车（基础移动版）

当前版本只做一件事：**上电自检 + PS2 无线手柄控制四个轮子正常移动**。
舵机（servo）和麦轮全向解算（mailun）已经删掉，代码结构重新分成清晰的四层。

---

## 1. 代码结构

```
Core/
├── Inc/  Src/
│   ├── main.c      CubeMX 生成的主程序，只负责初始化 → app_init() → while(1) app_task()
│   ├── motor.c/h   硬件层：一个轮子 = DRV8833 一路 H 桥 = 2 路 PWM
│   ├── chassis.c/h 运动层：前进/转向 → 四个轮子的速度（两轴差速模型）
│   ├── ps2.c/h     输入层：PS2 手柄软件模拟时序，读 9 字节一帧
│   ├── app.c/h     应用层：上电自检 + 20ms 主循环（手柄 → 底盘）
│   └── dwt_delay.c/h  DWT 周期计数器做 µs 级延时（PS2 时序靠它）
```

依赖方向是单向的：`app → chassis → motor`，`app → ps2`。没有循环依赖。

---

## 2. 接线（按引脚总表）

### 电机（3 片 DRV8833，共 8 路 PWM）

| 轮位 | 说明 | IN1 | IN2 | 定时器 |
|------|------|-----|-----|--------|
| 1 | 左前 FL | PA0 | PA1 | TIM2_CH1 / CH2 |
| 2 | 右前 FR | PA2 | PA3 | TIM2_CH3 / CH4 |
| 3 | 左后 RL | PB0 | PB1 | TIM3_CH3 / CH4 |
| 4 | 右后 RR | PA8 | PA11 | TIM1_CH1 / CH4 |

- 三个定时器都是 **PSC=0 / ARR=3599 → 20kHz**，高于人耳上限，电机不啸叫。
- DRV8833 **没有独立方向脚**，每个电机必须占 2 路 PWM，所以 4 个电机吃掉 8 路。
- 真值表：`IN1=PWM,IN2=0` 正转；`IN1=0,IN2=PWM` 反转；`0,0` 滑行；`1,1` 刹车。

### PS2 接收器（6 针）

| 接收器 | 板子 | 说明 |
|--------|------|------|
| DATA | PA4 | 输入 + 内部上拉 |
| CMD  | PA5 | 推挽输出 |
| ATT  | PA6 | 推挽输出，低有效 |
| CLK  | PA7 | 推挽输出 |
| VCC  | 3V3 | 用 3V3 最稳 |
| GND  | GND | **必须共地** |

### 状态灯

PC13，**低电平点亮**，只能点灯，不能带负载。

---

## 3. 四条铁律（不遵守就是"完全没反应"）

1. **舵机红线绝不接板载 3V3**，必须外接 5V。（本版本已无舵机代码，将来加回来也一样）
2. **所有 GND 必须共地**：降压板、蓝板、每片 DRV8833。
3. **每片 DRV8833 的 nSLEEP 各接 10kΩ 上拉到 3V3**。悬空 = 芯片睡眠 = 电机一动都不动。
4. **PA13 / PA14 是 SWD 调试口，永远别动**，一碰就再也烧不进去。

两个常见坑：
- 蓝板 5V 排针直连 mini-USB 的 VBUS —— 只插 ST-Link 时那里没电。
- "电机一转 MCU 就复位" —— 细地线压降 + 换向噪声串进 3V3。给电机电源单独走线，MCU 电源加滤波电容。

---

## 4. 上电会发生什么

1. **PC13 灯闪 3 下** —— 提示"我要开始动轮子了，手拿开"。
2. 四个轮子依次点动：**左前 → 右前 → 左后 → 右后**，每个都先按"前进"方向转 0.3s，停一下，再反转 0.3s。
3. 四轮一起**前进 0.5s → 停 → 后退 0.5s → 停**。
4. **灯常亮** = 自检结束，进入手柄控制。

> 第一次上电建议把车架起来（轮子悬空），万一转向不对也不会冲出去。

### 自检怎么看结果

| 现象 | 原因 | 怎么办 |
|------|------|--------|
| 某个轮子**完全不动** | 该片 DRV8833 的 nSLEEP 悬空 / 没共地 / 电机线没插 | 先量 nSLEEP 是不是 3V3 |
| 某个轮子**转向反了** | 电机两根线接反了 | 两种改法：①对调电机的两根线；②把 `Core/Src/motor.c` 里 `motor_map[]` 对应那一行的 `invert` 改成 `1` |
| 四个轮子**全不动** | 电池没电 / 电机电源没接 / 所有 nSLEEP 都悬空 | 量 VM 电压（电池 7.4V，满电 8.4V） |
| 一上电就重启 | 电源被电机拉垮 | 见上面第 2 个坑 |

---

## 5. 手柄操作

| 动作 | 结果 |
|------|------|
| 左摇杆 **上 / 下** | 前进 / 后退 |
| 左摇杆 **左 / 右** 或 右摇杆 **左 / 右** | 左转 / 右转（哪个推得多听哪个） |
| 按住 **START** | 急停（四轮滑行） |
| 接收器掉线 / 跑太远 | 灯灭 + 自动停车（`ps2_ok == 0` 就停，不会拿脏数据乱冲） |

摇杆死区 12（`APP_STICK_DEADZONE`），推到底 = 满速 600（`APP_SPEED_MAX`）。

---

## 6. 想改的东西在哪

| 想改什么 | 改哪 |
|----------|------|
| 不想要上电自检，直接进手柄控制 | `Core/Inc/app.h` 里 `APP_SELFTEST_ENABLE` 改成 `0` |
| 太快 / 太慢 | `Core/Inc/app.h` 里 `APP_SPEED_MAX`（当前 600，上限受 `CHASSIS_SPEED_MAX`=800 限制） |
| 摇杆太灵敏 / 太迟钝 | `Core/Inc/app.h` 里 `APP_STICK_DEADZONE`（当前 12） |
| 某个轮子转向反了 | `Core/Src/motor.c` 里 `motor_map[]` 的 `invert` |
| 自检转速 / 时长 | `Core/Src/app.c` 顶部 `TEST_SPEED` / `TEST_RUN_MS` / `TEST_GAP_MS` / `TEST_WHEEL_GAP` |
| 转弯半径太大（想原地转） | `Core/Src/chassis.c` 里 `chassis_drive()` 的左右分配方式 |

---

## 7. 编译

工程是 CubeMX + CMake + Ninja + arm-none-eabi-gcc（`arm-none-eabi-gcc 14.3.1`）。

**STM32CubeIDE / VS Code（CMake Tools）**：直接打开工程根目录，选 Debug preset 编译即可。

**命令行**（必须给工具链文件加引号，否则 PowerShell 会把 `.cmake` 当独立参数拆开）：

```powershell
$env:PATH = "<STM32CubeIDE>\plugins\...gnu-tools-for-stm32...\tools\bin;$env:PATH"
cmake -S . -B build/Debug -G Ninja `
      '-DCMAKE_BUILD_TYPE=Debug' `
      '-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake' `
      "-DCMAKE_MAKE_PROGRAM=<ninja.exe 绝对路径>"
cmake --build build/Debug
```

产物：`build/Debug/MyFirstCar.elf`（用 ST-Link 烧这个）。

当前实测占用量：**FLASH 12064 B / 64 KB（18.4%）**，**RAM 1824 B / 20 KB（8.9%）**。

---

## 8. 设计与制作文档（中期考核）

- **[docs/智能车设计与制作日志.md](docs/智能车设计与制作日志.md)**：按机器人功能划分的 8 个板块（功能描述 + 按时间排序的开发历程），含已实现功能一览、物资清单与成本、开源项目与第三方库说明；配图在 `docs/images/`（24 张）。

队伍：致远战队（实训战队：千里战队）｜队长：傅国钊｜2026 年中期考核
