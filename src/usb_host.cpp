#include "usb_host.h"
#include "libusb-1.0/libusb.h"

#include <optional>

static int LIBUSB_CALL libusbHotPlugCallback(libusb_context* ctx, libusb_device* device, libusb_hotplug_event event, void* user_data)
{
    if (user_data)
    {
        UsbHost* host = reinterpret_cast<UsbHost*>(user_data);
        switch (event)
        {
        case libusb_hotplug_event::LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
        {
            host->registerLibUsbDevice(device);
        } break;
        case libusb_hotplug_event::LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
        {
            host->unregisterLibUsbDevice(device);
        } break;
        default: break;
        }
        return 0;
    }
    return 1;//returning 1 will deregister this callback if user_data was not given
}

static std::optional<UsbDeviceId> createUsbDeviceId(libusb_device* device) 
{

    libusb_device_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    if (libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS)
    {
        return UsbDeviceId(descriptor.idVendor, descriptor.idProduct);
    }
    return std::nullopt;
}

//class UsbTransfer
UsbTransfer::UsbTransfer(const UsbDevice_sptr_t& device) 
    : mUsbDevice(device)
{

}

std::shared_ptr<UsbTransfer> UsbTransfer::makeShared(const UsbDevice_sptr_t& device)
{
    return std::shared_ptr<UsbTransfer>(new UsbTransfer(device));
}

UsbTransfer::~UsbTransfer() 
{

}

//class UsbDevice
UsbDevice::UsbDevice(libusb_device* device, const UsbDeviceId& id)
    : mId(id)
    , mLibUsbDeviceContext(device)
    , mLibUsbDeviceHandle(nullptr)
    , mLastLibUsbError(0)
    , mHandleMutex()
    , mInterfaceNumber(-1)
    , mIsValid(true)
{
}

std::shared_ptr<UsbDevice> UsbDevice::makeShared(libusb_device* device, const UsbDeviceId& id)
{
    return std::shared_ptr<UsbDevice>(new UsbDevice(device, id));
}

UsbDevice::~UsbDevice() 
{
    close();
}

const UsbDeviceId& UsbDevice::id() const noexcept { return mId; }

libusb_device* UsbDevice::native() const noexcept { return mLibUsbDeviceContext; }

libusb_device_handle* UsbDevice::native_handle() const noexcept 
{ 
    std::lock_guard guard(mHandleMutex);
    return mLibUsbDeviceHandle; 
}

int32_t UsbDevice::lastLibUsbError() const noexcept { return mLastLibUsbError.load();  }

bool UsbDevice::open(int32_t config_number, int32_t interface_number)
{
    std::lock_guard guard(mHandleMutex);
    if (mLibUsbDeviceContext && (mLibUsbDeviceHandle == nullptr))
    {
        mLastLibUsbError.store(libusb_open(mLibUsbDeviceContext, &mLibUsbDeviceHandle));
        if (mLastLibUsbError.load() != LIBUSB_SUCCESS) { mLibUsbDeviceHandle = nullptr; }
    }

    if ((config_number >= 0) && (interface_number >= 0))
    {
        if (mLibUsbDeviceHandle)
        {
            if (mInterfaceNumber >= 0)
            {
                libusb_release_interface(mLibUsbDeviceHandle, mInterfaceNumber);//retval is irrelevant
            }
            
            int res = libusb_set_configuration(mLibUsbDeviceHandle, config_number);
            if (res == LIBUSB_SUCCESS) 
            {
                res = libusb_claim_interface(mLibUsbDeviceHandle, interface_number);
                if (res == LIBUSB_SUCCESS) { mInterfaceNumber = interface_number; }
            }
            mLastLibUsbError.store(res);
        }
    }

    return false;
}

void UsbDevice::close()
{
    std::lock_guard guard(mHandleMutex);
    if (mLibUsbDeviceHandle)
    {
        if (mInterfaceNumber >= 0)
        {
            libusb_release_interface(mLibUsbDeviceHandle, mInterfaceNumber);//retval is irrelevant
            mInterfaceNumber = -1;
        }
        libusb_close(mLibUsbDeviceHandle);
    }
}

bool UsbDevice::resetPort()
{
    std::unique_lock locker(mHandleMutex);
    if (mLibUsbDeviceHandle)
    {
        int32_t res = libusb_reset_device(mLibUsbDeviceHandle);
        if (res != LIBUSB_SUCCESS) 
        {
            if (mInterfaceNumber >= 0)
            {
                libusb_release_interface(mLibUsbDeviceHandle, mInterfaceNumber);//retval is irrelevant
                mInterfaceNumber = -1;
            }
            libusb_close(mLibUsbDeviceHandle);
            mIsValid.store(false);
            return false;
        }
        return true;
    }
    return true;
}

bool UsbDevice::clearHalt(int32_t endpoint_number)
{
    std::unique_lock locker(mHandleMutex);
    if (mLibUsbDeviceHandle)
    {
        int32_t res = libusb_clear_halt(mLibUsbDeviceHandle, endpoint_number);
        if (res != LIBUSB_SUCCESS)
        {
            mLastLibUsbError.store(res);
            return false;
        }
        return true;
    }
    return true;
}

bool UsbDevice::isValid() const { return mIsValid.load();  }

UsbTransfer_sptr_t UsbDevice::newTransfer()
{
    return UsbTransfer::makeShared(shared_from_this());
}

//class UsbHost
UsbHost::UsbHost(const std::function<void(const UsbDevice_sptr_t&)>& plugged_in_cb, bool verbose, bool debug)
    : mLibUsbContext(nullptr)
    , mLibUsbHotPlugCbHandle(-1)
    , mLastLibUsbError(0)
    , mHotPlugMutex()
    , mDevices()
    , mWorker()
    , mPluggedInCallback(plugged_in_cb)
{
    mLastLibUsbError.store(libusb_init(&mLibUsbContext));
    if (mLastLibUsbError.load() != LIBUSB_SUCCESS)
    { mLibUsbContext = nullptr; }
    else if(verbose || debug)
    {
        auto log_level = debug ? libusb_log_level::LIBUSB_LOG_LEVEL_DEBUG : libusb_log_level::LIBUSB_LOG_LEVEL_WARNING;
        mLastLibUsbError.store(libusb_set_option(mLibUsbContext, LIBUSB_OPTION_LOG_LEVEL, log_level));
    }

    if (mLibUsbContext && (mLastLibUsbError.load() == LIBUSB_SUCCESS))
    {
        if (mPluggedInCallback) { mWorker.start(true); }
        if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
        {
            libusb_hotplug_register_callback(mLibUsbContext
                , (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
                , libusb_hotplug_flag::LIBUSB_HOTPLUG_ENUMERATE
                , LIBUSB_HOTPLUG_MATCH_ANY /*vendor_id*/
                , LIBUSB_HOTPLUG_MATCH_ANY /*product_id*/
                , LIBUSB_HOTPLUG_MATCH_ANY /*dev_class*/
                , libusbHotPlugCallback
                , this
                , &mLibUsbHotPlugCbHandle);
        }
        else
        {
            discoverDevices();
            mLastLibUsbError.store(LIBUSB_ERROR_NOT_SUPPORTED);
        }
    }
}

UsbHost::~UsbHost()
{
    if (mLibUsbContext)
    {
        libusb_hotplug_deregister_callback(mLibUsbContext, mLibUsbHotPlugCbHandle);
        mWorker.stop();
        closeDevices();
        libusb_exit(mLibUsbContext);
    }
}

libusb_context* UsbHost::native() const noexcept { return mLibUsbContext; }

int32_t UsbHost::lastLibUsbError() const noexcept
{
    return mLastLibUsbError.load();
}

UsbDevice_sptr_t UsbHost::getDevice(uint16_t vendor_id, uint16_t product_id) const
{
    std::lock_guard<std::mutex> hotplug_guard(mHotPlugMutex);
    UsbDeviceId id(vendor_id, product_id);
    const auto it = mDevices.find(id);
    if (it != mDevices.cend()) 
    {
        return it->second;
    }
    return nullptr;
}

int32_t UsbHost::registerLibUsbDevice(libusb_device* device) 
{
    std::lock_guard<std::mutex> hotplug_guard(mHotPlugMutex);
    if (auto opt = createUsbDeviceId(device))
    {
        auto& id = opt.value();
        auto it = mDevices.find(id);
        if (it == mDevices.end())
        {
            auto device_obj = UsbDevice::makeShared(device, id);
            mDevices.emplace(id, device_obj);
            if (mPluggedInCallback) 
            {
                auto plugged_in_cb = mPluggedInCallback;
                mWorker.push([plugged_in_cb, device_obj]() { plugged_in_cb(device_obj); });
            }
        }
    }
    return 0;
}

int32_t UsbHost::unregisterLibUsbDevice(libusb_device* device)
{
    std::lock_guard<std::mutex> hotplug_guard(mHotPlugMutex);
    if (auto opt = createUsbDeviceId(device))
    {
        mDevices.erase(opt.value());
    }
    return 0;
}

std::vector<UsbDeviceId> UsbHost::discoverDevicesIds()
{
    std::vector<UsbDeviceId> result;
    if (mLibUsbContext) 
    {
        libusb_device** device_list = nullptr;
        auto device_number = libusb_get_device_list(mLibUsbContext, &device_list);
        if (device_number > 0) 
        {
            for (int i = 0; i < device_number; ++i)
            {
                if (auto opt = createUsbDeviceId(device_list[i]))
                {
                    result.emplace_back(opt.value());
                }
            }
        }
        libusb_free_device_list(device_list, 0);
    }
    return result;
}

void UsbHost::discoverDevices()
{
    if (mLibUsbContext)
    {
        libusb_device** device_list = nullptr;
        auto device_number = libusb_get_device_list(mLibUsbContext, &device_list);
        if (device_number > 0)
        {
            for (int i = 0; i < device_number; ++i)
            {
                registerLibUsbDevice(device_list[i]);
            }
        }
        libusb_free_device_list(device_list, 0);
    }
}

void UsbHost::closeDevices()
{
    std::lock_guard<std::mutex> hotplug_guard(mHotPlugMutex);
    for (auto& [id, device] : mDevices) 
    {
        if (device) { device->close(); }
    }
}