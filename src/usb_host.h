#ifndef _LIB_USB_HOST_H_
#define _LIB_USB_HOST_H_

#include <stdint.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>
#include <condition_variable>
#include <queue>
#include <map>

#include "threading.h"

//predeclarations
struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

class UsbDevice;
typedef std::shared_ptr<UsbDevice> UsbDevice_sptr_t;
typedef std::weak_ptr<UsbDevice> UsbDevice_wptr_t;

class UsbTransfer : public std::enable_shared_from_this<UsbTransfer>
{
protected:
    UsbTransfer(const UsbDevice_sptr_t& device);
public:
    /**
     * Makes a new shared transfer object
     * @param device The shared device object to use for this transfer
     * @return A shared UsbTransfer object is returned
     */
    static std::shared_ptr<UsbTransfer> makeShared(const UsbDevice_sptr_t& device);
    virtual ~UsbTransfer();
private:
    UsbDevice_wptr_t mUsbDevice;
};
typedef std::shared_ptr<UsbTransfer> UsbTransfer_sptr_t;

struct UsbDeviceId 
{
    uint16_t vendor;
    uint16_t product;
    UsbDeviceId(uint16_t v = 0, uint16_t p = 0) : vendor(v), product(p) {}
    bool operator<(const UsbDeviceId& rhs) const { return (uint32_t(vendor) << 16 | uint32_t(product)) < (uint32_t(rhs.vendor) << 16 | uint32_t(rhs.product)); }
};

class UsbDevice : public std::enable_shared_from_this<UsbDevice>
{
protected:
    UsbDevice(libusb_device* device, const UsbDeviceId& id);
public:
    /**
    * Makes a new shared UsbDevice object
    * @return A shared UsbDevice object is returned
    */
    static std::shared_ptr<UsbDevice> makeShared(libusb_device* device, const UsbDeviceId& id);
    virtual ~UsbDevice();
    /**
     * Returns a structure that identifies the device by vendor and product ids
     */
    const UsbDeviceId& id() const noexcept;
    /**
     * Returns a pointer to the native libusb_device
     */
    libusb_device* native() const noexcept;
    /**
     * Returns a pointer to the native libusb_device_handle
     */
    libusb_device_handle* native_handle() const noexcept;
    /**
    * Returns the libusb error of the last operation
    * @return Zero is returned if there was no error otherwise a non-negative integer, see LIBUSB_<ERROR>s in libusb docs
    */
    int32_t lastLibUsbError() const noexcept;
    /**
     * Open the device to allow performing I/O operations
     *
     * !!! You MUST activate a configuration and claim the interface you wish to use 
     * before you can perform I/O on any of its endpoints !!!
     *
     * @param config_number The number of the configuration you wish to activate, -1 will put the device in unconfigured state
     * @param interface_number The number of the interface you wish to claim, -1 will claim no interface at all
     *
     * @return True is returned on success, otherwise false and lastLibUsbError() may return a propriate error
     */
    bool open(int32_t config_number = -1, int32_t interface_number = -1);
    /**
     * Close an open device
     */
    void close();
    /**
     * Perform a USB port reset to reinitialize a device. 
     * The system will attempt to restore the previous configuration 
     * and alternate settings after the reset has completed.
     *
     * If the reset fails, the descriptors change, or the previous state cannot be restored, 
     * the device will appear to be disconnected and reconnected!
     *
     * @return True is returned on success, otherwise false is returned and the UsbDevice object is no longer valid
     */
    bool resetPort();
    /**
     * Clear the halt/stall condition for an endpoint. 
     * Endpoints with halt status are unable to receive or transmit data until the halt condition is stalled.
     * You SHOULD cancel all pending transfers before attempting to clear the halt condition.
     * @return True is returned on success, otherwise false and lastLibUsbError() may return a propriate error
     */
    bool clearHalt(int32_t endpoint_number);
    /**
     * Tells if the device is valid to use
     * @return True is returned on success, otherwise false
     */
    bool isValid() const;
    /**
     * Returns a new UsbTranser object for I/O operations
     * @return On success a shared UsbTransfer object is returned, otherwise nullptr
     */
    UsbTransfer_sptr_t newTransfer();
private:
    UsbDeviceId             mId;
    libusb_device* const    mLibUsbDeviceContext;
    libusb_device_handle*   mLibUsbDeviceHandle;
    std::atomic_int32_t     mLastLibUsbError;
    mutable std::mutex      mHandleMutex;
    int                     mInterfaceNumber;
    std::atomic_bool        mIsValid;
};

class UsbHost
{
public:
    /**
     * Represents a USB host controller that can handle several devices
     * !WARNING! If hotplug is not supported on your platform then lastLibUsbError() may return LIBUSB_ERROR_NOT_SUPPORTED after
     * initialization.
     *
     * @param plugged_in_cb a callback to get a device that has been plugged in recenlty if hot plug is supported
     * @param verbose If set true all libusb warnings and errors sent to stderr
     * @param debug If set true all libusb debug information sent to stderr
     */
    UsbHost(const std::function<void(const UsbDevice_sptr_t&)>& plugged_in_cb = nullptr, bool verbose = false, bool debug = false);
    virtual ~UsbHost();
    /**
     * Returns a pointer to the native libusb_context
     */
    libusb_context* native() const noexcept;
    /**
     * Returns the libusb error of the last operation
     * @return Zero is returned if there was no error otherwise a non-negative integer, see LIBUSB_<ERROR>s in libusb docs
     */
    int32_t lastLibUsbError() const noexcept;
    /**
     * Returns a device object identified by the given vendor id and product id
     * @return A valid shared UsbDevice object is returned if exists such a device, otherwise nullptr
     */
    UsbDevice_sptr_t getDevice(uint16_t vendor_id, uint16_t product_id) const;
    /**
     * This function is used for hotplug, should not be called directly
     */
    int32_t registerLibUsbDevice(libusb_device* device);
    /**
     * This function is used for hotplug, should not be called directly
     */
    int32_t unregisterLibUsbDevice(libusb_device* device);
private:
    std::vector<UsbDeviceId> discoverDevicesIds();
    void discoverDevices();
    void closeDevices();    
    //members
    libusb_context*                              mLibUsbContext;
    int                                          mLibUsbHotPlugCbHandle;
    std::atomic_int32_t                          mLastLibUsbError;
    mutable std::mutex                           mHotPlugMutex;
    std::map<UsbDeviceId, UsbDevice_sptr_t>      mDevices;
    threading::Worker                            mWorker;
    std::function<void(const UsbDevice_sptr_t&)> mPluggedInCallback;
};

#endif
