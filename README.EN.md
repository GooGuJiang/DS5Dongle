# Pico2W DualSense 5 Bridge
[中文](./README.md)
> Turn the Pico2W into a wireless adapter for the DS5 controller

# Features
- Supports HD Haptics

# Usage
1. Hold down the BOOTSEL button on the Pico to enter flashing mode
2. Drag the `.uf2` file onto it
3. Put the DS5 controller into Bluetooth pairing mode
4. Enjoy it

- Adjusting the microphone volume changes the haptic gain multiplier, range: [1, 2]
- Enabling speaker mute disables the LED connection indicator (takes effect after the controller reconnects)
- Enabling microphone mute disables silent disconnection
- The device will only appear in the system after the controller is connected to the Pico

# Current Issues
- Audio may have slight stuttering
- Due to encoding requirements, the Pico needs to be overclocked. The current settings are 1.2V and 270MHz. In my own testing, the maximum stable frequency was 270MHz, with good results.
- If your Pico cannot boot with these overclocking settings, increase the voltage or lower the frequency yourself

# Future Plans

# Build
You need to upgrade the TinyUSB version in the Pico SDK to the latest version

# Citation
- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) - Inspiration for this project
- [egormanga/SAxense](https://github.com/egormanga/SAxense) - Haptics reports
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - Data report structure
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) - Speaker packet reports