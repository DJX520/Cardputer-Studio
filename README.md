<p align="center">
  <strong style="font-size:2em">Cardputer Studio</strong><br>
  <em>口袋乐坊 · Pocket Music Workshop</em>
</p>

<p align="center">
  <strong>One Cardputer, a whole pocket band.</strong><br>
  <strong>一台 Cardputer，一支口袋乐队。</strong>
</p>

<p align="center">
  合成器 · 收音机 · 听音识调 —— 三位一体，装进 M5Stack Cardputer ADV<br>
  <em>Synth · Radio · Key-Detection — one unified music firmware.</em>
</p>

---

# Cardputer Studio · 口袋乐坊

## 简介 · About

**Cardputer Studio（口袋乐坊）** 把三个开源 Cardputer 音乐固件（CardputerModularSynth / GLIDE / WebRadio）**去重整合、统一界面**，做成了 M5Stack Cardputer ADV 上的一套完整音乐系统：一台 **18 种波形的可演奏合成器**、一台**网络收音机**，还有一个能用麦克风**听出歌曲调性与节拍**的「耳朵」。

> **Cardputer Studio (Pocket Music Workshop)** unifies three open-source Cardputer music firmwares into one polished system: a playable **18-waveform synthesizer**, an **internet radio**, and a microphone that **hears a song's key and tempo** — all on the M5Stack Cardputer ADV.

---

## 作用 · What it's for

- 🎹 **零基础弹旋律**：音阶锁定，怎么按都不跑调，随手就能弹出好听的东西。
- 🎛 **当合成器音色库**：18 种波形 + 随机生成 + 变异，一键调出新音色。
- 🥁 **当伴奏机**：和弦自动进行 + 右手 solo + 鼓组，像自带一支乐队。
- 🎤 **听歌学调**：对着正在放的歌长按 `fn`+`k`，自动定调 + 定 BPM。
- 🔁 **录音循环**：实时录进 SD 卡，循环播放、可叠加，自己给自己伴奏。
- 📻 **听网络电台**：连 WiFi 听全球电台。

---

## 原理 · How it works

### 硬件
- **ESP32-S3FN8**：240 MHz 双核、8 MB Flash、**无 PSRAM**（512 KB SRAM）。
- **ES8311 单声道编解码器**：I2S 接口，输出喇叭 + 3.5mm，输入 MEMS 麦克风。
- **ST7789 显示屏**（240×135）、**TCA8418 键盘**（56 键，I2C）、**BMI270 六轴 IMU**（陀螺仪 + 加速度计）。

### 架构
- **双核分工**：核 1 专职音频（I2S + 合成 DSP，5000 周期/样本预算），核 0 跑界面/键盘/SD/I2C。参数与音符通过 `portMUX` 自旋锁跨核传递，音频路径无阻塞。
- **音频路径**：I2S **全双工主模式**——TX 驱动 DAC、RX 采集麦克风 ADC，共享 BCLK/LRCK；ES8311 用实测的「无 MCLK」寄存器序列初始化，48 kHz / 16 bit。

### 合成引擎
- **18 种波形**：虚拟模拟（锯齿/方波/PWM/超锯齿）、2-OP 与 6-OP FM、波表、颗粒、加法、和弦、群蜂、语音共振峰、波形地形、弦乐机、芯片、Karplus-Strong 拨弦、模态谐振铃、鼓组。
- **信号链**：PolyBLEP 抗混叠振荡器 → 音色处理（FM/折叠/同步/环形）→ SVF 四模滤波 → ADSR + LFO → 合唱/延迟/混响 → 主音量限幅。
- **音阶锁定「琴弦」**：四行键盘按四度定弦、每键映射音阶度，滑音/锤弦/勾弦模拟无品弦乐。

### 听音识调
麦克风 → ES8311 ADC → I2S RX → **FFT 色度图** → **Krumhansl-Schmuckler 调性模板**（24 个大小调相关）→ 能量包络自相关估节拍。

### SD 循环器
双环形缓冲桥接核 1（音频）与核 0（SD 读写），实时把演奏写成 48 kHz PCM，循环播放并支持**叠加合拍并入**，跨重启保留。

---

## 创新点 · Highlights

1. **三固件合一**：把「模块化合成器 + 可演奏合成器 + 收音机」去重整合成一套统一系统，一个启动器、一套主题。
2. **Plaits 引擎全手工移植**：18 种波形自包含实现，零外部 DSP 依赖，在无 PSRAM 的 512 KB SRAM 内跑下完整信号链。
3. **麦克风听音识调**：色度图 + K-S 调性模板 + 节拍自相关，对着歌就能自动定调。
4. **SD 长循环器 + 叠加**：把实际演奏实时写进 SD，可录数分钟，叠加层合拍并入、跨重启保留。
5. **体感双音色交叉淡化**：BMI270 倾斜，在当前音色与上一音色间做**参数级连续插值**。
6. **自包含键盘驱动**：直接 I2C 读写 TCA8418，多键和弦可靠、无外部库依赖、不锁死。
7. **内存精心优化**：在无 PSRAM 的约束下，用静态缓冲 + 常量表 + 长度裁剪把内存压在预算内。

---

## 功能总览 · Features

| 模式 | 功能 |
|---|---|
| 🎹 **SYNTH** | 音阶锁定琴弦+滑音、和弦/和声/琶音/延音踏板/和弦自动进行、18 波形、鼓组、节拍器、SD 循环器（含叠加）、体感、10 音色槽、随机/变异/撤销、SD 音色库、麦克风听音识调 |
| 📻 **RADIO** | WiFi 流媒体、SD 电台表、音量/静音/电平表 |
| ⚙️ **SETTINGS** | 6 套主题、亮度、WiFi 记忆管理 |

---

## 快速开始 · Quick Start

**依赖库**（Arduino 库管理器安装）：`M5Unified`、`M5GFX`、`ESP32-audioI2S`。

**烧录**：
1. 板包加 M5Stack：`https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json`
2. 选板 **M5Stack Cardputer**，分区 **Huge APP (3MB No OTA / 1MB SPIFFS)**，Flash 8 MB。
3. 打开 `CardputerStudio.ino` 上传。

> 详细步骤见 [`docs/烧录教程.md`](docs/烧录教程.md)。

---

## 文档 · Docs

| 文档 | 内容 |
|---|---|
| [`docs/客户手册.md`](docs/客户手册.md) | 面向使用者的玩法指南 + 按键速查 + FAQ |
| [`docs/烧录教程.md`](docs/烧录教程.md) | 逐步烧录 + 常见问题 |
| [`docs/波形音色对照表.md`](docs/波形音色对照表.md) | 18 种波形 ↔ 音色对照 + 调校说明 |

---

## 许可证 · License

- 本项目自身代码：**MIT**。
- 参考与灵感（各自保留原许可）：[CardputerModularSynth](https://github.com/2540825244/CardputerModularSynth)（MIT，音频路径/键盘/音阶）、[GLIDE-Synth-Cardputer-ADV](https://github.com/CHARL3X/GLIDE-Synth-Cardputer-ADV)（PolyForm 非商用，演奏体验，仅按手册重新实现、未用其代码）、[WebRadio_WuSiU_Cardputer_Adv](https://github.com/WuSiU/WebRadio_WuSiU_Cardputer_Adv)（MIT，收音机）。
- 第三方库：M5Unified / M5GFX（M5Stack，MIT）、ESP32-audioI2S（schreibfaul1）。

> 按原样提供，无任何担保；请在真机上验证音频与硬件时序相关功能。
