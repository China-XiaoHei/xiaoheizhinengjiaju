# 小黑APP

这个目录现在包含两部分内容：

- `FishLightApp`：安卓源码工程
- `output/小黑APP-鱼缸照明-v2.0.0-release.apk`：已经打好的安装包

## APP 功能

- 设备名称固定为 `鱼缸照明`
- 只保留一个主控按钮
- 通过 MQTT 远程网络控制
- 本地物理按钮状态会自动同步回 APP
- APP 根据最新状态自动显示应该点击 `开灯` 还是 `关灯`

## 构建方式

本机已经验证可用的构建方式：

- 直接使用 `E:\android-toolchain\gradle\gradle-8.7\bin\gradle.bat`
- 工程内也已经补了 `gradle wrapper`

源码主入口：

- [FishLightApp/app/src/main/java/com/xiaohei/fishlight/MainActivity.java](/E:/ST工程/小黑家居/小黑APP/FishLightApp/app/src/main/java/com/xiaohei/fishlight/MainActivity.java)
