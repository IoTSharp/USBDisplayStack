# 支持设备清单

本清单记录已经有匹配 USBDisplayStack 后端并完成硬件验证的 USB 显示设备。
不能只凭商品名称判断支持情况，必须同时匹配 USB 身份、接口布局和传输协议。

## 已验证设备

| 设备 | USB 身份 | 状态 | 后端 |
| --- | --- | --- | --- |
| 绿联 USB 转 HDMI 转换器（黑色 USB-A 款） | VID `185b`、PID `2d1d`；描述符为 `Compro` / `Mirroring Suit` | 实验性；参考设备已目视验证 1920x1080 LVGL 实时画面 | Actions Micro HID H.264 |

![绿联 USB 转 HDMI 转换器抠图](assets/devices/ugreen-usb-hdmi-185b-2d1d.png)

上图是根据用户提供的商品图片抠出的设备图，只用于说明外壳外观；外观相似的
绿联其他批次不代表使用相同芯片或协议。

### 绿联 USB 转 HDMI 转换器

- 匹配 USB ID：`185b:2d1d`。
- 已观察到两个 HID 接口：物理 `input0` 和 `input1`，通常对应两个
  `/dev/hidraw*` 节点。
- 传输协议：4096 字节厂商 HID 报文，承载 `RRIM/TADV` Annex-B H.264 和
  `_PPA` 心跳。
- 启动要求：需要用户有权使用的 `DPRPL001` 模板。参考固件需要
  `bootstrap=full`，先发送捕获流，再在同一传输会话中切换到实时 H.264。
- 验证结果：物理拔插 USB 重新上电后，参考设备显示了新的 LVGL 控件和滚动条，
  实时后端及画面生产进程持续运行。重复热插拔和长时间运行验证仍待完成。

选择后端前可先检查设备身份：

```bash
lsusb -d 185b:2d1d
```

DisplayLink、Trigger、MacroSilicon、Fresco Logic 等其他 USB 转 HDMI 产品使用
不同协议，不包含在本条目内。
