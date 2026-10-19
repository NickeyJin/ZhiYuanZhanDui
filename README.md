# MyFirstCar —— 四轮麦轮全向底盘 + 四轴机械臂（STM32F103C8T6）

致远战队比赛用机器人固件：PS2 无线手柄遥控，四轮麦克纳姆轮全向移动 + 四舵机机械臂拣放方块。

- **底盘**：4× TT 减速电机 + 60mm 麦轮，3 片 DRV8833 驱动 8 路 PWM（20kHz）；
  `vx/vy/ω` 三自由度麦轮逆解，左摇杆只取主导轴走四方向，右摇杆原地自转。
- **机械臂**：TIM4 四路 50Hz 舵机（肩 / 肘 / 腕 / 爪），点动 + 上电初始位 + 一键放块宏。
- **现场调参**：总速度、四轮单轮补偿、前后配平三组旋钮，值存在 RAM `.persist` 段（复位不丢、断电才丢）。
- **反馈**：只有 PC13 灯，靠灯语（按键快闪 / 推杆中闪 / 常亮 / 急停慢闪）。
- 调试不靠串口：板子只引出 SWD，所有状态用 `build\tools\read_vars.ps1` 读 RAM 变量核对。

> 📄 **开发日志（按功能板块、按完成时间排列）在 [`docs/`](docs/) 目录**，
> 从 [`docs/00-项目总览.md`](docs/00-项目总览.md) 进入。开源组件说明见
> [`docs/05-开源组件与参考资料.md`](docs/05-开源组件与参考资料.md)。

---

## 1. 代码结构

```
Core/
├── Inc/  Src/
│   ├── main.c        CubeMX 生成的主程序：初始化 → app_init() → while(1) app_task()
│   ├── app.c/h       应用层：20ms 主循环（手柄 → 保护 → 解算 → 斜坡 → 底盘/机械臂 → 灯）
│   ├── motor.c/h     硬件层：一个轮子 = DRV8833 一路 H 桥 = 2 路 PWM；含起步补偿
│   ├── chassis.c/h   运动层：vx/vy/ω → 四个轮速（麦轮逆解）
│   ├── ps2.c/h       输入层：PS2 手柄软件模拟 SPI，9 字节一帧（位宽 16µs）
│   ├── dwt_delay.c/h 底层时基：DWT 周期计数器做 µs 级延时
│   ├── arm.c/h       机械臂：四路舵机点动、上电初始位、`R2+R1` 放块宏状态机
│   ├── tune.c/h      现场调参：总速度/单轮补偿/前后配平旋钮 + 灯语
│   └── board.h       板级定义：PC13 灯态与非阻塞闪烁
build/tools/
├── direct_build.ps1  直接调 arm-none-eabi-gcc 编译（产物 build\direct\MyFirstCar.elf）
├── flash.ps1         STM32_Programmer_CLI 烧录 + 校验 + 复位
└── read_vars.ps1     经 ST-LINK GDB server 只读地读 RAM 变量
```

依赖方向单向：`app → chassis → motor`、`app → ps2`、`app → arm`、`app → tune`。
约定：**控制回路里绝不用 `HAL_Delay()`**（20ms 节拍会被顶爆，手柄掉帧），只用 `HAL_GetTick()` 做非阻塞计时。

---

## 2. 引脚

| 功能 | 引脚 | 说明 |
|---|---|---|
| 左前 FL / 右前 FR / 左后 RL / 右后 RR | PA0/PA1、PA2/PA3、PB0/PB1、PA8/PA11 | TIM2_CH1/CH2、TIM2_CH3/CH4、TIM3_CH3/CH4、TIM1_CH1/CH4，PSC=71/ARR=49 → 20kHz |
| PS2 DATA / CMD / ATT / CLK | PA4 / PA5 / PA6 / PA7 | DATA 内部上拉 |
| 舵机 肩 / 肘 / 腕 / 爪 | PB6 / PB7 / PB8 / PB9 | TIM4_CH1~CH4，50Hz，500~2500µs |
| 状态灯 | PC13 | 低电平点亮 |
| SWD | PA13 / PA14 | 禁止挪用 |

四条铁律（违反任何一条都表现为"完全没反应"）：

1. 舵机红线**绝不接板载 3V3**，必须走独立 5V（MG995 堵转 1A+ 会把 MCU 拉复位）；
2. 全系统共地（降压板、蓝板、每片 DRV8833）；
3. 每片 DRV8833 的 nSLEEP 各用 10kΩ 上拉到 3V3（悬空 = 芯片睡眠 = 电机不动）；
4. PA13/PA14 是 SWD，别动。

---

## 3. 手柄操作

| 按键 | 功能 |
|---|---|
| 左摇杆（只取主导轴） | 前 / 后 / 左 / 右 平移（四个 90° 扇区，不斜走） |
| 右摇杆左右 | 原地自转 |
| 按住 L1 | 速度减半（精细走位） |
| SELECT + ↑ / ↓ | 前后配平旋钮 |
| SELECT + ← / → | 总速度旋钮 |
| SELECT + L2 / L1 / R2 / R1 + ← / → | 单轮补偿：左前 / 左后 / 右前 / 右后 |
| START | 急停（底盘停 + 机械臂松劲，灯慢闪） |
| SELECT + START | 机械臂四路通电 / 松劲 |
| ↑ / ↓ · ← / → · △ / × · □ / ○ | 肩 · 肘 · 腕 · 爪 点动 |
| 按住 L1 / R1（点动时） | 点动 1/3 速 / 3 倍速 |
| R2 + R1 | 放块宏：三轴走到放块位 → 张爪丢块 → 回到按键前姿态 |

手柄掉线（`ps2_ok == 0`）自动停车；接收器须处于**模拟红灯模式**（ID `0x73`）。

---

## 4. 编译 / 烧录 / 读变量

工具链：GNU Tools for STM32 **14.3.1+st.2**（ST 打包）。

```powershell
# 编译（绕过 CMake try_compile，直接调 gcc；产物 build\direct\MyFirstCar.elf）
powershell -NoProfile -ExecutionPolicy Bypass -File build\tools\direct_build.ps1

# 烧录（ST-LINK + SWD，含校验与复位）
powershell -NoProfile -ExecutionPolicy Bypass -File build\tools\flash.ps1

# 读 RAM 变量（只读；会短暂 halt 芯片，读前记得车/臂已停稳）
powershell -NoProfile -ExecutionPolicy Bypass -File build\tools\read_vars.ps1 `
  -Symbols 't_fw_id,t_persist_magic,t_arm_magic,t_arm_lo,t_arm_hi,t_speed_scale,t_wheel_delta'
```

也可以用 CubeMX + CMake preset 编译（`build/Debug`）。两套产物的 `.persist` 布局不同，
**读变量时必须用与板上固件同版本的 ELF**，否则读到的是错位的垃圾值。

---

## 5. 上电会发生什么

1. 灯快闪 3 下（初始化完成签名）；
2. 底盘不动作、机械臂**保持松劲**（防止上电自动追位把结构扭坏）；
3. 按 `SELECT + START` 后机械臂四路通电，走到**上电初始位**（爪子完全闭合的抓取姿态：肩 1082 / 肘 2500 / 腕 2004 / 爪 570 µs）；
4. 之后即为正常遥控状态：灯 = 按键快闪 / 推杆中闪 / 常亮。

---

## 6. 想改参数去哪

| 想改什么 | 改哪 |
|---|---|
| 底盘速度上限 / 斜坡 / 死区 | `Core/Inc/app.h` 的 `APP_SPEED_MAX` / `APP_RAMP_MS` / `APP_STICK_DEADZONE` 等 |
| 满速占空比、起步补偿 | `Core/Inc/motor.h` 的 `MOTOR_DUTY_PERCENT` / `MOTOR_KICK_MS` / `MOTOR_KICK_GAIN` / `MOTOR_KICK_MIN` |
| 某轮转向反了 | `Core/Src/motor.c` 的 `motor_map[]` 里该轮 `invert` |
| 调参默认值（总速度 / 单轮补偿 / 配平） | `Core/Src/tune.c` 的 `defaults_init()` |
| 舵机软限位 / 点动速度 / 上电初始位 / 放块参数 | `Core/Inc/arm.h` |
| 改软限位后不生效 | `.persist` 里旧 magic 仍有效会沿用旧值 → 把 `arm.c` 的 `ARM_MAGIC` 版本号抬一位 |
| 读变量读出来全是垃圾 | ELF 与板上固件不同版本，或芯片刚复位还没跑初始化（等 3~5s 再读） |

---

## 7. 仓库结构

```
.
├── README.md                本文件
├── docs/                    开发日志（按功能板块、按完成时间排列）
│   ├── 00-项目总览.md       索引 + 约束 + 时间线
│   ├── 01-主控与电源平台.md
│   ├── 02-底盘与全向移动.md
│   ├── 03-遥控与现场调参.md
│   ├── 04-机械臂与夹取.md
│   ├── 05-开源组件与参考资料.md
│   └── 历史归档/            开发过程中的临时清单与旧模块说明（部分内容已过时）
├── Core/Inc/ Core/Src/      固件源码
├── Drivers/                 ST HAL + CMSIS（官方，未修改）
├── cmake/                   CubeMX 生成的构建模板
├── STM32F103xx_FLASH.ld     链接脚本（FLASH 62K + RAM `.persist` 段）
└── build/tools/             编译 / 烧录 / 读变量脚本
```
