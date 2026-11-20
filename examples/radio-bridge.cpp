#include <libusb.h>
#include <pty.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <vector>
#include <algorithm>
#include <regex>
#include "usbuart.hpp"

static volatile sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }

static void require(bool success, const std::string& message) {
	if (!success) throw std::runtime_error(message);
}

static unsigned number(const std::string& text) {
	std::size_t end = 0;
	unsigned long result = std::stoul(text, &end);
	require(end == text.size() && result > 0 && result <= 4000000, "Invalid number: " + text);
	return result;
}

static speed_t speed(unsigned baud) {
	switch (baud) {
	case 1200: return B1200;
	case 2400: return B2400;
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	default: throw std::runtime_error("Unsupported baud rate: " + std::to_string(baud));
	}
}

static unsigned baudrate(speed_t value) {
	for (unsigned baud : {1200u, 2400u, 4800u, 9600u, 19200u, 38400u, 57600u, 115200u, 230400u})
		if (speed(baud) == value) return baud;
	return 0;
}

static std::string json(const std::string& text) {
	std::ostringstream result;
	result << '"';
	for (unsigned char character : text) {
		if (character == '"' || character == '\\') result << '\\' << character;
		else if (character >= 32 && character < 127) result << character;
		else {
			char escaped[7];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
			result << escaped;
		}
	}
	result << '"';
	return result.str();
}

static void report(const std::string& path, const std::string& content) {
	if (path.empty()) { std::puts(content.c_str()); return; }
	std::string temporary = path + ".tmp." + std::to_string(getpid());
	{
		std::ofstream output(temporary);
		require(bool(output), "Cannot write report: " + path);
		output << content << '\n';
		output.close();
		require(bool(output), "Cannot finish report: " + path);
	}
	require(rename(temporary.c_str(), path.c_str()) == 0, "Cannot publish report: " + path);
}

struct device {
	libusb_context* context = nullptr;
	libusb_device_handle* handle = nullptr;
	~device() {
		if (handle) libusb_close(handle);
		if (context) libusb_exit(context);
	}
	void open(int descriptor) {
		libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
		require(libusb_init(&context) == 0, "libusb initialization failed");
		int result = libusb_wrap_sys_device(context, descriptor, &handle);
		require(result == 0, std::string("Cannot use Android USB descriptor: ") + libusb_error_name(result));
	}
};

struct usb_interface {
	int number;
	std::string name;
};

static std::string usb_string(libusb_device_handle* handle, uint8_t index) {
	if (!index) return "";
	unsigned char text[256];
	int count = libusb_get_string_descriptor_ascii(handle, index, text, sizeof(text));
	return count > 0 ? std::string(reinterpret_cast<char*>(text), count) : "";
}

static std::vector<usb_interface> interfaces(libusb_device_handle* handle) {
	libusb_config_descriptor* raw = nullptr;
	require(libusb_get_active_config_descriptor(libusb_get_device(handle), &raw) == 0,
		"Cannot read USB configuration");
	std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)>
		config(raw, libusb_free_config_descriptor);
	std::vector<usb_interface> result;
	for (int group = 0; group < config->bNumInterfaces; ++group) {
		for (int alternate = 0; alternate < config->interface[group].num_altsetting; ++alternate) {
			const auto& descriptor = config->interface[group].altsetting[alternate];
			if (descriptor.bAlternateSetting) continue;
			bool input = false, output = false;
			for (int index = 0; index < descriptor.bNumEndpoints; ++index) {
				const auto& endpoint = descriptor.endpoint[index];
				if ((endpoint.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
				if (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) input = true;
				else output = true;
			}
			if (input && output) result.push_back({descriptor.bInterfaceNumber,
				usb_string(handle, descriptor.iInterface)});
		}
	}
	return result;
}

struct port {
	std::unique_ptr<usbuart::driver> driver;
	int master = -1, slave = -1;
	std::string path, link, name;
	unsigned baud = 0;
	libusb_transfer* transfer = nullptr;
	std::vector<unsigned char> buffer, pending;
	bool submitted = false, failed = false;

	~port() {
		if (transfer) libusb_free_transfer(transfer);
		if (!link.empty()) {
			char target[512];
			ssize_t count = readlink(link.c_str(), target, sizeof(target));
			if (count > 0 && std::string(target, count) == path) unlink(link.c_str());
		}
		if (master >= 0) close(master);
		if (slave >= 0) close(slave);
	}

	static void received(libusb_transfer* transfer) {
		auto& current = *static_cast<port*>(transfer->user_data);
		current.submitted = false;
		if (transfer->status == LIBUSB_TRANSFER_CANCELLED) return;
		if (transfer->status != LIBUSB_TRANSFER_COMPLETED) { current.failed = true; return; }
		usbuart::size_t offset = 0;
		current.driver->read_callback(transfer, offset);
		current.pending.insert(current.pending.end(), transfer->buffer + offset,
			transfer->buffer + transfer->actual_length);
	}

	void initialize(libusb_device_handle* handle, int number, const std::string& label,
		const std::string& directory, unsigned initial_baud) {
		name = label;
		driver.reset(usbuart::make_driver(handle, number));
		const auto stops = label.find("ft891") == 0 && label.find("-enhanced") != std::string::npos ? usbuart::two : usbuart::one;
		usbuart::eia_tia_232_info settings{initial_baud, 8, usbuart::none, stops, usbuart::none_};
		driver->setup(settings);
		baud = initial_baud;
		char terminal[256];
		require(openpty(&master, &slave, terminal, nullptr, nullptr) == 0, "Cannot create PTY");
		path = terminal;
		termios attributes{};
		require(tcgetattr(slave, &attributes) == 0, "Cannot read PTY settings");
		cfmakeraw(&attributes);
		attributes.c_cflag &= ~(CSIZE | CSTOPB | PARENB | CRTSCTS | HUPCL);
		attributes.c_cflag |= CS8 | CLOCAL | CREAD;
		if (stops == usbuart::two) attributes.c_cflag |= CSTOPB;
		cfsetispeed(&attributes, speed(baud));
		cfsetospeed(&attributes, speed(baud));
		require(tcsetattr(slave, TCSANOW, &attributes) == 0, "Cannot configure PTY");
		require(fcntl(master, F_SETFL, O_NONBLOCK) == 0, "Cannot set PTY nonblocking");
		fchmod(slave, 0600);
		link = directory + "/" + name;
		std::string temporary = link + ".tmp." + std::to_string(getpid());
		require(symlink(path.c_str(), temporary.c_str()) == 0, "Cannot create link: " + temporary);
		if (rename(temporary.c_str(), link.c_str()) != 0) {
			unlink(temporary.c_str());
			throw std::runtime_error("Cannot publish link: " + link);
		}
		buffer.resize(driver->getifc().chunk_size);
		transfer = libusb_alloc_transfer(0);
		require(transfer != nullptr, "Cannot allocate USB transfer");
		libusb_fill_bulk_transfer(transfer, handle, driver->getifc().ep_bulk_in,
			buffer.data(), buffer.size(), received, this, 0);
		std::fprintf(stderr, "%s: interface=%d %s -> %s baud=%u 8N%d\n",
			name.c_str(), number, link.c_str(), path.c_str(), baud, stops == usbuart::two ? 2 : 1);
	}

	void synchronize(bool follow) {
		if (!follow) return;
		termios attributes{};
		require(tcgetattr(slave, &attributes) == 0, "Cannot read client termios");
		unsigned requested = baudrate(cfgetospeed(&attributes));
		if (!requested || requested == baud) return;
		driver->setbaudrate(requested);
		baud = requested;
		std::fprintf(stderr, "%s: client selected baud=%u\n", name.c_str(), baud);
	}

	void process(bool follow) {
		require(!failed, "USB read failed/disconnected: " + name);
		synchronize(follow);
		if (!pending.empty()) {
			ssize_t written = write(master, pending.data(), pending.size());
			if (written > 0) pending.erase(pending.begin(), pending.begin() + written);
			else require(written < 0 && (errno == EAGAIN || errno == EINTR), "PTY write failed");
		}
		if (!submitted && pending.size() < 65536) {
			require(libusb_submit_transfer(transfer) == 0, "Cannot submit USB read");
			submitted = true;
		}
		unsigned char outgoing[64];
		ssize_t count = read(master, outgoing, sizeof(outgoing));
		if (count < 0) {
			require(errno == EAGAIN || errno == EINTR, "PTY read failed");
			return;
		}
		if (count == 0) return;
		synchronize(follow);
		int offset = 0;
		while (offset < count) {
			int sent = 0;
			int result = libusb_bulk_transfer(driver->handle(), driver->getifc().ep_bulk_out,
				outgoing + offset, count - offset, &sent, 1000);
			require(result == 0 && sent > 0, "USB write failed; command was not retried");
			offset += sent;
		}
	}
};

int main(int argc, char** argv) {
	std::string report_path, directory, selection = "auto", port_names;
	bool probe = false, follow = true;
	unsigned baud = 115200;
	int descriptor = -1, enhanced = 0, standard = 1;
	try {
		if (const char* configured = getenv("USBUART_BAUD")) baud = number(configured);
		if (const char* mode = getenv("USBUART_BAUD_MODE")) {
			require(std::strcmp(mode, "follow") == 0 || std::strcmp(mode, "fixed") == 0,
				"USBUART_BAUD_MODE must be follow or fixed");
			follow = std::strcmp(mode, "fixed") != 0;
		}
		for (int argument = 1; argument < argc; ++argument) {
			std::string option = argv[argument];
			if (option == "--help") {
				std::puts("termux-usb -r -e 'radio-bridge --link-dir DIR [options]' DEVICE\n"
					"  --probe --report FILE     USB descriptors only, no serial commands\n"
					"  --radio MODEL            explicit model label (default generic)\n"
					"  --port-names NAMES       comma-separated names in interface order\n"
					"  --baud N                 initial baud (USBUART_BAUD, default 115200)\n"
					"  --baud-mode follow|fixed follow slave termios (default follow)\n"
					"  --enhanced N --standard N override CP2105 interface numbers\n"
					"  --report FILE            atomic JSON readiness report\n"
					"Model labels are supplied explicitly; USB-UART chips use generic names by default.\n"
					"FT-891 Enhanced: 8N2; other ports: 8N1; no flow control.\n"
					"RTS/DTR client ioctls are not forwarded. Use CAT PTT.");
				return 0;
			}
			if (option == "--probe") { probe = true; continue; }
			if (option.compare(0, 2, "--") == 0) {
				require(argument + 1 < argc, "Missing value for " + option);
				std::string value = argv[++argument];
				if (option == "--report") report_path = value;
				else if (option == "--link-dir") directory = value;
				else if (option == "--radio") selection = value;
				else if (option == "--port-names") port_names = value;
				else if (option == "--baud") baud = number(value);
				else if (option == "--baud-mode") {
					require(value == "follow" || value == "fixed", "Invalid baud mode");
					follow = value == "follow";
				} else if (option == "--enhanced" || option == "--standard") {
					std::size_t end = 0;
					int index = std::stoi(value, &end);
					require(end == value.size() && index >= 0 && index <= 255, "Invalid interface");
					if (option == "--enhanced") enhanced = index; else standard = index;
				} else throw std::runtime_error("Unknown option: " + option);
			} else {
				std::size_t end = 0;
				require(descriptor == -1, "Unexpected argument: " + option);
				descriptor = std::stoi(option, &end);
				require(end == option.size() && descriptor >= 0, "Invalid USB descriptor");
			}
		}
		require(descriptor >= 0, "USB descriptor missing; run via termux-usb -e");
		speed(baud);
		require(std::regex_match(selection, std::regex("[a-z0-9-]+")), "Invalid model label");
		device usb;
		usb.open(descriptor);
		libusb_device_descriptor description{};
		require(libusb_get_device_descriptor(libusb_get_device(usb.handle), &description) == 0,
			"Cannot read USB device descriptor");
		auto available = interfaces(usb.handle);
		unsigned char part = 0;
		if (description.idVendor == 0x10c4)
			libusb_control_transfer(usb.handle, 0xc0, 0xff, 0x370b, 0, &part, 1, 1000);
		std::string model = selection == "auto" ? "generic" : selection;
		std::ostringstream metadata;
		metadata << "\"pid\":" << getpid() << ",\"vid\":" << description.idVendor
			<< ",\"product_id\":" << description.idProduct << ",\"part\":" << unsigned(part)
			<< ",\"model\":" << json(model)
			<< ",\"serial\":" << json(usb_string(usb.handle, description.iSerialNumber))
			<< ",\"manufacturer\":" << json(usb_string(usb.handle, description.iManufacturer))
			<< ",\"product\":" << json(usb_string(usb.handle, description.iProduct))
			<< ",\"interfaces\":[";
		for (std::size_t index = 0; index < available.size(); ++index) {
			if (index) metadata << ',';
			metadata << "{\"number\":" << available[index].number << ",\"name\":" << json(available[index].name) << '}';
		}
		metadata << ']';
		if (probe) { report(report_path, "{" + metadata.str() + "}"); return 0; }
		require(!available.empty(), "No serial bulk interfaces found");
		require(!directory.empty() && directory.front() == '/', "--link-dir must be an absolute directory");
		struct stat info{};
		require(stat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode), "Link directory missing");
		signal(SIGINT, stop);
		signal(SIGTERM, stop);
		signal(SIGHUP, SIG_IGN);
		std::vector<std::unique_ptr<port>> ports;
		auto create = [&](int index, const std::string& name) {
			require(std::any_of(available.begin(), available.end(), [&](const usb_interface& item) {
				return item.number == index;
			}), "USB interface does not exist");
			ports.emplace_back(new port);
			ports.back()->initialize(usb.handle, index, name, directory, baud);
		};
		if (!port_names.empty()) {
			std::istringstream names(port_names);
			std::string name;
			std::vector<std::string> used;
			while (std::getline(names, name, ',')) {
				require(std::regex_match(name, std::regex("[a-zA-Z0-9][a-zA-Z0-9-]*")) &&
					std::find(used.begin(), used.end(), name) == used.end(), "Invalid or duplicate port name");
				require(used.size() < available.size(), "Too many port names");
				create(available[used.size()].number, name);
				used.push_back(name);
			}
			require(used.size() == available.size(), "Port names must match interface count");
		} else if (model == "ft891") {
			require(enhanced != standard, "Enhanced and Standard interfaces must differ");
			create(enhanced, "ft891-enhanced");
			create(standard, "ft891-standard");
		} else {
			for (std::size_t index = 0; index < available.size(); ++index)
				create(available[index].number, "ttyUSB" + std::to_string(index));
		}
		std::ostringstream links;
		for (std::size_t index = 0; index < ports.size(); ++index) {
			if (index) links << ',';
			links << json(ports[index]->name) << ':' << json(ports[index]->path);
		}
		int result = 0;
		try {
			for (auto& item : ports) item->process(follow);
			report(report_path, "{" + metadata.str() + ",\"ready\":true,\"baud\":" + std::to_string(baud)
				+ ",\"baud_mode\":" + json(follow ? "follow" : "fixed") + ",\"ports\":{" + links.str() + "}}");
			while (!stopping) {
				timeval timeout{0, 10000};
				int status = libusb_handle_events_timeout(usb.context, &timeout);
				require(status == 0 || status == LIBUSB_ERROR_INTERRUPTED, "USB event loop failed");
				for (auto& item : ports) item->process(follow);
			}
		} catch (const std::exception& error) {
			std::fprintf(stderr, "%s\n", error.what());
			result = 1;
		} catch (usbuart::error_t error) {
			std::fprintf(stderr, "USB driver error: %d\n", static_cast<int>(error));
			result = 1;
		}
		for (auto& item : ports) if (item->submitted) libusb_cancel_transfer(item->transfer);
		while (std::any_of(ports.begin(), ports.end(), [](const std::unique_ptr<port>& item) { return item->submitted; })) {
			timeval timeout{0, 10000};
			libusb_handle_events_timeout(usb.context, &timeout);
		}
		if (!report_path.empty()) unlink(report_path.c_str());
		return result;
	} catch (usbuart::error_t error) {
		std::fprintf(stderr, "USB driver initialization failed: %d\n", static_cast<int>(error));
	} catch (const std::exception& error) {
		std::fprintf(stderr, "%s\n", error.what());
	}
	return 1;
}
