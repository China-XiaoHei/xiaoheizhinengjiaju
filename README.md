# 小黑家居鱼缸照明方案

这个目录从零创建了三套工程：

- `STM32`：本地按键、继电器输出、串口状态同步
- `ESP8266`：WiFi 联网、MQTT 云端同步、和 STM32 串口桥接
- `小黑APP`：安卓远程控制 APP，设备名称固定为“鱼缸照明”

当前默认方案采用 MQTT 远程控制：

- 云端控制和本地物理按钮都只操作同一份灯状态
- `STM32` 作为继电器和本地按键的状态源
- `ESP8266` 负责把状态上传到云端，并把 APP 指令转给 `STM32`
- APP 只有一个主控按钮，根据实时状态显示“开灯”或“关灯”

## 默认硬件假设

- STM32：`BluePill / STM32F103C8T6`
- ESP8266：通用 `ESP-01 / ESP-12` 1MB 闪存模块
- 继电器：5V 单路继电器模块，控制端接 STM32 GPIO

这些假设都可以在各自工程的 `platformio.ini` 里修改。

## 通信逻辑

STM32 和 ESP8266 通过串口通信，默认协议如下：

- `CMD:SET:ON`
- `CMD:SET:OFF`
- `CMD:TOGGLE`
- `CMD:GET`
- `STATE:ON:button`
- `STATE:OFF:cloud`

MQTT 默认主题前缀：

- `xiaohei/fishlight/khome-20260419-a7c2/command`
- `xiaohei/fishlight/khome-20260419-a7c2/state`
- `xiaohei/fishlight/khome-20260419-a7c2/availability`

## 目录说明

- [STM32/platformio.ini](/E:/ST工程/小黑家居/STM32/platformio.ini)
- [STM32/src/main.cpp](/E:/ST工程/小黑家居/STM32/src/main.cpp)
- [ESP8266/platformio.ini](/E:/ST工程/小黑家居/ESP8266/platformio.ini)
- [ESP8266/src/main.cpp](/E:/ST工程/小黑家居/ESP8266/src/main.cpp)
- [小黑APP/FishLightApp/app/src/main/java/com/xiaohei/fishlight/MainActivity.java](/E:/ST工程/小黑家居/小黑APP/FishLightApp/app/src/main/java/com/xiaohei/fishlight/MainActivity.java)
- [docs/接线说明.md](/E:/ST工程/小黑家居/docs/接线说明.md)

## 当前构建结果

- 安卓 APK 已生成：`小黑APP/output/小黑APP-鱼缸照明-v2.0.0-release.apk`
- `ESP8266` 固件已编译通过
- `STM32` 固件已编译通过
- `ESP8266` 已尝试烧录一次，但设备未进入下载模式，暂未刷入成功
