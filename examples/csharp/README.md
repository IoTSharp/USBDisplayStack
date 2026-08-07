# C# framebuffer and DRM examples

The two .NET 8 console applications render animated XRGB8888 patterns without
touching the primary display:

```bash
dotnet run --project examples/csharp/Fbdev -- /dev/fb1
dotnet run --project examples/csharp/Drm -- /dev/dri/card1
```

The fbdev example verifies the sysfs framebuffer name before writing. The DRM
example calls libdrm through P/Invoke and verifies the DRM driver name before
creating and dirtying a dumb buffer. Both refuse devices that are not owned by
USBDisplayStack. The DRM runtime must provide `libdrm.so.2`.
