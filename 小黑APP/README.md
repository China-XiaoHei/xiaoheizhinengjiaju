# 小黑APP

安卓控制端在 `FishLightApp` 目录。

## 功能

- 设备名固定显示：`鱼缸照明`
- 显示状态：
  - 鱼缸照明开/关
  - 鱼泵开/关
  - 总开关状态
  - 设备总状态
  - 最后来源
- 两种控制模式：
  - 云端 MQTT
  - ESP 本地 HTTP

## 打包

```powershell
cd E:\ST工程\小黑家居\小黑APP\FishLightApp
.\gradlew.bat clean assembleRelease
```

打包后 APK 会复制到：

- `E:\ST工程\小黑家居\小黑APP\output\小黑APP-鱼缸控制-v3.0.0-release.apk`
