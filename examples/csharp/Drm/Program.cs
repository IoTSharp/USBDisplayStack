// SPDX-License-Identifier: GPL-2.0-only

using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

internal static unsafe class Program
{
    private const uint DrmModeConnected = 1;
    private const uint DrmModeTypePreferred = 1U << 3;
    private const uint DrmFormatXrgb8888 = 0x34325258;
    private const ulong DrmIoctlModeCreateDumb = 0xc02064b2;
    private const ulong DrmIoctlModeMapDumb = 0xc01064b3;
    private const ulong DrmIoctlModeDestroyDumb = 0xc00464b4;
    private const int OpenReadWrite = 2;
    private const int OpenCloseOnExec = 0x80000;
    private const int ProtRead = 1;
    private const int ProtWrite = 2;
    private const int MapShared = 1;

    private static volatile bool stopRequested;

    private static int Main(string[] args)
    {
        if (args.Length > 2)
        {
            Console.Error.WriteLine("Usage: DrmExample [DRM_CARD [SECONDS]]");
            return 2;
        }

        string device = args.Length > 0 ? args[0] : "/dev/dri/card1";
        double seconds = args.Length > 1
            ? double.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture)
            : 0;

        try
        {
            using DrmTarget target = DrmTarget.Open(device);
            Console.CancelKeyPress += (_, eventArgs) =>
            {
                stopRequested = true;
                eventArgs.Cancel = true;
            };
            Stopwatch runtime = Stopwatch.StartNew();
            uint frame = 0;
            while (!stopRequested &&
                   (seconds <= 0 || runtime.Elapsed.TotalSeconds < seconds))
            {
                target.Draw(frame++);
                Thread.Sleep(33);
            }

            Console.WriteLine(
                $"Rendered {frame} frames to {device} ({target.Width}x{target.Height}).");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"DrmExample: {exception.Message}");
            return 1;
        }
    }

    private sealed class DrmTarget : IDisposable
    {
        private int descriptor = -1;
        private uint connectorId;
        private uint crtcId;
        private uint framebufferId;
        private DrmModeCreateDumb dumb;
        private IntPtr mapping = new(-1);
        private bool disposed;

        private DrmTarget()
        {
        }

        public int Width => checked((int)dumb.Width);

        public int Height => checked((int)dumb.Height);

        public static DrmTarget Open(string path)
        {
            DrmTarget target = new();
            try
            {
                target.descriptor = Native.open(path, OpenReadWrite | OpenCloseOnExec);
                if (target.descriptor < 0)
                {
                    ThrowLastError("open");
                }

                target.VerifyDriver();
                DrmModeModeInfo mode = target.SelectConnector();
                target.CreateFramebuffer(mode);
                return target;
            }
            catch
            {
                target.Dispose();
                throw;
            }
        }

        public void Draw(uint frameNumber)
        {
            Span<byte> buffer = new((void*)mapping, checked((int)dumb.Size));
            int marker = (int)(frameNumber * 9 % dumb.Width);
            for (int y = 0; y < Height; y++)
            {
                Span<uint> row = MemoryMarshal.Cast<byte, uint>(
                    buffer.Slice(checked(y * (int)dumb.Pitch), Width * 4));
                for (int x = 0; x < Width; x++)
                {
                    byte red = (byte)(24 + 100 * x / Math.Max(1, Width - 1));
                    byte green = (byte)(30 + 80 * y / Math.Max(1, Height - 1));
                    byte blue = (byte)(44 + ((x / 32 + y / 32) & 1) * 16);
                    if (Math.Abs(x - marker) < 8)
                    {
                        red = 101;
                        green = 167;
                        blue = 255;
                    }

                    row[x] = (uint)(red << 16 | green << 8 | blue);
                }
            }

            DrmModeClip clip = new()
            {
                X1 = 0,
                Y1 = 0,
                X2 = checked((ushort)Width),
                Y2 = checked((ushort)Height),
            };
            if (Native.drmModeDirtyFB(descriptor, framebufferId, ref clip, 1) != 0)
            {
                ThrowLastError("drmModeDirtyFB");
            }
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }
            disposed = true;
            if (descriptor >= 0 && crtcId != 0)
            {
                Native.drmModeSetCrtcDisable(
                    descriptor, crtcId, 0, 0, 0, IntPtr.Zero, 0, IntPtr.Zero);
            }
            if (mapping != new IntPtr(-1))
            {
                Native.munmap(mapping, (nuint)dumb.Size);
                mapping = new IntPtr(-1);
            }
            if (descriptor >= 0 && framebufferId != 0)
            {
                Native.drmModeRmFB(descriptor, framebufferId);
            }
            if (descriptor >= 0 && dumb.Handle != 0)
            {
                DrmModeDestroyDumb destroy = new() { Handle = dumb.Handle };
                Native.ioctlDestroyDumb(descriptor, DrmIoctlModeDestroyDumb, ref destroy);
            }
            if (descriptor >= 0)
            {
                Native.close(descriptor);
                descriptor = -1;
            }
        }

        private void VerifyDriver()
        {
            IntPtr versionPointer = Native.drmGetVersion(descriptor);
            if (versionPointer == IntPtr.Zero)
            {
                ThrowLastError("drmGetVersion");
            }
            try
            {
                DrmVersion version = Marshal.PtrToStructure<DrmVersion>(versionPointer);
                string name = Marshal.PtrToStringAnsi(version.Name, version.NameLength) ?? "";
                if (name != "usbdisplay")
                {
                    throw new InvalidOperationException(
                        $"Refusing DRM driver '{name}'; expected usbdisplay.");
                }
            }
            finally
            {
                Native.drmFreeVersion(versionPointer);
            }
        }

        private DrmModeModeInfo SelectConnector()
        {
            IntPtr resourcesPointer = Native.drmModeGetResources(descriptor);
            if (resourcesPointer == IntPtr.Zero)
            {
                ThrowLastError("drmModeGetResources");
            }
            try
            {
                DrmModeResources resources =
                    Marshal.PtrToStructure<DrmModeResources>(resourcesPointer);
                for (int index = 0; index < resources.CountConnectors; index++)
                {
                    uint candidateId = ReadUInt32(resources.Connectors, index);
                    IntPtr connectorPointer = Native.drmModeGetConnector(
                        descriptor, candidateId);
                    if (connectorPointer == IntPtr.Zero)
                    {
                        continue;
                    }
                    try
                    {
                        DrmModeConnector connector =
                            Marshal.PtrToStructure<DrmModeConnector>(connectorPointer);
                        if (connector.Connection != DrmModeConnected ||
                            connector.CountModes == 0)
                        {
                            continue;
                        }

                        connectorId = connector.ConnectorId;
                        crtcId = SelectCrtc(resources, connector);
                        if (crtcId == 0)
                        {
                            continue;
                        }

                        DrmModeModeInfo selected = ReadMode(connector.Modes, 0);
                        for (int modeIndex = 0;
                             modeIndex < connector.CountModes;
                             modeIndex++)
                        {
                            DrmModeModeInfo candidate =
                                ReadMode(connector.Modes, modeIndex);
                            if ((candidate.Type & DrmModeTypePreferred) != 0)
                            {
                                selected = candidate;
                                break;
                            }
                        }
                        return selected;
                    }
                    finally
                    {
                        Native.drmModeFreeConnector(connectorPointer);
                    }
                }
            }
            finally
            {
                Native.drmModeFreeResources(resourcesPointer);
            }

            throw new InvalidOperationException("No connected usbdisplay connector was found.");
        }

        private uint SelectCrtc(
            DrmModeResources resources, DrmModeConnector connector)
        {
            if (connector.EncoderId != 0)
            {
                uint active = ReadEncoderCrtc(connector.EncoderId, out _);
                if (active != 0)
                {
                    return active;
                }
            }

            for (int encoderIndex = 0;
                 encoderIndex < connector.CountEncoders;
                 encoderIndex++)
            {
                uint encoderId = ReadUInt32(connector.Encoders, encoderIndex);
                uint active = ReadEncoderCrtc(encoderId, out uint possible);
                if (active != 0)
                {
                    return active;
                }
                for (int crtcIndex = 0; crtcIndex < resources.CountCrtcs; crtcIndex++)
                {
                    if ((possible & (1U << crtcIndex)) != 0)
                    {
                        return ReadUInt32(resources.Crtcs, crtcIndex);
                    }
                }
            }
            return 0;
        }

        private uint ReadEncoderCrtc(uint encoderId, out uint possibleCrtcs)
        {
            possibleCrtcs = 0;
            IntPtr encoderPointer = Native.drmModeGetEncoder(descriptor, encoderId);
            if (encoderPointer == IntPtr.Zero)
            {
                return 0;
            }
            try
            {
                DrmModeEncoder encoder =
                    Marshal.PtrToStructure<DrmModeEncoder>(encoderPointer);
                possibleCrtcs = encoder.PossibleCrtcs;
                return encoder.CrtcId;
            }
            finally
            {
                Native.drmModeFreeEncoder(encoderPointer);
            }
        }

        private void CreateFramebuffer(DrmModeModeInfo mode)
        {
            dumb = new DrmModeCreateDumb
            {
                Width = mode.HDisplay,
                Height = mode.VDisplay,
                BitsPerPixel = 32,
            };
            if (Native.ioctlCreateDumb(
                    descriptor, DrmIoctlModeCreateDumb, ref dumb) != 0)
            {
                ThrowLastError("DRM_IOCTL_MODE_CREATE_DUMB");
            }

            uint[] handles = [dumb.Handle, 0, 0, 0];
            uint[] pitches = [dumb.Pitch, 0, 0, 0];
            uint[] offsets = [0, 0, 0, 0];
            if (Native.drmModeAddFB2(
                    descriptor, dumb.Width, dumb.Height, DrmFormatXrgb8888,
                    handles, pitches, offsets, out framebufferId, 0) != 0)
            {
                ThrowLastError("drmModeAddFB2");
            }

            DrmModeMapDumb map = new() { Handle = dumb.Handle };
            if (Native.ioctlMapDumb(descriptor, DrmIoctlModeMapDumb, ref map) != 0)
            {
                ThrowLastError("DRM_IOCTL_MODE_MAP_DUMB");
            }
            mapping = Native.mmap(
                IntPtr.Zero, (nuint)dumb.Size, ProtRead | ProtWrite,
                MapShared, descriptor, checked((long)map.Offset));
            if (mapping == new IntPtr(-1))
            {
                ThrowLastError("mmap");
            }

            uint[] connectors = [connectorId];
            if (Native.drmModeSetCrtc(
                    descriptor, crtcId, framebufferId, 0, 0,
                    connectors, 1, ref mode) != 0)
            {
                ThrowLastError("drmModeSetCrtc");
            }
        }
    }

    private static uint ReadUInt32(IntPtr pointer, int index) =>
        unchecked((uint)Marshal.ReadInt32(pointer, index * sizeof(uint)));

    private static DrmModeModeInfo ReadMode(IntPtr pointer, int index) =>
        Marshal.PtrToStructure<DrmModeModeInfo>(
            IntPtr.Add(pointer, index * sizeof(DrmModeModeInfo)));

    private static void ThrowLastError(string operation) =>
        throw new Win32Exception(Marshal.GetLastWin32Error(), operation);

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmVersion
    {
        public int Major;
        public int Minor;
        public int PatchLevel;
        public int NameLength;
        public IntPtr Name;
        public int DateLength;
        public IntPtr Date;
        public int DescriptionLength;
        public IntPtr Description;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeResources
    {
        public int CountFramebuffers;
        public IntPtr Framebuffers;
        public int CountCrtcs;
        public IntPtr Crtcs;
        public int CountConnectors;
        public IntPtr Connectors;
        public int CountEncoders;
        public IntPtr Encoders;
        public uint MinWidth;
        public uint MaxWidth;
        public uint MinHeight;
        public uint MaxHeight;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeConnector
    {
        public uint ConnectorId;
        public uint EncoderId;
        public uint ConnectorType;
        public uint ConnectorTypeId;
        public uint Connection;
        public uint WidthMillimeters;
        public uint HeightMillimeters;
        public uint SubPixel;
        public int CountModes;
        public IntPtr Modes;
        public int CountProperties;
        public IntPtr Properties;
        public IntPtr PropertyValues;
        public int CountEncoders;
        public IntPtr Encoders;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeEncoder
    {
        public uint EncoderId;
        public uint EncoderType;
        public uint CrtcId;
        public uint PossibleCrtcs;
        public uint PossibleClones;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    private unsafe struct DrmModeModeInfo
    {
        public uint Clock;
        public ushort HDisplay;
        public ushort HSyncStart;
        public ushort HSyncEnd;
        public ushort HTotal;
        public ushort HSkew;
        public ushort VDisplay;
        public ushort VSyncStart;
        public ushort VSyncEnd;
        public ushort VTotal;
        public ushort VScan;
        public uint VRefresh;
        public uint Flags;
        public uint Type;
        public fixed byte Name[32];
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeCreateDumb
    {
        public uint Height;
        public uint Width;
        public uint BitsPerPixel;
        public uint Flags;
        public uint Handle;
        public uint Pitch;
        public ulong Size;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeMapDumb
    {
        public uint Handle;
        public uint Pad;
        public ulong Offset;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeDestroyDumb
    {
        public uint Handle;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DrmModeClip
    {
        public ushort X1;
        public ushort Y1;
        public ushort X2;
        public ushort Y2;
    }

    private static class Native
    {
        [DllImport("libc", SetLastError = true)]
        internal static extern int open(string path, int flags);

        [DllImport("libc", SetLastError = true)]
        internal static extern int close(int descriptor);

        [DllImport("libc", SetLastError = true)]
        internal static extern IntPtr mmap(
            IntPtr address, nuint length, int protection, int flags,
            int descriptor, long offset);

        [DllImport("libc", SetLastError = true)]
        internal static extern int munmap(IntPtr address, nuint length);

        [DllImport("libc", EntryPoint = "ioctl", SetLastError = true)]
        internal static extern int ioctlCreateDumb(
            int descriptor, ulong request, ref DrmModeCreateDumb value);

        [DllImport("libc", EntryPoint = "ioctl", SetLastError = true)]
        internal static extern int ioctlMapDumb(
            int descriptor, ulong request, ref DrmModeMapDumb value);

        [DllImport("libc", EntryPoint = "ioctl", SetLastError = true)]
        internal static extern int ioctlDestroyDumb(
            int descriptor, ulong request, ref DrmModeDestroyDumb value);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern IntPtr drmGetVersion(int descriptor);

        [DllImport("libdrm.so.2")]
        internal static extern void drmFreeVersion(IntPtr version);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern IntPtr drmModeGetResources(int descriptor);

        [DllImport("libdrm.so.2")]
        internal static extern void drmModeFreeResources(IntPtr resources);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern IntPtr drmModeGetConnector(
            int descriptor, uint connectorId);

        [DllImport("libdrm.so.2")]
        internal static extern void drmModeFreeConnector(IntPtr connector);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern IntPtr drmModeGetEncoder(int descriptor, uint encoderId);

        [DllImport("libdrm.so.2")]
        internal static extern void drmModeFreeEncoder(IntPtr encoder);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern int drmModeAddFB2(
            int descriptor, uint width, uint height, uint pixelFormat,
            [In] uint[] handles, [In] uint[] pitches, [In] uint[] offsets,
            out uint framebufferId, uint flags);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern int drmModeRmFB(int descriptor, uint framebufferId);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern int drmModeSetCrtc(
            int descriptor, uint crtcId, uint framebufferId,
            uint x, uint y, [In] uint[] connectors, int connectorCount,
            ref DrmModeModeInfo mode);

        [DllImport("libdrm.so.2", EntryPoint = "drmModeSetCrtc", SetLastError = true)]
        internal static extern int drmModeSetCrtcDisable(
            int descriptor, uint crtcId, uint framebufferId,
            uint x, uint y, IntPtr connectors, int connectorCount, IntPtr mode);

        [DllImport("libdrm.so.2", SetLastError = true)]
        internal static extern int drmModeDirtyFB(
            int descriptor, uint framebufferId, ref DrmModeClip clip,
            uint clipCount);
    }
}
