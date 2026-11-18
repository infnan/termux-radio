#include <libusb.h>
#include <array>
#include "usbuart.hpp"

namespace usbuart {

class cp210x : public generic {
public:
	cp210x(libusb_device_handle* handle, uint8_t number, const interface& endpoints)
		: generic(handle, endpoints, number) {}

	void setbaudrate(baudrate_t baud) const throw(error_t) {
		std::array<unsigned char, 4> data{{
			static_cast<unsigned char>(baud), static_cast<unsigned char>(baud >> 8),
			static_cast<unsigned char>(baud >> 16), static_cast<unsigned char>(baud >> 24)}};
		transfer(0x1e, 0, data.data(), data.size());
	}

	void setup(const eia_tia_232_info& info) const throw(error_t) {
		if (info.databits != 8 || info.parity != none || (info.stopbits != one && info.stopbits != two) ||
			info.flowcontrol != none_) throw error_t::not_supported;
		transfer(0x00, 1, nullptr, 0);
		setbaudrate(info.baudrate);
		transfer(0x03, 0x0800 | static_cast<uint16_t>(info.stopbits), nullptr, 0);
		std::array<unsigned char, 16> flow{};
		transfer(0x13, 0, flow.data(), flow.size());
		transfer(0x07, 0x0300, nullptr, 0);
	}

private:
	void transfer(uint8_t request, uint16_t value, unsigned char* data,
		uint16_t length) const throw(error_t) {
		int result = libusb_control_transfer(dev, 0x41, request, value, ifcnum,
			data, length, timeout);
		if (result != length) throw error_t::control_error;
	}

	static class factory : public driver::factory {
		driver* create(libusb_device_handle* handle, uint8_t number) const throw(error_t) {
			device_id id = devid(handle);
			if (id.vid != 0x10c4 || (id.pid != 0xea60 && id.pid != 0xea70 &&
				id.pid != 0xea71)) return nullptr;
			libusb_config_descriptor* config = nullptr;
			if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config) < 0)
				throw error_t::usb_error;
			interface endpoints{};
			bool found = false;
			for (int group = 0; group < config->bNumInterfaces; ++group) {
				for (int alternate = 0; alternate < config->interface[group].num_altsetting; ++alternate) {
					const auto& descriptor = config->interface[group].altsetting[alternate];
					if (descriptor.bInterfaceNumber != number || descriptor.bAlternateSetting != 0) continue;
					for (int index = 0; index < descriptor.bNumEndpoints; ++index) {
						const auto& endpoint = descriptor.endpoint[index];
						if ((endpoint.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
						if (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
							endpoints.ep_bulk_in = endpoint.bEndpointAddress;
							endpoints.chunk_size = endpoint.wMaxPacketSize;
						} else endpoints.ep_bulk_out = endpoint.bEndpointAddress;
					}
					found = endpoints.ep_bulk_in && endpoints.ep_bulk_out && endpoints.chunk_size;
				}
			}
			libusb_free_config_descriptor(config);
			if (!found) throw error_t::no_interface;
			return new cp210x(handle, number, endpoints);
		}
	} factory_;
};

cp210x::factory cp210x::factory_;

}
