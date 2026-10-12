U-Boot for the POCO X3 NFC (surya)
==================================

`u-boot-surya.img` is a ready-to-flash U-Boot for the POCO X3 NFC. It is the U-Boot the phone runs Mobian with,
and the one the Mobian recovery zip installs.

Boot chain: Xiaomi's ABL → U-Boot (an Android boot image in the `boot` partition) → systemd-boot on the ESP
→ the kernel from this repository.

    sha256sum -c SHA256SUMS
    fastboot flash boot u-boot-surya.img

What it needs on the phone
--------------------------

* **An unlocked bootloader**, and **verified boot off**:
  `fastboot --disable-verity --disable-verification flash vbmeta vbmeta.img`, with any stock vbmeta image.
* **The system is a whole disk image in `userdata`.** At start (`preboot`), U-Boot maps the `userdata`
  partition as a block device of its own (`blkmap`) and boots its EFI System Partition through `bootefi bootmgr`.
  `userdata` therefore holds a GPT image with an ESP (systemd-boot, the kernel, the initramfs and the DTB) and
  the root file system. It needs 4096-byte sectors, as on the phone's UFS. Write such an image to `userdata`
  with `fastboot flash userdata <image>` or from recovery.

What is different from the stock sm7150-mainline U-Boot
-------------------------------------------------------

Source: [sm7150-mainline/u-boot](https://github.com/sm7150-mainline/u-boot) at `f8c04469`, built with
`qcom_defconfig qcom-phone.config` plus `build/qcom-quiet.config`.

* **Quiet boot.** It leaves the panel as ABL left it, with the POCO logo (`CONFIG_NO_FB_CLEAR`). The console
  goes to serial only (`build/qcom-phone-quiet.env`), so no banner or probe messages are drawn over the logo,
  and `bootdelay=0` boots straight on. The boot menu, and any error, still show on the display: hold **Volume Down**
  while U-Boot starts. The menu offers fastboot, USB mass storage (the whole UFS), a serial gadget and a
  shell.
* **`CONFIG_BLKMAP`**, which `qcom_defconfig` lacks and the `preboot` above needs. Without it every boot stops
  at "press any key to continue".
* **The logo.** A small white Debian swirl is centred under the POCO logo, instead of the U-Boot logo in the top
  right corner (`build/u_boot_logo.bmp`: 100x100, 8-bit grey palette, uncompressed, and
  `build/0001-video-centre-the-logo.patch`).

The device tree inside is `sm7150-xiaomi-surya-huaxing.dtb`. U-Boot only needs it for the panel's framebuffer
and the buttons, and the Linux device tree is chosen separately on the ESP. The image also boots phones with the
Tianma panel (untested).

Build
-----

    # inside this directory, so that ../build is the build/ folder here
    git clone https://github.com/sm7150-mainline/u-boot src && cd src && git checkout f8c04469
    git apply ../build/0001-video-centre-the-logo.patch
    cp ../build/u_boot_logo.bmp drivers/video/u_boot_logo.bmp
    cp ../build/qcom-phone-quiet.env ../build/qcom-quiet.config board/qualcomm/   # next to qcom-phone.config
    make O=.output CROSS_COMPILE=aarch64-linux-gnu- qcom_defconfig qcom-phone.config qcom-quiet.config
    make -j$(nproc) O=.output CROSS_COMPILE=aarch64-linux-gnu-
    # needs: bison flex swig xxd libgnutls28-dev libssl-dev python3-dev uuid-dev

`build/u-boot.config` is the full resulting `.config`. The boot image is an Android boot image v0: page size
4096, kernel offset 0x8000, ramdisk 0x1000000, tags 0x100, no ramdisk. Its kernel is
`gzip(u-boot-nodtb.bin)` with the DTB appended:

    gzip -9 -n -c .output/u-boot-nodtb.bin > u-boot-nodtb.bin.gz
    cat u-boot-nodtb.bin.gz .output/dts/upstream/src/arm64/qcom/sm7150-xiaomi-surya-huaxing.dtb > kernel-dtb
    mkbootimg --kernel kernel-dtb --base 0x0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
        --tags_offset 0x100 --pagesize 4096 --header_version 0 -o u-boot-surya.img

Going back
----------

`fastboot flash boot` with the boot image the phone had before (a LineageOS `boot.img`, or another U-Boot).
Fastboot works without U-Boot: hold Volume Down + Power from power-off to reach ABL's fastboot.
