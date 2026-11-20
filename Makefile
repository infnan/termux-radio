PREFIX ?= /usr/local
CXX := $(if $(wildcard $(PREFIX)/bin/clang++),$(PREFIX)/bin/clang++,c++)
BUILD-DIR ?= build
TARGET-DIR := bin
CPPFLAGS += -Iinclude -Isrc $(shell pkg-config --cflags libusb-1.0)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++14 -Wall -Wextra -fPIC -MMD -MP
USB_LIBS := $(shell pkg-config --libs libusb-1.0)
PTY_LIBS := $(if $(filter Android,$(shell uname -o 2>/dev/null)),,-lutil)
SOURCES := capi ch34x core cp210x ftdi generic log pl2303
OBJECTS := $(addprefix $(BUILD-DIR)/,$(addsuffix .o,$(SOURCES)))

.PHONY: all clean install
all: $(TARGET-DIR)/libusbuart.so radio-bridge

$(BUILD-DIR) $(TARGET-DIR):
	mkdir -p $@

$(BUILD-DIR)/%.o: src/%.cpp | $(BUILD-DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD-DIR)/%.o: examples/%.cpp | $(BUILD-DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TARGET-DIR)/libusbuart.so: $(OBJECTS) | $(TARGET-DIR)
	$(CXX) $(LDFLAGS) -shared -Wl,-soname,libusbuart.so -o $@ $^ $(USB_LIBS)

ptyserial uartcat radio-bridge: %: $(BUILD-DIR)/%.o $(TARGET-DIR)/libusbuart.so
	$(CXX) $(LDFLAGS) -o $@ $< -L$(TARGET-DIR) -Wl,-rpath,'$$ORIGIN/bin:$$ORIGIN/../lib' -lusbuart $(USB_LIBS) $(PTY_LIBS)

install: all
	install -d $(PREFIX)/bin $(PREFIX)/lib $(PREFIX)/libexec/termux-radio
	install -m755 $(TARGET-DIR)/libusbuart.so $(PREFIX)/lib/
	install -m755 radio-bridge $(PREFIX)/bin/
	install -m755 scripts/termux-usb-serial scripts/termux-radio $(PREFIX)/bin/
	install -m644 scripts/termux_usb_serial.py $(PREFIX)/libexec/termux-radio/

clean:
	rm -f $(OBJECTS) $(BUILD-DIR)/*.d $(BUILD-DIR)/ptyserial.o $(BUILD-DIR)/uartcat.o $(BUILD-DIR)/radio-bridge.o
	rm -f $(TARGET-DIR)/libusbuart.so ptyserial uartcat radio-bridge

-include $(wildcard $(BUILD-DIR)/*.d)
