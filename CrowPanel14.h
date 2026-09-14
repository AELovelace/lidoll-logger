#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

// Advance 7-inch V1.3/V1.4/V1.5 wiring, verified against Elecrow's lesson-03 driver (README.md).
class CrowPanel14 : public lgfx::LGFX_Device {
  lgfx::Bus_RGB bus;
  lgfx::Panel_RGB panel;
  lgfx::Touch_GT911 touch;
public:
  CrowPanel14() { // Describe the RGB bus, PSRAM framebuffer and capacitive touch before starting hardware.
    auto p=panel.config();p.memory_width=p.panel_width=800;p.memory_height=p.panel_height=480;panel.config(p);
    auto detail=panel.config_detail();detail.use_psram=1;panel.config_detail(detail);
    auto b=bus.config();b.panel=&panel;
    b.pin_d0=21;b.pin_d1=47;b.pin_d2=48;b.pin_d3=45;b.pin_d4=38; // Blue, least to most significant.
    b.pin_d5=9;b.pin_d6=10;b.pin_d7=11;b.pin_d8=12;b.pin_d9=13;b.pin_d10=14; // Green.
    b.pin_d11=7;b.pin_d12=17;b.pin_d13=18;b.pin_d14=3;b.pin_d15=46; // Red.
    b.pin_henable=42;b.pin_vsync=41;b.pin_hsync=40;b.pin_pclk=39;b.freq_write=16000000;
    b.hsync_polarity=0;b.hsync_front_porch=8;b.hsync_pulse_width=4;b.hsync_back_porch=8;
    b.vsync_polarity=0;b.vsync_front_porch=8;b.vsync_pulse_width=4;b.vsync_back_porch=8;b.pclk_idle_high=1;
    bus.config(b);panel.setBus(&bus);
    auto t=touch.config();t.x_min=0;t.x_max=800;t.y_min=0;t.y_max=480;t.pin_int=-1;t.pin_rst=-1;
    t.bus_shared=false;t.offset_rotation=0;t.i2c_port=I2C_NUM_0;t.pin_sda=15;t.pin_scl=16;t.freq=400000;t.i2c_addr=0x5D;
    touch.config(t);panel.setTouch(&touch);setPanel(&panel);
  }
};
