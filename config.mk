# note: requires x86 because we assume grub is the mbr bootloader.
ifeq ($(TARGET_ARCH),x86)
ifeq ($(TARGET_USE_DISKINSTALLER),true)

diskinstaller_root := bootable/diskinstaller

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
installer_kernel := $(INSTALLED_KERNEL_TARGET)
installer_ramdisk := $(TARGET_INSTALLER_OUT)/ramdisk-installer.img.gz
installer_build_prop := $(INSTALLED_BUILD_PROP_TARGET)
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
		$(installer_build_prop)
	@echo ----- Making installer image ------
	rm -rf $(TARGET_INSTALLER_ROOT_OUT)
	mkdir -p $(TARGET_INSTALLER_OUT)
	mkdir -p $(TARGET_INSTALLER_ROOT_OUT)
	mkdir -p $(TARGET_INSTALLER_ROOT_OUT)/sbin
	mkdir -p $(TARGET_INSTALLER_ROOT_OUT)/data
	mkdir -p $(TARGET_INSTALLER_ROOT_OUT)/images
	mkdir -p $(TARGET_INSTALLER_SYSTEM_OUT)
	mkdir -p $(TARGET_INSTALLER_SYSTEM_OUT)/etc
	mkdir -p $(TARGET_INSTALLER_SYSTEM_OUT)/bin
	@echo Copying baseline ramdisk...
	cp -fR $(TARGET_ROOT_OUT) $(TARGET_INSTALLER_OUT)
	@echo Copying sysbase files...
	$(call sysbase-copy-files,$(TARGET_OUT),$(TARGET_INSTALLER_SYSTEM_OUT))
	@echo Copying installer base files...
	$(call installer-copy-modules,$(TARGET_OUT),\
		$(TARGET_INSTALLER_SYSTEM_OUT))
	@echo Modifying ramdisk contents...
	cp -f $(installer_initrc) $(TARGET_INSTALLER_ROOT_OUT)/
	cp -f $(TARGET_DISK_LAYOUT_CONFIG) \
		$(TARGET_INSTALLER_SYSTEM_OUT)/etc/disk_layout.conf
	cp -f $(TARGET_DISKINSTALLER_CONFIG) \
		$(TARGET_INSTALLER_SYSTEM_OUT)/etc/installer.conf
	cp -f $(installer_binary) $(TARGET_INSTALLER_SYSTEM_OUT)/bin/installer
	$(hide) chmod ug+rw $(TARGET_INSTALLER_ROOT_OUT)/default.prop
	cat $(installer_build_prop) >> $(TARGET_INSTALLER_ROOT_OUT)/default.prop
	$(MKBOOTFS) $(TARGET_INSTALLER_ROOT_OUT) | gzip > $(installer_ramdisk)
	@echo ----- Made installer ramdisk -[ $@ ]-


######################################################################
# Now make a data image that contains all the target image files for the
# installer.

# $(1): src directory
# $(2): output file
# $(3): mount point
# $(4): ext variant (ext2, ext3, ext4)
# $(5): size of the partition
define build-installerimage-ext-target
  @mkdir -p $(dir $(2))
    $(hide) PATH=$(foreach p,$(INTERNAL_USERIMAGES_BINARY_PATHS),$(p):)$(PATH) \
          $(MKEXTUSERIMG) $(1) $(2) $(4) $(3) $(5)
endef

installer_data_img := $(TARGET_INSTALLER_OUT)/installer_data.squashfs
installer_bootloader := $(TARGET_INSTALLER_OUT)/data/bootldr.bin
installer_mbr_bin = $(SYSLINUX_BASE)/mbr.bin

$(installer_data_img): \
			$(diskinstaller_root)/config.mk \
			$(installer_target_data_files) \
			$(INSTALLED_BOOTIMAGE_TARGET) \
			$(INSTALLED_SYSTEMIMAGE) \
			$(INSTALLED_USERDATAIMAGE_TARGET) \
			$(installer_mbr_bin) \
			$(installer_ptable_bin) \
			$(installer_ramdisk)
	@echo --- Making installer data image ------
	mkdir -p $(TARGET_INSTALLER_OUT)
	mkdir -p $(TARGET_INSTALLER_OUT)/data
	cp $(installer_mbr_bin) $(installer_bootloader)
	cp -f $(INSTALLED_BOOTIMAGE_TARGET) $(TARGET_INSTALLER_OUT)/data/boot.img
	cp -f $(INSTALLED_SYSTEMIMAGE) \
		$(TARGET_INSTALLER_OUT)/data/system.img
	cp -f $(INSTALLED_USERDATAIMAGE_TARGET) \
		$(TARGET_INSTALLER_OUT)/data/userdata.img
	PATH=/sbin:/usr/sbin:$(PATH) mksquashfs $(TARGET_INSTALLER_OUT)/data $@ -no-recovery -noappend
	@echo --- Finished installer data image -[ $@ ]-

######################################################################

installer_live_initrc := $(diskinstaller_root)/init-live.$(TARGET_INSTALLER_BOOTMEDIA).rc
installer_live_ramdisk := $(PRODUCT_OUT)/ramdisk-live.img.gz
installer_boot_img := $(TARGET_INSTALLER_OUT)/installer_boot.img
installer_syslinux_cfg := $(TARGET_INSTALLER_OUT)/syslinux.cfg
installer_syslinux_cfgin := $(diskinstaller_root)/syslinux-installer.cfg.in
installer_syslinux_splash := $(diskinstaller_root)/splash.png
installer_syslinux_menu := $(SYSLINUX_BASE)/vesamenu.c32
installer_layout := $(diskinstaller_root)/installer_img_layout.conf
edit_mbr := $(HOST_OUT_EXECUTABLES)/editdisklbl


# Create the ramdisk for the live boot environment
$(installer_live_ramdisk): \
			$(installer_ramdisk) \
			$(MKBOOTFS) \
			$(INSTALLED_RAMDISK_TARGET) \
			$(INSTALLED_BUILD_PROP_TARGET) \
			$(TARGET_ROOT_OUT)/init.rc \
			$(installer_live_initrc)
	@echo "Creating live ramdisk: $@"
	rm -rf $(TARGET_LIVEINSTALLER_OUT)
	mkdir -p $(TARGET_LIVEINSTALLER_OUT)
	cp -fR $(TARGET_INSTALLER_ROOT_OUT) $(TARGET_LIVEINSTALLER_OUT)
	cp -f $(installer_live_initrc) $(TARGET_LIVEINSTALLER_ROOT_OUT)/init.$(TARGET_INSTALLER_BOOTMEDIA).rc
	cp -f $(INSTALLED_BUILD_PROP_TARGET) $(TARGET_LIVEINSTALLER_ROOT_OUT)
	cp -f $(TARGET_ROOT_OUT)/init.rc $(TARGET_LIVEINSTALLER_ROOT_OUT)
	mkdir -p $(TARGET_LIVEINSTALLER_ROOT_OUT)/cache
	$(MKBOOTFS) $(TARGET_LIVEINSTALLER_ROOT_OUT) | gzip > $@
	@echo "Done with live ramdisk -[ $@ ]-"


# Put the correct kernel command line in the syslinux configuration
$(installer_syslinux_cfg): $(installer_syslinux_cfgin) $(BOARD_INSTALLER_CMDLINE_FILE)
	mkdir -p $(TARGET_INSTALLER_OUT)
	sed "s|CMDLINE|$(shell cat $(BOARD_INSTALLER_CMDLINE_FILE))|g" $(installer_syslinux_cfgin) > $@

# Create the FAT boot partition with SYSLINUX installed on it
tmp_dir_for_inst_image := \
	$(call intermediates-dir-for,EXECUTABLES,installer_img)/installer_img

$(installer_boot_img): \
				$(installer_live_ramdisk) \
				$(installer_ramdisk) \
				$(installer_syslinux_menu) \
				$(installer_syslinux_splash) \
				$(installer_syslinux_cfg) \
				$(installer_kernel) \
				$(HOST_OUT_EXECUTABLES)/syslinux
	$(MKFATBOOTIMG) \
	    --syslinux $(HOST_OUT_EXECUTABLES)/syslinux \
	    --kernel $(installer_kernel) \
	    --file $(installer_live_ramdisk) \
	    --file $(installer_ramdisk) \
	    --file $(installer_syslinux_menu) \
	    --file $(installer_syslinux_splash) \
	    --file $(installer_syslinux_cfg) \
	    --tmpdir $(TARGET_INSTALLER_OUT)/boot \
	    --output $@

# Create a small amount of writable space under /stash where users can copy
# stuff off their device if needed onto the USB key
installer_stash_img := $(TARGET_INSTALLER_OUT)/installer_stash.img
installer_stash_size := 10M
$(installer_stash_img): $(MAKE_EXT4FS)
	$(MAKE_EXT4FS) -l $(installer_stash_size) -a stash $@

INSTALLED_DISKINSTALLERIMAGE_TARGET := $(PRODUCT_OUT)/installer.img
$(INSTALLED_DISKINSTALLERIMAGE_TARGET): \
					$(installer_boot_img) \
					$(installer_data_img) \
					$(installer_stash_img) \
					$(edit_mbr) \
					$(installer_layout) \
					$(installer_ptable_bin) \
					$(installer_mbr_bin)
	@echo "Creating bootable installer image: $@"
	@rm -f $@
	dd if=/dev/zero of=$@ bs=512 count=32
	$(edit_mbr) -v -l $(installer_layout) -i $@ \
		inst_boot=$(installer_boot_img) \
		inst_data=$(installer_data_img) \
		inst_stash=$(installer_stash_img)
	dd if=$(installer_mbr_bin) of=$@ bs=440 count=1 conv=notrunc
	@echo "Done with bootable installer image -[ $@ ]-"


######################################################################
# now convert the installer_img (disk image) to a VirtualBox image

INSTALLED_VBOXINSTALLERIMAGE_TARGET := $(PRODUCT_OUT)/installer.vdi
virtual_box_manager := VBoxManage
virtual_box_manager_options := convertfromraw

$(INSTALLED_VBOXINSTALLERIMAGE_TARGET): $(INSTALLED_DISKINSTALLERIMAGE_TARGET)
	@rm -f $(INSTALLED_VBOXINSTALLERIMAGE_TARGET)
	@$(virtual_box_manager) $(virtual_box_manager_options) $(INSTALLED_DISKINSTALLERIMAGE_TARGET) $(INSTALLED_VBOXINSTALLERIMAGE_TARGET)
	@echo "Done with VirtualBox bootable installer image -[ $@ ]-"

else  # ! TARGET_USE_DISKINSTALLER
INSTALLED_DISKINSTALLERIMAGE_TARGET :=
INSTALLED_VBOXINSTALLERIMAGE_TARGET :=
endif
endif # TARGET_ARCH == x86

.PHONY: installer_img
installer_img: $(INSTALLED_DISKINSTALLERIMAGE_TARGET)

.PHONY: installer_vdi
installer_vdi: $(INSTALLED_VBOXINSTALLERIMAGE_TARGET)
