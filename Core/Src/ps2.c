/*
ps2.c —— PS2 无线手柄驱动（软件模拟 PS2 时序）
    ps2_init(): 初始化 GPIO + 内部上拉 + 状态灯
    ps2_read_frame(): 拉低 ATT，整帧 9 字节收发完再放开
    ps2_xfer(): 收发 1 个字节，返回同时读回来的 1 个字节
    ps2_axes_to_center(): 摇杆归中，按键归位
    ps2_task(): 每 20ms 拉低 ATT，收发一帧数据
    ps2_ok: 这一帧数据能不能用
*/
#include "main.h"
#include "ps2.h"
#include "dwt_delay.h"
#include "app.h"        /* 只为拿 APP_PS2_TEST 开关，不调用 app 层的函数 */

#define PS2_BIT_US      16

/* 一帧读不到就重试几轮，无线接收器经常要连着读几次才吐数据 */
#define PS2_READ_RETRY  4

#define PS2_PORT_DATA   GPIOA
#define PS2_PIN_DATA    GPIO_PIN_4     /* 输入：手柄 -> MCU */
#define PS2_PIN_CMD     GPIO_PIN_5     /* 输出：MCU  -> 手柄 */
#define PS2_PIN_ATT     GPIO_PIN_6     /* 输出：片选，低有效 */
#define PS2_PIN_CLK     GPIO_PIN_7     /* 输出：时钟 */

uint8_t ps2_ok;                  /* 这一帧数据能不能用 */
uint8_t ps2_btn1;                /* 按键组1（已取反：按下 = 1） */
uint8_t ps2_btn2;                /* 按键组2（已取反：按下 = 1） */
uint8_t ps2_lx, ps2_ly;          /* 左摇杆 左右 / 上下 */
uint8_t ps2_rx, ps2_ry;          /* 右摇杆 左右 / 上下 */

static uint8_t ps2_buf[9];       /* 收回来的 9 个字节 */

/* 诊断用：最近一帧原始数据 + 累计计数（合法不合法都记） */
uint8_t  ps2_raw[9];
uint32_t ps2_ok_cnt;
uint32_t ps2_bad_cnt;

/* 主机要发出去的 9 个字节：0x01 = 起始，0x42 = 请求数据 */
static const uint8_t ps2_cmd[9] =
{
    0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 让手柄切到"模拟模式"（红绿灯都亮）的三条标准配置命令。
   不切的话手柄上电默认是数字模式（ID=0x41），第 5~8 字节不是摇杆值，
   摇杆怎么推都读不到 —— 表现就是"能连上、但车不动"。
   原厂例程（ax_ps2.c）从来不发这三条，完全靠手柄自己一直保持在模拟模式；
   手柄一旦休眠 / 换电池 / 按过 MODE 键，就掉回数字模式，这也是本车会突然
   "摇杆没反应"的根因，所以这里主动切一次。 */
static const uint8_t ps2_cmd_cfg_enter[9] = { 0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t ps2_cmd_set_analog[9] = { 0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t ps2_cmd_cfg_exit[9]  = { 0x01, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A };

/* 诊断计数：试了几次切模拟模式、成功几次 */
uint32_t ps2_mode_try_cnt;
uint32_t ps2_mode_ok_cnt;

static void ps2_axes_to_center(void)
{
    ps2_btn1 = 0;
    ps2_btn2 = 0;
    ps2_lx = ps2_ly = PS2_STICK_CENTER;
    ps2_rx = ps2_ry = PS2_STICK_CENTER;
}

void ps2_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* CMD / ATT / CLK：推挽输出 */
    g.Pin   = PS2_PIN_CMD | PS2_PIN_ATT | PS2_PIN_CLK;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(PS2_PORT_DATA, &g);

    /* DATA：输入 + 内部上拉 */
    g.Pin  = PS2_PIN_DATA;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(PS2_PORT_DATA, &g);

    /* PC13 状态灯：低电平点亮，只能点灯，不能带负载 */
    g.Pin   = GPIO_PIN_13;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &g);

    /* 空闲态：ATT 高、CLK 高、CMD 高，保证不会启动 */
    HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_ATT | PS2_PIN_CLK | PS2_PIN_CMD, GPIO_PIN_SET);
    ps2_axes_to_center();
    ps2_ok = 0;
}

/* 收发 1 个字节，返回同时读回来的 1 个字节 */
static uint8_t ps2_xfer(uint8_t cmd)
{
    uint8_t res = 0;
    for (uint8_t ref = 0x01; ref != 0; ref <<= 1)
    {
        HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_CMD,(cmd & ref) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_CLK, GPIO_PIN_RESET);    delay_us(PS2_BIT_US);
        if(HAL_GPIO_ReadPin(PS2_PORT_DATA, PS2_PIN_DATA) == GPIO_PIN_SET) res |= ref;     
        HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_CLK, GPIO_PIN_SET);      delay_us(PS2_BIT_US);
    }
    return res;
}

/* 拉低 ATT，整帧 9 字节收发完再放开 */
static void ps2_read_frame(void)
{
    HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_ATT, GPIO_PIN_RESET);
    for (uint8_t i = 0; i < 9; i++) ps2_buf[i] = ps2_xfer(ps2_cmd[i]);
    HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_ATT, GPIO_PIN_SET);
}

/* 发一帧指定的命令，不管手柄回什么（只用来进/出配置、切模式） */
static void ps2_send(const uint8_t c[9])
{
    HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_ATT, GPIO_PIN_RESET);
    for (uint8_t i = 0; i < 9; i++) (void)ps2_xfer(c[i]);
    HAL_GPIO_WritePin(PS2_PORT_DATA, PS2_PIN_ATT, GPIO_PIN_SET);
}

/*
把"数字模式"的手柄切到"模拟模式"，返回 1 = 切完再读已经是模拟模式。
每条命令之间必须留 1~2ms 让手柄处理，切完还要再读一帧确认。
注意：无线接收器是透传的，这套配置命令能不能落地取决于手柄本身；
切不动也不会把东西弄坏，下位机会隔一秒再试一次。
*/
static uint8_t ps2_enable_analog(void)
{
    ps2_send(ps2_cmd_cfg_enter);  HAL_Delay(2);
    ps2_send(ps2_cmd_set_analog); HAL_Delay(2);
    ps2_send(ps2_cmd_cfg_exit);   HAL_Delay(2);
    ps2_send(ps2_cmd_cfg_exit);   HAL_Delay(2);   /* 部分副厂要连发两次退出才认 */

    ps2_read_frame();
    return (uint8_t)((ps2_buf[1] != 0x41) && (ps2_buf[1] != 0x00) && (ps2_buf[1] != 0xFF));
}

/* 手柄 ID：0x41 数字模式，0x73 / 0x53 模拟模式，0x79 部分副厂手柄 */
static uint8_t ps2_id_valid(uint8_t id)
{
#if APP_PS2_TEST
    /*
     * 测试模式放宽 ID 白名单：厂商官方例程（ax_ps2.c）根本不做 ID 校验，
     * 直接拿数据用。这里只要不是"全 1 浮空（0xFF）"或"全 0（0x00）"就当收到帧了，
     * 免得手柄的模式字节跟白名单不一样时被误判成"手柄坏了"。
     * 真实 ID 一直留在 ps2_raw[1] 里，调试器直接看。
     */
    return (uint8_t)((id != 0x00) && (id != 0xFF));
#else
    return (id == 0x41) || (id == 0x73) || (id == 0x53) || (id == 0x79);
#endif
}

void ps2_poll(void)
{
    uint8_t id = 0;
    static uint16_t digital_run;   /* 连续读到数字模式的帧数，用来限制"切模拟模式"的重试频率 */

    for (uint8_t i = 0; i < PS2_READ_RETRY; i++)
    {
        ps2_read_frame();
        id = ps2_buf[1];
        if ( ps2_id_valid(id)) break;
    }

    /* 诊断：不管这一帧合不合法，都把原始数据留下来，方便判断是"没数据"还是"数据不对" */
    for (uint8_t i = 0; i < 9; i++) ps2_raw[i] = ps2_buf[i];

    if (!ps2_id_valid(id))
    {
        /* 接收器没插 / 没对码 / 跑太远：整组退回中立，上层看到 ps2_ok == 0 就会立刻触发停车 */
        ps2_ok = 0;
        ps2_bad_cnt++;
        ps2_axes_to_center();
        return;
    }

    ps2_ok = 1;
    ps2_ok_cnt++;

    /* 数据字节是"按下 = 0"，取反之后用起来变成1，判断的时候1为真，直接用就行！ */
    ps2_btn1 = (uint8_t)(~ps2_buf[3]);
    ps2_btn2 = (uint8_t)(~ps2_buf[4]);

    if (id == 0x41)
    {
        /* 数字模式：第 5~8 字节不是摇杆值（只会是 0x00 / 0xFF），硬当中位算，不然车会自己冲出去 */
        ps2_lx = ps2_ly = PS2_STICK_CENTER;
        ps2_rx = ps2_ry = PS2_STICK_CENTER;

        /* 顺手把它切回模拟模式：上电后第 5 帧试一次（等接收器起来），之后每 50 帧（约 1 秒）试一次 */
        digital_run++;
        if ((digital_run == 5U) || ((digital_run % 50U) == 0U))
        {
            ps2_mode_try_cnt++;
            if (ps2_enable_analog()) ps2_mode_ok_cnt++;
        }
    }
    else{
        digital_run = 0;
        ps2_rx = ps2_buf[5];
        ps2_ry = ps2_buf[6];
        ps2_lx = ps2_buf[7];
        ps2_ly = ps2_buf[8];
    }
    return;
}
