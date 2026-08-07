// SPDX-License-Identifier: GPL-2.0-only

using System.Buffers.Binary;
using System.Diagnostics;

internal static class Program
{
    private static volatile bool stopRequested;

    private static int Main(string[] args)
    {
        if (args.Length > 2)
        {
            Console.Error.WriteLine("Usage: FbdevExample [FBDEV [SECONDS]]");
            return 2;
        }

        string device = args.Length > 0 ? args[0] : "/dev/fb1";
        double seconds = args.Length > 1
            ? double.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture)
            : 0;

        try
        {
            Run(device, seconds);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"FbdevExample: {exception.Message}");
            return 1;
        }
    }

    private static void Run(string device, double seconds)
    {
        string node = Path.GetFileName(device);
        if (node == "fb0")
        {
            throw new InvalidOperationException("Refusing the primary framebuffer /dev/fb0.");
        }

        string sysfs = Path.Combine("/sys/class/graphics", node);
        string driverName = File.ReadAllText(Path.Combine(sysfs, "name")).Trim();
        if (driverName != "usbdisplay")
        {
            throw new InvalidOperationException(
                $"{device} is '{driverName}', expected the usbdisplay framebuffer.");
        }

        string[] size = File.ReadAllText(Path.Combine(sysfs, "virtual_size"))
            .Trim().Split(',');
        int width = int.Parse(size[0]);
        int height = int.Parse(size[1]);
        int bitsPerPixel = int.Parse(
            File.ReadAllText(Path.Combine(sysfs, "bits_per_pixel")).Trim());
        if (bitsPerPixel != 32)
        {
            throw new NotSupportedException($"Expected XRGB8888, got {bitsPerPixel} bpp.");
        }

        int stride = ReadStride(sysfs, width);
        byte[] frame = new byte[checked(stride * height)];
        using FileStream stream = new(
            device, FileMode.Open, FileAccess.Write, FileShare.ReadWrite,
            bufferSize: 64 * 1024, FileOptions.None);

        Console.CancelKeyPress += (_, eventArgs) =>
        {
            stopRequested = true;
            eventArgs.Cancel = true;
        };
        Stopwatch runtime = Stopwatch.StartNew();
        uint frameNumber = 0;
        while (!stopRequested && (seconds <= 0 || runtime.Elapsed.TotalSeconds < seconds))
        {
            DrawFrame(frame, width, height, stride, frameNumber++);
            stream.Seek(0, SeekOrigin.Begin);
            stream.Write(frame);
            stream.Flush();
            Thread.Sleep(33);
        }

        Console.WriteLine($"Rendered {frameNumber} frames to {device} ({width}x{height}).");
    }

    private static int ReadStride(string sysfs, int width)
    {
        string path = Path.Combine(sysfs, "stride");
        return File.Exists(path) ? int.Parse(File.ReadAllText(path).Trim()) : width * 4;
    }

    private static void DrawFrame(
        Span<byte> frame, int width, int height, int stride, uint frameNumber)
    {
        int marker = (int)(frameNumber * 9 % (uint)Math.Max(1, width));
        for (int y = 0; y < height; y++)
        {
            Span<byte> row = frame.Slice(y * stride, width * 4);
            for (int x = 0; x < width; x++)
            {
                byte red = (byte)(28 + 90 * x / Math.Max(1, width - 1));
                byte green = (byte)(34 + 75 * y / Math.Max(1, height - 1));
                byte blue = (byte)(48 + ((x / 32 + y / 32) & 1) * 18);
                if (Math.Abs(x - marker) < 8)
                {
                    red = 76;
                    green = 224;
                    blue = 168;
                }

                uint xrgb = (uint)(red << 16 | green << 8 | blue);
                BinaryPrimitives.WriteUInt32LittleEndian(row.Slice(x * 4, 4), xrgb);
            }
        }
    }
}
