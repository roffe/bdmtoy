#include "worker.h"
#include "main.h"

#include <chrono>
#include <libusb.h>
#include "../../../shared/toy_update.h"
#include "../../../core/core.h"
#include "../../../../shared/enums.h"
#include "../../../../shared/cmddesc.h"

// static struct libusb_transfer *transfer_out = nullptr;
static struct libusb_transfer *transfer_in = nullptr;
static libusb_device_handle *handle = nullptr;
static libusb_context *ctx = nullptr;
static bool claimed_interfaces[2] = {false, false};

#define NUM_INTERFACES (2)

// static std::mutex usbsmutex;

static glue glue_;
static Worker *wptr;

// Local buffer for receiving USB data
static uint8_t in_buffer[ADAPTER_BUFzOUT];

// Async. USB thread has to know when it's time to quit
static volatile bool run_USBThread;

// transfer_in stays submitted (cb_in resubmits it) until its cancellation
// completes. The event thread must drain that before we free/exit libusb.
static volatile bool rx_in_flight;

// Spawned threads has to know file name and which target to perform actions on
static int index_;
static QString fname_;

// The adapter probe: one try at opening it, and none of the per-operation chatter
static volatile bool probing_;

// Constructors and destructors
Worker::Worker() {}
Worker::~Worker() {}

////////////////////////////////////////////////////
/// Err.. Don't ask!
// Only reason for conversion is to preserve the message when it's transferred between threads
void Worker::WrkMsg_push(QString msg)
{
    glue_.CastMessage(msg.toUtf8());
}
void Worker::WrkProg_push(uint prog)
{
    glue_.CastProgress(prog);
}

void Worker::WrkMsg_inter(const char *msg)
{
    emit WrkMsg_emit(QString::fromStdString(msg));
}
void Worker::WrkProg_inter(uint prog)
{
    emit WrkProg_emit(prog);
}

static void MessagePoint(const char *msg)
{
    wptr->WrkMsg_inter(msg);
}
static void ProgressPoint(uint prog)
{
    wptr->WrkProg_inter(prog);
}

////////////////////////////////////////////////////
/// USB functions
static void cb_in(struct libusb_transfer *transfer)
{
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED)
    {
        rx_in_flight = false; // Cancelled or errored: no longer submitted
        return;
    }

    core_HandleRecData(transfer->buffer, static_cast<uint32_t>(transfer->actual_length));

    if (run_USBThread)
        libusb_submit_transfer(transfer);
    else
        rx_in_flight = false;
}

static void cb_out(struct libusb_transfer *transfer)
{
    libusb_free_transfer(transfer);
    // usbsmutex.unlock();
    // qDebug() << "Mutex unlocked";
}

static void USBProcess()
{
    // qDebug() << "Async USB thread ID:" << QThread::currentThreadId();
    run_USBThread = true;
    // Keep pumping until the RX transfer's cancellation has been drained, so we
    // never free/exit libusb with a transfer still in flight.
    while (run_USBThread || rx_in_flight)
    {
        libusb_handle_events_completed(ctx, nullptr);
        // libusb_handle_events(ctx);
    }
}

static void StopUSBThread(std::thread &usbThread)
{
    run_USBThread = false;

    // Cancel in-flight receive transfer to wake libusb event handling.
    if (transfer_in)
        libusb_cancel_transfer(transfer_in);

    if (usbThread.joinable())
        usbThread.join();
}

// This is.. let's say a particularly stupid thing to do ;)
static void usb_SendArr(void *ptr, uint32_t noBytes)
{
    // qDebug() << "Thread ID trying to send:" << QThread::currentThreadId();
    // Our callback will unlock this
    // usbsmutex.lock();
    // qDebug() << "Mutex locked";

    libusb_transfer *transfer_out = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(
        transfer_out,                     // Transfer
        handle,                           // Device handle
        LIBUSB_ENDPOINT_OUT | 3,          // Endpoint
        reinterpret_cast<uint8_t *>(ptr), // Send buffer
        static_cast<int>(noBytes),        // Size out
        cb_out,                           // Callback
        nullptr,                          // user data to pass to callback function
        8000);                            // Timeout

    // Queue it and forget it
    libusb_submit_transfer(transfer_out);
}

static bool usb_open()
{
    int res = libusb_init(&ctx);
    if (res != 0)
    {
        core_castText("Could not initialize libusb: %s", libusb_error_name(res));
        return false;
    }

    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    // USB device can briefly disappear during re-enumeration; retry for a short window.
    for (int i = 0; i < (probing_ ? 1 : 40); i++)
    {
        handle = libusb_open_device_with_vid_pid(ctx, 0xFFFF, 0x0107);
        if (handle)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(125));
    }

    if (!handle)
    {
        libusb_device **list = nullptr;
        ssize_t count = libusb_get_device_list(ctx, &list);
        bool foundDevice = false;
        bool permissionDenied = false;

        if (count >= 0)
        {
            for (ssize_t i = 0; i < count; i++)
            {
                libusb_device_descriptor desc;
                if (libusb_get_device_descriptor(list[i], &desc) != 0)
                    continue;

                if (desc.idVendor == 0xFFFF && desc.idProduct == 0x0107)
                {
                    foundDevice = true;
                    libusb_device_handle *probe = nullptr;
                    int openRes = libusb_open(list[i], &probe);
                    if (openRes == LIBUSB_ERROR_ACCESS)
                        permissionDenied = true;
                    else if (openRes == 0 && probe)
                        libusb_close(probe);
                    break;
                }
            }
            libusb_free_device_list(list, 1);
        }

        if (permissionDenied)
            core_castText("Unable to open device: permission denied (set a udev rule for VID:PID FFFF:0107 or run as root)");
        else if (foundDevice)
            core_castText("Unable to open device: busy or blocked by kernel driver");
        else if (probing_)
            core_castText("No adapter found");
        else
            core_castText("Unable to open device: device FFFF:0107 not found (waited 5s)");

        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    for (int i = 0; i < NUM_INTERFACES; i++)
        claimed_interfaces[i] = false;

    res = libusb_set_auto_detach_kernel_driver(handle, 1);
    if (res != 0 && res != LIBUSB_ERROR_NOT_SUPPORTED)
        core_castText("Warning: auto-detach unavailable: %s", libusb_error_name(res));

    for (int i = 0; i < NUM_INTERFACES; i++)
    {
        res = libusb_kernel_driver_active(handle, i);
        if (res == 1)
        {
            int detRes = libusb_detach_kernel_driver(handle, i);
            if (detRes != 0 && detRes != LIBUSB_ERROR_NOT_FOUND && detRes != LIBUSB_ERROR_NOT_SUPPORTED)
            {
                core_castText("Error detaching kernel driver on interface %d: %s", i, libusb_error_name(detRes));
                libusb_close(handle);
                handle = nullptr;
                libusb_exit(ctx);
                ctx = nullptr;
                return false;
            }
        }

        // Interface 1 is only there on the CDC layout of firmware before 2.2
        res = libusb_claim_interface(handle, i);
        if (res != 0 && i > 0)
            continue;
        if (res != 0)
        {
            core_castText("Error claiming interface %d: %s", i, libusb_error_name(res));

            for (int j = 0; j < i; j++)
            {
                if (claimed_interfaces[j])
                {
                    libusb_release_interface(handle, j);
                    claimed_interfaces[j] = false;
                }
            }

            libusb_close(handle);
            handle = nullptr;
            libusb_exit(ctx);
            ctx = nullptr;
            return false;
        }

        claimed_interfaces[i] = true;
    }

    transfer_in = libusb_alloc_transfer(0);

    libusb_fill_bulk_transfer(
        transfer_in,            // Transfer
        handle,                 // Device handle
        LIBUSB_ENDPOINT_IN | 1, // Endpoint
        in_buffer,              // Receive buffer
        ADAPTER_BUFzOUT,        // Size in
        cb_in,                  // Callback
        nullptr,                // user data to pass to callback function
        0);                     // Timeout

    res = libusb_submit_transfer(transfer_in);
    if (res != 0)
    {
        core_castText("Error submitting receive transfer: %s", libusb_error_name(res));
        libusb_free_transfer(transfer_in);
        transfer_in = nullptr;

        for (int i = 0; i < NUM_INTERFACES; i++)
        {
            if (claimed_interfaces[i])
            {
                libusb_release_interface(handle, i);
                claimed_interfaces[i] = false;
            }
        }

        libusb_close(handle);
        handle = nullptr;
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    rx_in_flight = true;
    return true;
}

void Worker::DeInitUSB()
{
    core_InstallSendArray(nullptr);

    if (transfer_in)
    {
        libusb_free_transfer(transfer_in);
        transfer_in = nullptr;
    }

    if (handle)
    {
        for (int i = 0; i < NUM_INTERFACES; i++)
        {
            if (claimed_interfaces[i])
            {
                libusb_release_interface(handle, i);
                claimed_interfaces[i] = false;
            }
        }

        libusb_close(handle);
        handle = nullptr;
    }

    if (ctx)
    {
        libusb_exit(ctx);
        ctx = nullptr;
    }

    if (!probing_)
        core_castText("Device detached");
}

bool Worker::InitUSB()
{
    // usbsmutex.unlock();

    if (usb_open())
    {
        if (!probing_)
            core_castText("Device attached");
        return true;
    }
    return false;
}

////////////////////////////////////////////////////
/// Glue..
static bool SaveBufferToFile(int Size)
{
    const void *bufptr = core_ReturnBufferPointer();
    if (bufptr)
    {
        QFile file(fname_);
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(reinterpret_cast<const char *>(bufptr), Size);
            file.waitForBytesWritten(10000);
            file.close();

            uint32_t checksum = 0;
            const uint8_t *ptr = reinterpret_cast<const unsigned char *>(reinterpret_cast<const char *>(bufptr));

            for (int i = 0; i < Size; i++)
                checksum += *ptr++;

            core_castText("Buffer length: %08X", Size);
            core_castText("Checksum     : %08X", checksum);
            return true;
        }
        else
            core_castText("Error: Could not open file for saving");
    }

    // No buffer pointer was given due to previous fault
    else
        core_castText(core_TranslateFault());
    return false;
}

// Call this BEFORE making other calls to core
static void InstallPointers(Worker *classptr)
{
    wptr = classptr;
    core_InstallSendArray(&usb_SendArr);
    core_InstallMessage(reinterpret_cast<void *>(&MessagePoint));
    core_InstallProgress(reinterpret_cast<void *>(&ProgressPoint));
}

////////////////////////////////////////////////////
/// Thread workers

void Worker::DumpEepromProcess()
{
    // qDebug() << "Dump thread id:" << QThread::currentThreadId();
    QElapsedTimer tim;

    InstallPointers(this);
    ProgressPoint(0);
    tim.start();

    if (!InitUSB())
    {
        emit finished();
        return;
    }

    // Spawn another thread for USB
    std::thread t1(USBProcess);

    core_DumpEEPROM(static_cast<uint>(index_));
    if (core_ReturnFaultStatus())
        core_castText("EEPROM dump failed");
    else
    {
        if (SaveBufferToFile(static_cast<int>(core_TargetSizeEEPROM(static_cast<uint>(index_)))))
            core_castText("EEPROM dump successful");
    }

    StopUSBThread(t1);
    DeInitUSB();

    long long time = tim.elapsed();
    double speed = (core_TargetSizeEEPROM(static_cast<uint>(index_)) / 1024) / (time / 1000.0);
    core_castText("Took: %u mS (%1.3f KB/S)", time, speed);
    emit finished();
}

void Worker::FlashEepromProcess()
{
    // qDebug() << "Flash id:" << QThread::currentThreadId();
    QElapsedTimer tim;
    uint32_t index = static_cast<uint32_t>(index_);

    InstallPointers(this);
    ProgressPoint(0);
    tim.start();

    QFile file(fname_);
    if (file.open(QIODevice::ReadOnly))
    {
        if (file.size() != core_TargetSizeEEPROM(index))
        {
            file.close();
            core_castText("File size does not match target");
            emit finished();
            return;
        }

        const QByteArray qarr = file.readAll();
        file.close();

        if (!InitUSB())
        {
            emit finished();
            return;
        }

        // Spawn another thread for USB
        std::thread t1(USBProcess);

        core_WriteEEPROM(static_cast<uint>(index_), const_cast<char *>(qarr.data()));
        if (core_ReturnFaultStatus())
            core_castText("EEPROM write failed");
        else
            core_castText("EEPROM write successful");

        StopUSBThread(t1);
        DeInitUSB();
    }
    else
        core_castText("Error: Could not open file for reading");

    long long time = tim.elapsed();
    double speed = (core_TargetSizeEEPROM(index) / 1024) / (time / 1000.0);
    core_castText("Took: %u mS (%1.3f KB/S)", time, speed);
    emit finished();
}

void Worker::DumpProcess()
{
    // qDebug() << "Dump thread id:" << QThread::currentThreadId();
    QElapsedTimer tim;

    InstallPointers(this);
    ProgressPoint(0);
    tim.start();

    if (!InitUSB())
    {
        emit finished();
        return;
    }

    // Spawn another thread for USB
    std::thread t1(USBProcess);

    core_DumpFLASH(static_cast<uint>(index_));
    if (core_ReturnFaultStatus())
        core_castText("Dump failed");
    else
    {
        if (SaveBufferToFile(static_cast<int>(core_TargetSizeFLASH(static_cast<uint>(index_)))))
            core_castText("Dump successful");
    }

    StopUSBThread(t1);
    DeInitUSB();

    long long time = tim.elapsed();
    double speed = (core_TargetSizeFLASH(static_cast<uint>(index_)) / 1024) / (time / 1000.0);
    core_castText("Took: %u mS (%1.3f KB/S)", time, speed);
    emit finished();
}

void Worker::FlashProcess()
{
    // qDebug() << "Flash id:" << QThread::currentThreadId();
    QElapsedTimer tim;
    uint32_t index = static_cast<uint32_t>(index_);

    InstallPointers(this);
    ProgressPoint(0);
    tim.start();

    QFile file(fname_);
    if (file.open(QIODevice::ReadOnly))
    {
        if (file.size() != core_TargetSizeFLASH(index))
        {
            file.close();
            core_castText("File size does not match target");
            emit finished();
            return;
        }

        const QByteArray qarr = file.readAll();
        file.close();

        if (!InitUSB())
        {
            emit finished();
            return;
        }

        // Spawn another thread for USB
        std::thread t1(USBProcess);

        core_FLASH(static_cast<uint>(index_), const_cast<char *>(qarr.data()));
        if (core_ReturnFaultStatus())
            core_castText("Flash failed");
        else
            core_castText("Flash successful");

        StopUSBThread(t1);
        DeInitUSB();
    }
    else
        core_castText("Error: Could not open file for reading");

    long long time = tim.elapsed();
    double speed = (core_TargetSizeFLASH(index) / 1024) / (time / 1000.0);
    core_castText("Took: %u mS (%1.3f KB/S)", time, speed);
    emit finished();
}

void Worker::WorkerDone()
{
    if (probing_)
        probing_ = false;
    else
    {
        WrkMsg_push("Thread gone *poof*");
        WrkMsg_push(" ");
    }
    // btnDumpClick() / btnFlashClick() disables input so we have to manually enable it again
    glue_.ECUIndexLogic(index_);
}

////////////////////////////////////////////////////
/// More glue...
// There's a known bug in Qt. XCB error: 3 is likely not my fault in case you see it in the logs
void FileDialog::OpenDialog()
{
    fname = QFileDialog::getOpenFileName(
        this,
        "Open File",
        QDir::homePath(),
        "Binary files (*.bin) ;; All files (*.*)");
}
void FileDialog::SaveDialog()
{
    fname = QFileDialog::getSaveFileName(
        this,
        "Save File",
        QDir::homePath(),
        "Binary files (*.bin) ;; All files (*.*)");
}

void Worker::PrepareThread(int index, QString fname, const char *slot)
{
    index_ = index;
    fname_ = fname;

    QThread *thread = new QThread;
    Worker *worker = new Worker();
    worker->moveToThread(thread);

    connect(thread, SIGNAL(started()), worker, slot);
    connect(worker, SIGNAL(WrkMsg_emit(QString)), this, SLOT(WrkMsg_push(QString)));
    connect(worker, SIGNAL(WrkProg_emit(uint)), this, SLOT(WrkProg_push(uint)));
    connect(worker, SIGNAL(finished()), thread, SLOT(quit()));
    connect(worker, SIGNAL(finished()), worker, SLOT(deleteLater()));
    connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));
    connect(thread, SIGNAL(finished()), this, SLOT(WorkerDone()));

    thread->start();
}

void Worker::StartEepromFlash(int index)
{
    FileDialog fd;
    fd.OpenDialog();

    if (fd.fname != nullptr)
        PrepareThread(index, fd.fname, SLOT(FlashEepromProcess()));
    // btnFlashClick() disables input so we have to manually enable it again
    else
        glue_.ECUIndexLogic(index);
}

void Worker::StartEepromDump(int index)
{
    FileDialog fd;
    fd.SaveDialog();

    if (fd.fname != nullptr)
        PrepareThread(index, fd.fname, SLOT(DumpEepromProcess()));
    // btnDumpClick() disables input so we have to manually enable it again
    else
        glue_.ECUIndexLogic(index);
}

void Worker::StartDump(int index)
{
    FileDialog fd;
    fd.SaveDialog();

    if (fd.fname != nullptr)
        PrepareThread(index, fd.fname, SLOT(DumpProcess()));
    // btnDumpClick() disables input so we have to manually enable it again
    else
        glue_.ECUIndexLogic(index);
}

// Check for the adapter and its firmware. The caller disables the controls;
// WorkerDone() gives them back.
void Worker::StartProbe()
{
    probing_ = true;
    PrepareThread(0, QString(), SLOT(ProbeProcess()));
}

void Worker::ProbeProcess()
{
    InstallPointers(this);

    if (InitUSB())
    {
        std::thread t1(USBProcess);
        uint16_t version = 0;

        switch (core_FirmwareVersion(&version))
        {
        case RET_OK:
            core_castText("Adapter firmware v%u.%u connected", (unsigned)(version >> 8), (unsigned)(version & 0xFF));
            break;
        case RET_NOTSUP:
            core_castText("Adapter connected, but its firmware is older than v1.0: please update it (see firmware/README.md)");
            break;
        default:
            core_castText("Adapter found, but it does not answer: replug it");
            break;
        }

        StopUSBThread(t1);
        DeInitUSB();
    }

    emit finished();
}

// Firmware update over the adapter's USB bootloader (2.0+)
void Worker::StartUpdate(int index)
{
    FileDialog fd;
    fd.OpenDialog();

    if (fd.fname != nullptr)
        PrepareThread(index, fd.fname, SLOT(UpdateProcess()));
    else
        glue_.ECUIndexLogic(index);
}

static void UpdateProgress(int percent)
{
    ProgressPoint(static_cast<uint>(percent));
}

void Worker::UpdateProcess()
{
    InstallPointers(this);
    ProgressPoint(0);

    QFile file(fname_);
    if (!file.open(QIODevice::ReadOnly))
    {
        core_castText("Error: Could not open the firmware file");
        emit finished();
        return;
    }
    const QByteArray image = file.readAll();
    file.close();

    if (toy_update(reinterpret_cast<const uint8_t *>(image.constData()), static_cast<size_t>(image.size()),
                   MessagePoint, UpdateProgress))
        core_castText("Firmware update failed");

    emit finished();
}

void Worker::StartFlash(int index)
{
    FileDialog fd;
    fd.OpenDialog();

    if (fd.fname != nullptr)
        PrepareThread(index, fd.fname, SLOT(FlashProcess()));
    // btnFlashClick() disables input so we have to manually enable it again
    else
        glue_.ECUIndexLogic(index);
}
