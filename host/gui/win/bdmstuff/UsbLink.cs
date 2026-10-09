using System;
using System.Threading;
using LibUsbDotNet;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;
using NameMeDLL;

namespace bdmstuff
{
    // The adapter's USB link (ffff:0107; WinUSB loads for it on its own): the
    // core's frames go out on EP 0x03, a thread hands what comes back on 0x81
    // to the core. Open one around each operation.
    sealed class UsbLink : IDisposable
    {
        const ushort VID = 0xFFFF, PID_APP = 0x0107, PID_BOOT = 0x0108;
        const int TOY_BOOT_FW = 0x0200; // first firmware with the bootloader

        readonly UsbContext ctx = new UsbContext();
        readonly IUsbDevice dev;
        readonly UsbEndpointReader reader;
        readonly UsbEndpointWriter writer;
        readonly noNameGizmo gizmo;
        readonly Thread thread;
        volatile bool stop;

        public UsbLink(noNameGizmo gizmo)
        {
            this.gizmo = gizmo;
            dev = Find(ctx, PID_APP);
            if (dev == null)
            {
                ctx.Dispose();
                throw new InvalidOperationException("No adapter found (USB ffff:0107; firmware before 2.2 needs the WinUSB driver from Zadig, see the README)");
            }

            try
            {
                // A USB reset puts the firmware's frame parser on a frame boundary
                try { dev.ResetDevice(); } catch (UsbException) { }
                dev.ClaimInterface(0);
                // Only there on the CDC layout of firmware before 2.2, which
                // still has to be told to update
                try { dev.ClaimInterface(1); } catch (UsbException) { }
                reader = dev.OpenEndpointReader(ReadEndpointID.Ep01, 4096);
                writer = dev.OpenEndpointWriter(WriteEndpointID.Ep03);
                reader.ReadFlush();
            }
            catch
            {
                dev.Dispose();
                ctx.Dispose();
                throw;
            }

            gizmo.SendFrame = Send;
            thread = new Thread(ReadLoop) { IsBackground = true, Name = "bdmtoy USB" };
            thread.Start();
        }

        static IUsbDevice Find(UsbContext ctx, ushort pid)
        {
            var d = ctx.Find(x => x.VendorId == VID && x.ProductId == pid);
            if (d != null && d.TryOpen())
                return d;
            d?.Dispose();
            return null;
        }

        static IUsbDevice WaitFor(UsbContext ctx, ushort pid, int ms)
        {
            for (int waited = 0; waited <= ms; waited += 200)
            {
                var d = Find(ctx, pid);
                if (d != null)
                    return d;
                Thread.Sleep(200);
            }
            return null;
        }

        // Called from the core, on the operation's thread. A failed write
        // shows up as the core timing out on the answer.
        void Send(byte[] frame) => writer.Write(frame, 2000, out _);

        void ReadLoop()
        {
            var buf = new byte[4096];
            while (!stop)
            {
                var err = reader.Read(buf, 100, out int n);
                if (n > 0)
                    gizmo.Receive(buf, n);
                if (err != Error.Success && err != Error.Timeout)
                    break; // unplugged; the core times out on its own
            }
        }

        public void Dispose()
        {
            stop = true;
            thread.Join();
            gizmo.SendFrame = null;
            try { dev.ReleaseInterface(0); } catch (UsbException) { }
            try { dev.ReleaseInterface(1); } catch (UsbException) { }
            dev.Dispose();
            ctx.Dispose();
        }

        /////////////////////////////////////////////////////////////
        // Firmware update over the adapter's USB DFU bootloader (2.0+),
        // the same steps as host/shared/toy_update.c

        const byte DFU_DETACH = 0, DFU_DNLOAD = 1, DFU_GETSTATUS = 3, DFU_CLRSTATUS = 4, DFU_ABORT = 6;
        const byte DFU_IDLE = 2, DFU_DNLOAD_IDLE = 5, DFU_ERROR = 10;
        const int DFU_BLOCK = 1024; // the bootloader's wTransferSize: one flash page

        public static bool Update(noNameGizmo gizmo, byte[] image, Action<string> say, Action<int> progress)
        {
            if (image.Length < 8 || image.Length > 0xC000)
            {
                say("That is not an app image (" + image.Length + " bytes): use firmware/bin/firmware.bin");
                return false;
            }

            using var ctx = new UsbContext();
            var boot = Find(ctx, PID_BOOT);
            if (boot == null)
            {
                if (!EnterBootloader(gizmo, say))
                    return false;
                boot = WaitFor(ctx, PID_BOOT, 5000);
                if (boot == null)
                {
                    say("The bootloader (ffff:0108) did not show up");
                    return false;
                }
            }

            try
            {
                boot.ClaimInterface(0);
                if (!DfuIdle(boot))
                {
                    say("The bootloader does not respond");
                    return false;
                }

                say("Bootloader ready, writing " + image.Length + " bytes");
                for (int off = 0; off < image.Length; off += DFU_BLOCK)
                {
                    var block = image.AsSpan(off, Math.Min(DFU_BLOCK, image.Length - off)).ToArray();
                    if (!DfuDownload(boot, off / DFU_BLOCK, block, say))
                    {
                        say("Writing the block at 0x" + off.ToString("X5") + " failed");
                        return false;
                    }
                    progress((off + block.Length) * 100 / image.Length);
                }

                // The empty block ends the download: the bootloader writes the
                // vector page (manifest). DETACH then starts the new app; a
                // USB reset would too, but WinUSB cannot reset the port.
                if (!DfuDownload(boot, (image.Length + DFU_BLOCK - 1) / DFU_BLOCK, null, say))
                {
                    say("Finishing the download failed");
                    return false;
                }
                try { boot.ControlTransfer(new UsbSetupPacket(0x21, DFU_DETACH, 1000, 0, 0)); } catch (UsbException) { }
                try { boot.ReleaseInterface(0); } catch (UsbException) { }
                try { boot.ResetDevice(); } catch (UsbException) { }
            }
            catch (UsbException e)
            {
                say("USB error: " + e.Message);
                return false;
            }
            finally
            {
                boot.Dispose();
            }

            // Back in the app: say what runs now
            using (var app = WaitFor(ctx, PID_APP, 8000))
            {
                if (app == null)
                {
                    say("Firmware written, but the adapter did not come back");
                    return false;
                }
            }
            try
            {
                using (new UsbLink(gizmo))
                {
                    int v = gizmo.FirmwareVersion();
                    if (v <= 0)
                    {
                        say("Firmware written, but the adapter does not answer");
                        return false;
                    }
                    say("Adapter firmware v" + (v >> 8) + "." + (v & 0xFF) + " running");
                }
            }
            catch (Exception e)
            {
                say(e.Message);
                return false;
            }
            return true;
        }

        static bool EnterBootloader(noNameGizmo gizmo, Action<string> say)
        {
            try
            {
                using (new UsbLink(gizmo))
                {
                    int v = gizmo.FirmwareVersion();
                    if (v < TOY_BOOT_FW)
                    {
                        say("The adapter firmware has no USB bootloader (it needs 2.0 or later): flash bdmtoy-full.hex over SWD once, see firmware/README.md");
                        return false;
                    }
                    say("Adapter firmware v" + (v >> 8) + "." + (v & 0xFF) + ", rebooting it into the bootloader");
                    if (!gizmo.EnterBootloader())
                    {
                        say("The adapter did not take the bootloader command");
                        return false;
                    }
                }
                return true;
            }
            catch (Exception e)
            {
                say(e.Message);
                return false;
            }
        }

        // status, poll timeout (ms), state
        static (byte, int, byte) DfuStatus(IUsbDevice d)
        {
            var b = new byte[6];
            if (d.ControlTransfer(new UsbSetupPacket(0xA1, DFU_GETSTATUS, 0, 0, b.Length), b, 0, b.Length) != b.Length)
                throw new UsbException("Short DFU status");
            return (b[0], b[1] | b[2] << 8 | b[3] << 16, b[4]);
        }

        // Get the bootloader to dfuIDLE: clear an error or abort a broken-off download
        static bool DfuIdle(IUsbDevice d)
        {
            for (int i = 0; i < 3; i++)
            {
                var (_, _, state) = DfuStatus(d);
                if (state == DFU_IDLE)
                    return true;
                d.ControlTransfer(new UsbSetupPacket(0x21, state == DFU_ERROR ? DFU_CLRSTATUS : DFU_ABORT, 0, 0, 0));
            }
            return false;
        }

        // One block (null: the end of the download), then poll until it is dealt with
        static bool DfuDownload(IUsbDevice d, int block, byte[] data, Action<string> say)
        {
            int len = data?.Length ?? 0;
            if (d.ControlTransfer(new UsbSetupPacket(0x21, DFU_DNLOAD, block, 0, len), data, 0, len) != len)
                return false;

            byte want = len > 0 ? DFU_DNLOAD_IDLE : DFU_IDLE;
            for (int tries = 0; tries < 500; tries++)
            {
                var (status, poll, state) = DfuStatus(d);
                if (status != 0 || state == DFU_ERROR)
                {
                    say("The bootloader reports DFU status " + status);
                    return false;
                }
                if (state == want)
                    return true;
                Thread.Sleep(Math.Max(poll, 5));
            }
            return false;
        }
    }
}
