Linux 7.2.9 for the POCO X3 NFC (surya)
=======================================

Mainline Linux for the Xiaomi POCO X3 NFC (codename surya, Snapdragon 732G / SM7150).
It runs on the phone under postmarketOS with GNOME.

The tree is stable **v7.2.9**, plus the SM7150 work of
[sm7150-mainline/linux](https://github.com/sm7150-mainline/linux) (branch `v7.2`), plus the
surya changes listed below.

Changes for the POCO X3 NFC
===========================

Audio: WCD9375 codec, microphones and earpiece
----------------------------------------------

* **Device tree: WCD9375 microphone capture.** The codec sits on the LPASS SoundWire buses. Its
  reset is LPI GPIO 6. Supplies are L10A, L15A and BOB, and the three mic biases are 2700 mV, all
  as in Xiaomi's surya audio overlay. Capture gets its own MultiMedia2 frontend. `&q6asmdai dai@1`
  is added because q6asm only registers the DAIs listed there. It also gets a "WCD Capture" back
  end on TX_CODEC_DMA_TX_3 and the VA macro link. Built on the davinci work in sm7150-mainline
  PR #60.
* **Device tree: WCD9375 playback link** and a MultiMedia3 frontend, so the earpiece plays
  through the WCD EAR PA.
* **Headset detection (MBHC):** surya thresholds.
* **TAS2562 speaker amp:** Digital Volume range and MSB-first writes.
* **Quieter logs:** the q6afe/SoundWire debug prints are toned down.

Touchscreen and display
-----------------------

* **Double tap to wake.** When the panel turns off, the NT36xxx driver puts the controller into
  wakeup-gesture mode instead of suspending it. A double tap is reported as `KEY_WAKEUP` on a
  separate "NT36XXX Wake Gesture" input device. It is controlled by the `wake_gesture` module
  parameter (on by default).
* **Panel keeps the touch controller powered.** On unprepare, `panel-huaxing-nt36672c` leaves
  reset and supply alone (`keep_powered`, on by default), so the touch side of the TDDI stays
  alive.
* **nt36xxx fix: use-after-free.** `_nt36xxx_boot_download_firmware` released the firmware and
  then read its size and first byte. This faulted in the download worker at boot and could hang
  the phone.

Charging: USB-C power delivery
------------------------------

* **The port can sink power.** The connector was declared source-only with no `sink-pdos`, so
  the phone had nothing to request from a PD charger. Each time, the request failed, the charger
  sent a hard reset about 30 ms later and dropped VBUS, and charging kept starting and stopping.
  The port is now dual-role (`try-power-role = "sink"`) with a 5 V / 3 A sink PDO, and PD chargers
  hold a 5 V contract.
* **Limits:** the device tree keeps the charger input at 1.5 A, and the driver fixes the battery
  charge current at 1.95 A. The input limit (`current_max`) can be raised from userspace, for
  example by a temperature-guarded service. 9 V PD needs driver work. The 33 W fast charging
  uses an external charge pump with no mainline driver.

Wi-Fi: 5 GHz channels
---------------------

* **`ath.regdomain=<country>`**, a new module option. The phone's Wi-Fi calibration names China
  (CN). On top of that, for any country in the calibration, the ath driver first applied its most
  restrictive world table, which leaves out 5 GHz channels 100-144 permanently. Networks on those
  channels were invisible. The option replaces the calibration's country with the one the phone is
  used in, and starts from a table covering all of 2.4 GHz and 5 GHz up to 5875 MHz, so that
  country's rules decide. Set it with `options ath regdomain=DE` in `/etc/modprobe.d/`.

Cameras: CAMSS and sensors
--------------------------

Status of the cameras:

| Camera     | Sensor                    | PHY                                         | State   |
|------------|---------------------------|---------------------------------------------|---------|
| Main       | Sony IMX682 + DW9800 VCM  | CSIPHY0, C-PHY                              | working |
| Front      | Samsung S5K3T2            | CSIPHY1, D-PHY, behind the CAM_SEL mux      | working |
| Macro      | SK Hynix Hi-259           | CSIPHY2, combo mode (data lane 2, clock 3)  | working |
| Ultrawide  | SK Hynix Hi-1337          | CSIPHY1, behind the CAM_SEL mux             | working (full 4208x3120 mode only) |
| Depth      | OmniVision OV02B1B        |                                             | no driver |

### CAMSS: every SM7150 CSIPHY needs `csiphy0_clk`

* **The key fix.** The vendor device tree lists `CAM_CC_CSIPHY0_CLK` for all four CSIPHYs.
  Mainline only enabled it for CSIPHY0. Without it, CSIPHY1-3 saw the lanes toggle but never
  passed a packet to the CSID: zero SoT, while the CSID test pattern generator still worked.
  This is what blocked the front, macro and ultrawide cameras.
* **refgen** is voted as a CSIPHY supply.
* **CSIPHY RX clock** starts at the vendor's 384 MHz.
* **CSIPHY rails** run at the vendor voltages (vdda-phy 0.88 V, vdda-pll 1.2 V).

### CAMSS: SM7150 CSIPHY tables, combo mode and C-PHY

* **Lane tables.** Qualcomm's v1.2 lane tables (`csiphy_2ph_v1_2_reg` and the combo-mode variant)
  are programmed lane by lane, as the vendor driver does.
* **Combo mode** lets two sensors share one PHY. `clock-lanes = <3>` selects it, and the second
  sensor is clocked on lane 3.
* **C-PHY** for the main camera: the bus type is carried from the endpoint to the CSIPHY and CSID,
  with Qualcomm's `csiphy_3ph_v1_2_reg` trio tables.
* **Bus clocks.** VFE1 and VFE_LITE get the cpas/camnoc bus clocks.
* **Buffer flush fix.** `vfe_flush_buffers()` now clears every returned buffer. Before, a
  stop/start left requests queued, libcamera aborted in `stop()`, and switching cameras killed
  WirePlumber.
* **Shared PHYs.** Sensors that share a CSIPHY get mutable links (only the first is enabled), so
  libcamera can switch between them.
* **Debug cleanup.** The bring-up debug module parameters and trace prints are removed.
* **A failed stream start no longer breaks every camera.** When one stage failed to start, the
  stages already started stayed marked as streaming, and since all four cameras share CSID0/VFE0,
  every later start was silently skipped until a reboot. The started stages are now stopped again.
* **The main camera's lens driver (DW9807) stays quiet while the sensor is off.** It used to send
  writes into a powered-down lens, and the bus timeouts broke the IMX682's own start. Waking the
  lens now only switches on its supply. It is first addressed on a focus write, while streaming.
* **The IMX682 retries its init writes** (up to three times) when the shared bus hiccups. Switching
  between all four cameras in GNOME Snapshot works reliably (30/30 in a stress test).

### New drivers

* **`regulator: wl2866d`:** Will Semiconductor WL2866D camera LDO (2x DVDD, 2x AVDD over I2C).
  There is no public datasheet. The register layout comes from Xiaomi's driver, and the voltage
  encoding is fitted to the vendor codes.
* **`media: i2c: hi259`:** SK Hynix Hi-259, the 2 MP macro camera. 1600x1200 SBGGR10 on one
  lane at 30 fps, with exposure, gain and test pattern. The tables are decoded from the vendor
  CamX module.
* **`media: i2c: hi1337`:** SK Hynix Hi-1337, the 13 MP ultrawide, at 4208x3120 and 2104x1560.
  The register tables are written with `cci_multi_reg_write()`. `regmap_multi_reg_write()` on
  the 8-bit CCI regmap dropped the 16-bit width, so only the low byte of each entry went out.
* **Front and ultrawide no longer knock each other out at boot.** Both sensors share CSIPHY1 and
  power up at probe to read their chip id. The one that probed second found the shared switch busy,
  failed, and took every camera with it, because CAMSS waits for all sensors. A probe now waits up
  to 3 s for the switch.
* **Hi-1337 offers only its full 4208x3120 mode.** The binned 2104x1560 mode delivered frames with
  bands of shifted colour and garbled lines. The software ISP scales the full mode down at 30 fps.
* **`media: i2c: imx682`, `media: i2c: s5k3t2`:** the main and front sensors, from
  [woodyst/surya-pmos](https://github.com/woodyst/surya-pmos).
* **`media: dw9807-vcm`:** the main camera's DW9800 actuator gets its vcc (coil) supply. I2C errors
  on resume are non-fatal, and the regulator is no longer disabled twice.

### CAM_SEL mux

The front and ultrawide sensors share CSIPHY1 through a board switch (pm6150 GPIO). It is now a
`gpio-mux` controller: s5k3t2 selects state 0 and hi1337 selects state 1. The unused
`camera_mipi_switch_en` regulator is dropped so the mux GPIOs stay free.

### Device tree

* **Camera power:** the WL2866D LDOs.
* **Sensor nodes:** all four sensors and the actuator, each on its CSIPHY.
* **Rotation:** 270 for the cameras.

Known issues
------------

* **IMX682 probe at boot:** a bus timeout during its chip id read used to fail the probe, and with it
  every camera (CAMSS waits for all sensors). The probe now power-cycles the sensor and retries.
* **Untested:** headset microphone and headphone output.
* **Camera image quality:** no libcamera tuning yet.

Building
--------

Cross-compile for arm64 with the SM7150 config fragment and the drivers above:

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out defconfig sm7150.config
    ./scripts/config --file out/.config \
        -m REGULATOR_WL2866D -m VIDEO_QCOM_CAMSS -m VIDEO_HI259 -m VIDEO_HI1337 \
        -m VIDEO_IMX682 -m VIDEO_S5K3T2 -m VIDEO_DW9807_VCM \
        -m SND_SOC_WCD937X -m SND_SOC_WCD937X_SDW -m SND_SOC_TAS2562 \
        -e TOUCHSCREEN_NT36XXX_SPI
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out olddefconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out -j$(nproc) vmlinuz.efi dtbs modules

There are two device trees, one per display panel. Pick the one that matches your phone:

* `arch/arm64/boot/dts/qcom/sm7150-xiaomi-surya-huaxing.dtb`
* `arch/arm64/boot/dts/qcom/sm7150-xiaomi-surya-tianma.dtb`

Credits
-------

* **[sm7150-mainline](https://github.com/sm7150-mainline/linux):** the SM7150 platform and surya
  bring-up (Danila Tikhonov, Jens/Aelin Reidel, David Wronek, Luca Weiss and others). WCD9375
  audio for davinci by Artem Hutsaliuk (PR #60).
* **[woodyst/surya-pmos](https://github.com/woodyst/surya-pmos):** IMX682 / S5K3T2 drivers,
  C-PHY, the DW9807 fixes, refgen/rails/384 MHz.
* **[asidko/qcom-camss-mainline](https://github.com/asidko/qcom-camss-mainline):** the VFE
  buffer-flush fix.

The original kernel README is in [`README`](README).

