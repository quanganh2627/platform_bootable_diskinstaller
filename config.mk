ifeq ($(TARGET_USE_DISKINSTALLER),true)

diskinstaller_root := bootable/diskinstaller

installer_root_out := $(TARGET_INSTALLER_OUT)/installer-root
installer_system_out := $(installer_root_out)/system
installer_live_root_out := $(TARGET_INSTALLER_OUT)/live-root

android_sysbase_modules := \
	libc \
	libcutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	libusbhost \
	linker \
	ash \
	toolbox \
	logcat \
	gdbserver \
	strace \
	netcfg
android_sysbase_files = \
	$(call module-installed-files,$(android_sysbase_modules))

# $(1): source base dir
# $(2): target base dir
define sysbase-copy-files
$(hide) $(foreach _f,$(android_sysbase_files), \
	f=$(patsubst $(1)/%,$(2)/%,$(_f)); \
	mkdir -p `dirname $$f`; \
	echo "Copy: $$f" ; \
	cp -fR $(_f) $$f; \
)
endef

installer_base_modules := \
	libdiskconfig \
	libext2fs \
	libext2_com_err \
	libext2_e2p \
	libext2_blkid \
	libext2_uuid \
	libext2_profile \
	libext4_utils \
	libz \
	badblocks \
	make_ext4fs \
	resize2fs \
	tune2fs \
	mke2fs \
	e2fsck
installer_base_files = \
	$(call module-built-files,$(installer_base_modules))

# $(1): source base dir
# $(2): target base dir
define installer-copy-modules
$(hide) $(foreach m,$(installer_base_modules), \
	src=$(firstword $(strip $(call module-built-files,$(m)))); \
	dest=$(patsubst $(strip $(1))/%,$(strip $(2))/%,\
		$(firstword $(strip $(call module-installed-files,$(m))))); \
	echo "Copy: $$src -> $$dest"; \
	mkdir -p `dirname $$dest`; \
	cp -fdp $$src $$dest; \
)
endef

# Build the installer ramdisk image
installer_initrc := $(diskinstaller_root)/init.rc
installer_ramdisk := $(TARGET_INSTALLER_OUT)/ramdisk-installer.img.gz
installer_binary := \
	$(call intermediates-dir-for,EXECUTABLES,diskinstaller)/diskinstaller
$(installer_ramdisk): $(diskinstaller_root)/config.mk \
		$(MKBOOTFS) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(INSTALLED_BOOTIMAGE_TARGET) \
		$(TARGET_DISK_LAYOUT_CONFIG) \
		$(installer_binary) \
		$(installer_initrc) \
		$(TARGET_DISKINSTALLER_CONFIG) \
		$(android_sysbase_files) \
		$(installer_base_files) \
		$(INSTALLED_BUILD_PROP_TARGET) $(MINIGZIP)
	@echo ----- Making installer image ------
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) rm -rf $(installer_root_out)
	@echo Copying baseline ramdisk...
	$(hide) cp -fR $(TARGET_ROOT_OUT) $(installer_root_out)
	$(hide) rm -f $(installer_root_out)/initlogo.rle
	$(hide) mkdir -p $(installer_system_out)/
	$(hide) mkdir -p $(installer_system_out)/etc
	@echo Copying sysbase files...
	$(call sysbase-copy-files,$(TARGET_OUT),$(installer_system_out))
	@echo Copying installer base files...
	$(call installer-copy-modules,$(TARGET_OUT),\
		$(installer_system_out))
	@echo Modifying ramdisk contents...
	$(hide) cp -f $(installer_initrc) $(installer_root_out)/
	$(hide) cp -f $(TARGET_DISK_LAYOUT_CONFIG) \
		$(installer_system_out)/etc/disk_layout.conf
	$(hide) cp -f $(TARGET_DISKINSTALLER_CONFIG) \
		$(installer_system_out)/etc/installer.conf
	$(hide) cp -f $(installer_binary) $(installer_system_out)/bin/installer
	$(hide) chmod ug+rw $(installer_root_out)/default.prop
	$(hide) cat $(INSTALLED_BUILD_PROP_TARGET) >> $(installer_root_out)/default.prop
	$(hide) $(MKBOOTFS) $(installer_root_out) | $(MINIGZIP) > $@
	@echo ----- Made installer ramdisk -[ $@ ]-


# Data image containing all the images that installer will write to the device
installer_data_img := $(TARGET_INSTALLER_OUT)/installer_data.squashfs
installer_data_images := $(INSTALLED_BOOTLOADER_MODULE) \
			 $(INSTALLED_BOOTIMAGE_TARGET) \
			 $(INSTALLED_RECOVERYIMAGE_TARGET) \
			 $(INSTALLED_SYSTEMIMAGE) \
			 $(INSTALLED_USERDATAIMAGE_TARGET)
ifeq ($(TARGET_STAGE_DROIDBOOT),true)
installer_data_images += $(DROIDBOOT_BOOTIMAGE)
endif
ifeq ($(TARGET_USE_SYSLINUX),true)
installer_data_images += $(SYSLINUX_BASE)/mbr.bin
endif
$(installer_data_img): $(diskinstaller_root)/config.mk \
			$(installer_data_images)
	@echo --- Making installer data image ------
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)/data
	$(hide) cp -f $(installer_data_images) $(TARGET_INSTALLER_OUT)/data/
	$(hide) PATH=/sbin:/usr/sbin:$(PATH) mksquashfs $(TARGET_INSTALLER_OUT)/data $@ -no-recovery -noappend
	@echo --- Finished installer data image -[ $@ ]-


# Ramdisk for the live boot environment
installer_live_initrc := $(diskinstaller_root)/init-live.$(TARGET_INSTALLER_BOOTMEDIA).rc
installer_live_ramdisk := $(TARGET_INSTALLER_OUT)/ramdisk-live.img.gz
$(installer_live_ramdisk):  $(diskinstaller_root)/config.mk \
		$(MKBOOTFS) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(installer_live_initrc) $(MINIGZIP)
	@echo "Creating live ramdisk: $@"
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) rm -rf $(installer_live_root_out)
	$(hide) cp -fR $(TARGET_ROOT_OUT) $(installer_live_root_out)
	$(hide) cp -f $(installer_live_initrc) $(installer_live_root_out)/init.$(TARGET_INSTALLER_BOOTMEDIA).rc
	$(hide) mkdir -p $(installer_live_root_out)/images
	$(hide) $(MKBOOTFS) $(installer_live_root_out) | $(MINIGZIP) > $@
	@echo "Done with live ramdisk -[ $@ ]-"


# Bootimage for entering the installation process
# Force normal VGA so that printks can be seen on the display
installer_boot_img := $(TARGET_INSTALLER_OUT)/installer_boot.img
$(installer_boot_img):  $(diskinstaller_root)/config.mk \
		$(INSTALLED_KERNEL_TARGET) \
		$(installer_ramdisk) \
		$(MKBOOTIMG)
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
		     --ramdisk $(installer_ramdisk) \
		     --cmdline "$(BOARD_KERNEL_CMDLINE) vga=normal androidboot.bootmedia=$(TARGET_INSTALLER_BOOTMEDIA)" \
		     --output $@


# Bootimage for launching into a live Android environment from the USB
# stick. The system partition is loopback mounted from the collection
# of images, and data/cache are ramdisks.
installer_live_boot_img := $(TARGET_INSTALLER_OUT)/installer_live_boot.img
$(installer_live_boot_img): $(diskinstaller_root)/config.mk \
		$(INSTALLED_KERNEL_TARGET) \
		$(installer_live_ramdisk) \
		$(MKBOOTIMG)
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
		     --ramdisk $(installer_live_ramdisk) \
		     --cmdline "$(BOARD_KERNEL_CMDLINE) androidboot.bootmedia=$(TARGET_INSTALLER_BOOTMEDIA)" \
		     --output $@


# Create a small amount of writable space under /stash where users can copy
# stuff off their device if needed onto the USB key
installer_stash_img := $(TARGET_INSTALLER_OUT)/installer_stash.img
installer_stash_size := 10M
$(installer_stash_img): $(diskinstaller_root)/config.mk $(MAKE_EXT4FS)
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) $(MAKE_EXT4FS) -l $(installer_stash_size) -a stash $@


# A phony target for all the diskinstaller image artifacts that don't
# depend on SYSLINUX
.PHONY: diskinstaller-partitions
diskinstaller-partitions: \
		$(installer_data_img) \
		$(installer_boot_img) \
		$(installer_live_boot_img) \
		$(installer_stash_img)

ifeq ($(TARGET_USE_SYSLINUX),true)
# bootloader partition contains syslinux binaries and configuration; the
# actual kernels for the boot targets live on other partitions and are
# in standard android boot format.
# The SYSLINUX Makefiles create one of these already, but we need one
# with our own configuration to bootload the USB stick
installer_bootloader_img := $(TARGET_INSTALLER_OUT)/installer_bootloader.img
installer_syslinux_files := $(diskinstaller_root)/splash.png \
		$(SYSLINUX_BASE)/vesamenu.c32 \
		$(SYSLINUX_BASE)/android.c32
$(installer_bootloader_img): $(diskinstaller_root)/config.mk \
		$(installer_syslinux_files) \
		$(SYSLINUX_MK_IMG) \
		$(SYSLINUX_BIN)
	$(hide) mkdir -p $(TARGET_INSTALLER_OUT)
	$(hide) $(SYSLINUX_MK_IMG) --syslinux $(SYSLINUX_BIN) \
			   --tmpdir $(TARGET_INSTALLER_OUT)/bootloader \
			   --config $(diskinstaller_root)/syslinux.cfg \
			   --output $@ \
			   $(installer_syslinux_files)


# Create a hard disk image that can be dd'd directly to the USB
# stick block device
installer_layout := $(diskinstaller_root)/installer_img_layout.conf
edit_mbr := $(HOST_OUT_EXECUTABLES)/editdisklbl
INSTALLED_DISKINSTALLERIMAGE_TARGET := $(PRODUCT_OUT)/installer.img
$(INSTALLED_DISKINSTALLERIMAGE_TARGET): $(diskinstaller_root)/config.mk \
		$(installer_bootloader_img) \
		$(installer_live_boot_img) \
		$(installer_boot_img) \
		$(installer_data_img) \
		$(installer_stash_img) \
		$(edit_mbr) \
		$(installer_layout) \
		$(installer_mbr_bin)
	@echo "Creating bootable installer image: $@"
	$(hide) rm -f $@
	$(hide) touch $@
	$(hide) $(edit_mbr) -v -l $(installer_layout) -i $@ \
		inst_bootloader=$(installer_bootloader_img) \
		inst_boot=$(installer_boot_img) \
		inst_live=$(installer_live_boot_img) \
		inst_data=$(installer_data_img) \
		inst_stash=$(installer_stash_img)
	$(hide) dd if=$(SYSLINUX_BASE)/mbr.bin of=$@ bs=440 count=1 conv=notrunc
	@echo "Done with bootable installer image -[ $@ ]-"


# convert the installer_img (disk image) to a VirtualBox .vdi image
INSTALLED_VBOXINSTALLERIMAGE_TARGET := $(PRODUCT_OUT)/installer.vdi
$(INSTALLED_VBOXINSTALLERIMAGE_TARGET): \
		$(INSTALLED_DISKINSTALLERIMAGE_TARGET) \
		$(diskinstaller_root)/config.mk
	$(hide) rm -f $@
	$(hide) VBoxManage convertfromraw $< $@
	@echo "Done with VirtualBox bootable installer image -[ $@ ]-"

.PHONY: installer_img
installer_img: $(INSTALLED_DISKINSTALLERIMAGE_TARGET)

.PHONY: installer_vdi
installer_vdi: $(INSTALLED_VBOXINSTALLERIMAGE_TARGET)

endif # TARGET_USE_SYSLINUX
endif # TARGET_USE_DISKINSTALLER

