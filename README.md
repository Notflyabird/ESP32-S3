# ESP32-S3 ESP-SR Dou Dizhu Scorekeeper

Pure ESP-IDF 5.3.5 project for an ESP32-S3 N16R8 board with a single INMP441 microphone.

## Hardware

### 引脚总览

| 外设 | 信号 | GPIO | 说明 |
|------|------|------|------|
| INMP441 (I2S0) | BCLK | GPIO4 | I2S 位时钟 |
| INMP441 (I2S0) | WS | GPIO5 | I2S 左右通道时钟 |
| INMP441 (I2S0) | SD | GPIO6 | I2S 数据输入 |
| ST7789 LCD (SPI2) | BL | GPIO7 | 背光控制 |
| ST7789 LCD (SPI2) | RST | GPIO8 | 硬件复位 |
| ST7789 LCD (SPI2) | DC | GPIO9 | 数据/命令选择 |
| ST7789 LCD (SPI2) | CS | GPIO10 | SPI 片选 |
| ST7789 LCD (SPI2) | MOSI | GPIO11 | SPI 主机输出 |
| ST7789 LCD (SPI2) | SCLK | GPIO12 | SPI 时钟 |
| MAX98357A (I2S1) | BCLK | GPIO15 | I2S 位时钟 |
| MAX98357A (I2S1) | LRCLK | GPIO16 | I2S 左右通道时钟 |
| MAX98357A (I2S1) | DOUT | GPIO17 | I2S 数据输出 |
| MAX98357A | SD | GPIO18 | 静音控制 (HIGH=播放/LOW=静音) |

> 共占用 13 个 GPIO（GPIO4 ~ GPIO18），INMP441 的 L/R 引脚接地。
> 已配置外设：INMP441 麦克风 (I2S0)、ST7789 LCD (SPI2, 240×320)、MAX98357A 功放 (I2S1)。

## Voice Commands

Say `Hi ESP`, then use one command within six seconds.

Scoring command format:

```text
<player> di zhu <result> <points> fen
```

Players:

- `yi hao`
- `er hao`
- `san hao`

Results:

- `ying`
- `shu`

Supported point phrases:

- `liang fen`
- `si fen`
- `liu fen`
- `ba fen`
- `yi shi fen`
- `yi shi er fen`
- `yi shi si fen`
- `yi shi liu fen`
- `yi shi ba fen`
- `er shi fen`

Examples:

- `yi hao di zhu ying liang fen`
- `er hao di zhu shu ba fen`
- `san hao di zhu ying er shi fen`

Other commands:

- `cha xun fen shu`
- `chong zhi suo you fen shu`

## Scoring

If player X landlord wins N points:

- Player X: `+N`
- Other two players: `-N/2` each

If player X landlord loses N points:

- Player X: `-N`
- Other two players: `+N/2` each

Supported N values are 2, 4, 6, 8, 10, 12, 14, 16, 18, and 20.

## Build And Flash

Open an ESP-IDF 5.3.5 terminal and run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```
