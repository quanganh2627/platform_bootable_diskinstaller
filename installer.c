/* commands/sysloader/installer/installer.c
 *
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "installer"

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#include <cutils/config_utils.h>
#include <cutils/log.h>
#include <cutils/misc.h>
#include <make_ext4fs.h>
#include <sparse_format.h>

#include "diskconfig/diskconfig.h"
#include "installer.h"

#define MKE2FS_BIN          "/system/bin/mke2fs"
#define MAKE_EXT4FS_BIN     "/system/bin/make_ext4fs"
#define E2FSCK_BIN          "/system/bin/e2fsck"
#define TUNE2FS_BIN         "/system/bin/tune2fs"
#define RESIZE2FS_BIN       "/system/bin/resize2fs"
#define SIMG2IMG_BIN        "/system/bin/simg2img"
#define MKDOSFS_BIN         "/system/bin/newfs_msdos"
#define FSCK_MSDOS_BIN      "/system/bin/fsck_msdos"
#define MAX_DIGITS          (20)
/*SYSLINUX related*/
#define BOOTLOADER_PATH     "/bootloader"
#define SYSLINUX_BIN        "/system/bin/syslinux"
#define SYSLINUX_FILES_PATH "/data/syslinux"
#define SYSLINUX_CFG_TEM_FN SYSLINUX_FILES_PATH "/syslinux.cfg"
#define SYSLINUX_CFG_FN     BOOTLOADER_PATH "/syslinux.cfg"

static int
usage(void)
{
    fprintf(stderr, "Usage: %s\n", LOG_TAG);
    fprintf(stderr, "\t-c <path> - Path to installer conf file "
                    "(/system/etc/installer.conf)\n");
    fprintf(stderr, "\t-l <path> - Path to device disk layout conf file "
                    "(/system/etc/disk_layout.conf)\n");
    fprintf(stderr, "\t-h        - This help message\n");
    fprintf(stderr, "\t-d        - Dump the compiled in partition info.\n");
    fprintf(stderr, "\t-p <path> - Path to device that should be mounted"
                    " to /data.\n");
    fprintf(stderr, "\t-t        - Test mode. Don't write anything to disk.\n");
    return 1;
}

static cnode *
read_conf_file(const char *fn)
{
    cnode *root = config_node("", "");
    config_load_file(root, fn);

    if (root->first_child == NULL) {
        ALOGE("Could not read config file %s", fn);
        return NULL;
    }

    return root;
}

static int
exec_cmd(const char *cmd, ...) /* const char *arg, ...) */
{
    va_list ap;
    int size = 0;
    char *str;
    char *outbuf;
    int rv;

    /* compute the size for the command buffer */
    size = strlen(cmd) + 1;
    va_start(ap, cmd);
    while ((str = va_arg(ap, char *))) {
        size += strlen(str) + 1;  /* need room for the space separator */
    }
    va_end(ap);

    if (!(outbuf = malloc(size + 1))) {
        ALOGE("Can't allocate memory to exec cmd");
        return -1;
    }

    /* this is a bit inefficient, but is trivial, and works */
    strcpy(outbuf, cmd);
    va_start(ap, cmd);
    while ((str = va_arg(ap, char *))) {
        strcat(outbuf, " ");
        strcat(outbuf, str);
    }
    va_end(ap);

    ALOGI("Executing: %s", outbuf);
    rv = system(outbuf);
    if (rv < 0) {
        ALOGI("Error while trying to execute '%s'", cmd);
        rv = -1;
        goto exec_cmd_end;
    }
    rv = WEXITSTATUS(rv);
    ALOGI("Done executing %s (%d)", outbuf, rv);

exec_cmd_end:
    free(outbuf);
    return rv;
}

static int
do_vfat_fsck(const char *dst)
{
    int rv;
    int pass = 1;

    /* copied from /system/vold/Fat.cpp */
    ALOGI("Running fsck_msdos... This MAY take a while.");
    do {
        rv = exec_cmd(FSCK_MSDOS_BIN, "-p", "-f", dst, NULL);

        switch(rv) {
        case 0:
            ALOGI("Filesystem check completed OK");
            return 0;

        case 2:
            ALOGE("Filesystem check failed (not a FAT filesystem)");
            errno = ENODATA;
            return -1;

        case 4:
            if (pass++ <= 3) {
                ALOGW("Filesystem modified - rechecking (pass %d)",
                        pass);
                continue;
            }
            ALOGE("Failing check after too many rechecks");
            errno = EIO;
            return -1;

        default:
            ALOGE("Filesystem check failed (unknown exit code %d)", rv);
            errno = EIO;
            return -1;
        }
    } while (0);

    return -1;
}

static int
do_e2fsck(const char *dst, int force)
{
    int rv;
    const char *opts = force ? "-fy" : "-y";


    ALOGI("Running e2fsck... (force=%d) This MAY take a while.", force);
    if ((rv = exec_cmd(E2FSCK_BIN, "-C 0", opts, dst, NULL)) < 0)
        return 1;
    if (rv >= 4) {
        ALOGE("Error while running e2fsck: %d", rv);
        return 1;
    }
    sync();
    ALOGI("e2fsck succeeded (exit code: %d)", rv);

    return 0;
}

static int
process_ext2_image(const char *dst, const char *src, uint32_t flags, int test, int part_size)
{
    int rv;
    char resize_fs[MAX_DIGITS];
    int fd;
    sparse_header_t sparse_hdr;
    int is_sparse_img = 0;

    /* Check if the src is a sparse image */
    fd = open(src, O_RDONLY);
    if (fd < 0) {
        ALOGE("Cannot open '%s' for read: %s", src, strerror(errno));
        return 1;
    }
    if (read(fd, &sparse_hdr, sizeof(sparse_header_t)) >= (int)sizeof(sparse_header_t)) {
        if (sparse_hdr.magic == SPARSE_HEADER_MAGIC) {
            is_sparse_img = 1;
        }
    }
    close(fd);

    /* First, write the image to disk. */
    if (is_sparse_img) {
        // write sparse image to disk using simg2img
        if ((rv = exec_cmd(SIMG2IMG_BIN, src, dst, NULL)) < 0)
            return 1;
        if (rv) {
            ALOGE("Error while running simg2img: %d", rv);
            return 1;
        }
    } else if (write_raw_image(dst, src, 0, test))
        return 1;

    if (test)
        return 0;

    /* Next, let's e2fsck the fs to make sure it got written ok, and
     * everything is peachy */
    if (do_e2fsck(dst, 1))
        return 1;

    /* set the mount count to 1 so that 1st mount on boot doesn't complain */
    if ((rv = exec_cmd(TUNE2FS_BIN, "-C", "1", dst, NULL)) < 0)
        return 1;
    if (rv) {
        ALOGE("Error while running tune2fs: %d", rv);
        return 1;
    }

    /* If the user requested that we resize, let's do it now */
    if (flags & INSTALL_FLAG_RESIZE) {
         /* Resize the filesystem to extend into the left partition space */
        if (part_size)
            rv = snprintf(resize_fs , MAX_DIGITS, "%dK", part_size);
        else
            strcpy(resize_fs, " ");
        if (rv < 0) {
            ALOGE("Error setting size: %d", rv);
            return 1;
        }
        if ((rv = exec_cmd(RESIZE2FS_BIN, "-F", dst, (char *)resize_fs, NULL)) < 0)
            return 1;
        if (rv) {
            ALOGE("Error while running resize2fs: %d", rv);
            return 1;
        }
        sync();
        if (do_e2fsck(dst, 0))
            return 1;
    }

    /* make this an ext3 fs? */
    if (flags & INSTALL_FLAG_ADDJOURNAL) {
        if ((rv = exec_cmd(TUNE2FS_BIN, "-j", dst, NULL)) < 0)
            return 1;
        if (rv) {
            ALOGE("Error while running tune2fs: %d", rv);
            return 1;
        }
        sync();
        if (do_e2fsck(dst, 0))
            return 1;
    }

    return 0;
}

static int
do_syslinux(const char* dst, struct disk_info *dinfo, int test)
{
    int rv = 1, i;
    DIR* dirp;
    struct dirent *de;
    char* src_fp = NULL;
    char* dst_fp = NULL;
    FILE *dst_fd;
    void *data = NULL;
    unsigned int data_sz;

    /* hold the partition numbers, -1 means non-exist */
    int part_misc_no = 0;
    int part_boot_no = 0;
    int part_recovery_no = 0;
    int part_droidboot_no = 0;
    char* part_dev;
    char* disk_dev_prefix;

    if (test) {
        ALOGE("SYSLINUX bootloader is not installed due to test mode.");
        return 0;
    }

    /* check for file existence first */
    if (access(SYSLINUX_CFG_TEM_FN, R_OK)) {
        ALOGE("Error: %s has no read access or does not exist", SYSLINUX_CFG_TEM_FN);
        return 1;
    }
    if (access(SYSLINUX_BIN, R_OK | X_OK)) {
        ALOGE("Error: %s has no read/execution access or does not exist", SYSLINUX_CFG_TEM_FN);
        return 1;
    }

    /* Figure out partition number and make sure essential ones are defined */
    if (asprintf(&disk_dev_prefix, "%s%%d", dinfo->device) == -1) {
        ALOGE("Error allocating memory");
        return 1;
    }
    part_dev = find_part_device(dinfo, "misc");
    if (part_dev) {
        sscanf(part_dev, disk_dev_prefix, &part_misc_no);
    }
    part_dev = find_part_device(dinfo, "boot");
    if (part_dev) {
        sscanf(part_dev, disk_dev_prefix, &part_boot_no);
    }
    part_dev = find_part_device(dinfo, "recovery");
    if (part_dev) {
        sscanf(part_dev, disk_dev_prefix, &part_recovery_no);
    }
    part_dev = find_part_device(dinfo, "droidboot");
    if (part_dev) {
        sscanf(part_dev, disk_dev_prefix, &part_droidboot_no);
    }

    if (!part_misc_no) {
        ALOGE("Error finding the 'misc' partition in partition table");
        return 1;
    }
    if (!part_boot_no) {
        ALOGE("Error finding the 'boot' partition in partition table");
        return 1;
    }
    if (!part_recovery_no) {
        ALOGE("Error finding the 'recovery' partition in partition table");
        return 1;
    }
    if (!part_droidboot_no) {
        ALOGI("Partiton 'droidboot' is not in partition table. There will be no droidboot on device.");
    }

    /* executing syslinux to installer bootloader */
    if ((rv = exec_cmd(SYSLINUX_BIN, "--install", dst, NULL)) < 0)
        return 1;
    if (rv) {
        ALOGE("Error while running syslinux: %d", rv);
        return 1;
    }

    /* Mount file system */
    if (mount(dst, BOOTLOADER_PATH, "vfat", 0, NULL)) {
        ALOGE("Could not mount %s on %s as vfat", dst, BOOTLOADER_PATH);
        return 1;
    }

    /* Copy files there */
    dirp = opendir(SYSLINUX_FILES_PATH);
    while (dirp && (de = readdir(dirp))) {
        if (de->d_type == DT_REG) {
            if (asprintf(&src_fp, "%s/%s", SYSLINUX_FILES_PATH, de->d_name) != -1) {
                if (asprintf(&dst_fp, "%s/%s", BOOTLOADER_PATH, de->d_name) != -1) {
                    /* load file first using cutils' load_file() */
                    data = load_file(src_fp, &data_sz);

                    if (!data) {
                        ALOGE("Error reading '%s': %s", src_fp, strerror(errno));
                        goto fail;
                    }

                    /* write to destination manually... */
                    dst_fd = fopen(dst_fp, "w");
                    if (!dst_fd) {
                        ALOGE("Error creating file '%s': %s", dst_fp, strerror(errno));
                        goto fail;
                    }

                    i = write(fileno(dst_fd), data, data_sz);
                    if (i < 0) {
                        ALOGE("Error writing to file '%s': %s", dst_fp, strerror(errno));
                        goto fail;
                    }

                    /* clean up */
                    fclose(dst_fd); dst_fd = NULL;
                    free(data);     data = NULL;
                    free(src_fp);   src_fp = NULL;
                    free(dst_fp);   dst_fp = NULL;
                } else {
                    ALOGE("Error constructing destination filename for '%s'", de->d_name);
                    goto fail;
                }
            } else {
                ALOGE("Error constructing source filename for '%s'", de->d_name);
                goto fail;
            }
        }
    }
    closedir(dirp);

    /* Process SYSLINUX config file */
    data = load_file(SYSLINUX_CFG_TEM_FN, &data_sz);
    if (!data) {
        ALOGE("Error reading '%s': %s", SYSLINUX_CFG_TEM_FN, strerror(errno));
        goto fail;
    }
    dst_fd = fopen(SYSLINUX_CFG_FN, "w");
    if (!dst_fd) {
        ALOGE("Error creating file '%s': %s", dst_fp, strerror(errno));
        goto fail;
    }

    i = write(fileno(dst_fd), data, data_sz);
    if (i < 0) {
        ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    i = fprintf(dst_fd, "menu androidcommand %d\n\n", part_misc_no);
    if (i < 0) {
        ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    i = fprintf(dst_fd,
            "label boot\n\tmenu label ^Boot Android system\n\tcom32 android.c32\n\tappend current %d\n\n",
            part_boot_no);
    if (i < 0) {
        ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    i = fprintf(dst_fd,
            "label recovery\n\tmenu label ^OS Recovery mode\n\tcom32 android.c32\n\tappend current %d\n\n",
            part_recovery_no);
    if (i < 0) {
        ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    if (part_droidboot_no > 0) {
        i = fprintf(dst_fd,
                "label fastboot\n\tmenu label ^Fastboot mode\n\tcom32 android.c32\n\tappend current %d\n\n",
                part_droidboot_no);
        if (i < 0) {
            ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
            goto fail;
        }
    }

    fclose(dst_fd); dst_fd = NULL;
    free(data);     data = NULL;

    /* force sync and un-mount */
    sync();
    umount(BOOTLOADER_PATH);

    rv = 0;

fail:
    if (dst_fd) fclose(dst_fd);
    if (src_fp) free(src_fp);
    if (dst_fp) free(dst_fp);
    if (data)   free(data);
    return rv;
}

/* TODO: PLEASE break up this function into several functions that just
 * do what they need with the image node. Many of them will end up
 * looking at same strings, but it will be sooo much cleaner */
static int
process_image_node(cnode *img, struct disk_info *dinfo, int test)
{
    struct part_info *pinfo = NULL;
    loff_t offset = (loff_t)-1;
    const char *filename = NULL;
    const char *partname = NULL;
    char *dest_part = NULL;
    const char *tmp;
    uint32_t flags = 0;
    uint8_t type = 0;
    int rv;
    int func_ret = 1;
    int part_size = 0; /* in kilo-bytes */
    int footer_size = 0; /* in kilo-bytes */
    int is_e2fs = 0;
    int is_vfat = 0;

    filename = config_str(img, "filename", NULL);
    /* Use partname as either filename or partition parameter */
    partname = filename;

    /* process the 'offset' image parameter */
    if ((tmp = config_str(img, "offset", NULL)) != NULL)
        offset = strtoull(tmp, NULL, 0);

    /* process the 'partition' image parameter */
    if ((tmp = config_str(img, "partition", NULL)) != NULL) {
        if (offset != (loff_t)-1) {
            ALOGE("Cannot specify the partition name AND an offset for %s",
                 img->name);
            goto fail;
        }

        if (!(pinfo = find_part(dinfo, tmp))) {
            ALOGE("Cannot find partition %s while processing %s",
                 tmp, img->name);
            goto fail;
        }

        if (!(dest_part = find_part_device(dinfo, pinfo->name))) {
            ALOGE("Could not get the device name for partition %s while"
                 " processing image %s", pinfo->name, img->name);
            goto fail;
        }
        partname = tmp;
        offset = pinfo->start_lba * dinfo->sect_size;
        part_size = pinfo->len_kb;
    }

    /* proess the 'footer' image parameter */
    if ((tmp = config_str(img, "footer", NULL)) != NULL) {
        char *footer_param, *p_end;
        float multiplier = 0.0f;
        int len = 0;
        char last_char = 0;

        if (!(footer_param = strdup(tmp))) {
            ALOGE("Cannot allocate memory for dup'd footer string");
            goto fail;
        }
        len = strlen(footer_param);
        last_char = footer_param[len - 1];

        if (last_char == 'K')
            multiplier = 1;
        else if (last_char == 'M')
            multiplier = 1024;
        else if (last_char == 'G')
            multiplier = 1024 * 1024;
        else if (!isdigit(last_char)) {
            ALOGE("[%s] Unsupported footer size suffix: '%c'", partname, last_char);
            free(footer_param);
            goto fail;
        }

        errno = 0;
        if (multiplier == 0) {
            /* Size is in bytes, transform it into kilo-bytes. */
            footer_size = strtol(footer_param, &p_end, 10) / 1024;
        } else {
            /* Size is suffixed. Cut the suffix and use the multiplier */
            footer_param[len - 1] = '\0';
            footer_size = strtol(footer_param, &p_end, 10) * multiplier;
        }
        /* Make sure that the number was correctly parsed and the parser went to
         * the end of the string*/
        if (errno != 0 || p_end - footer_param < (int)strlen(footer_param)) {
            ALOGE("[%s] Invalid footer size: %s", partname, tmp);
            free(footer_param);
            goto fail;
        }

        free(footer_param);
    }


    /* process the 'mkfs' parameter */
    if ((tmp = config_str(img, "mkfs", NULL)) != NULL) {
        char vol_lbl[16]; /* ext2/3 has a 16-char volume label */

        if (!pinfo) {
            ALOGE("Target partition required for mkfs for '%s'", img->name);
            goto fail;
        } else if (filename) {
            ALOGE("Providing filename and mkfs parameters is meaningless");
            goto fail;
        }

        /* put the partition name as the volume label */
        strncpy(vol_lbl, pinfo->name, sizeof(vol_lbl));

        if (!strcmp(tmp, "ext4")) {
            int reserved = 0;
            if (footer_size > 0) {
                ALOGI("[%s] Using footer %dKB as reserved bytes in make_ext4fs", partname, footer_size);
                reserved = -footer_size * 1024;
               /* Since we used the footer in make_ext4fs, reset it to 0, so we don't use it again
                * in resize2fs */
               footer_size = 0;
            }
            if (make_ext4fs(dest_part, reserved, vol_lbl, NULL)) {
                ALOGE("make_ext4fs failed");
                goto fail;
            }
        } else {
            if (!strcmp(tmp, "ext2")) {
                rv = exec_cmd(MKE2FS_BIN, "-L", vol_lbl, dest_part, NULL);
                is_e2fs = 1;
            } else if (!strcmp(tmp, "ext3")) {
                rv = exec_cmd(MKE2FS_BIN, "-L", vol_lbl, "-j", dest_part, NULL);
                is_e2fs = 1;
            } else if (!strcmp(tmp, "vfat")) {
                rv = exec_cmd(MKDOSFS_BIN, "-L", vol_lbl, dest_part, NULL);
                is_vfat = 1;
            } else {
                ALOGE("Unknown filesystem type for mkfs: %s", tmp);
                goto fail;
            }

            if (rv < 0)
                goto fail;
            else if (rv > 0) {
                ALOGE("Error creating filesystem for %s: %d", vol_lbl, rv);
                goto fail;
            }
            sync();
            if (is_e2fs) {
                if (do_e2fsck(dest_part, 0))
                    goto fail;
            } else if (is_vfat) {
                if (do_vfat_fsck(dest_part))
                    goto fail;
            }
        }

        /* need to install bootloader onto partition? */
        const char* tmp2;
        if ((tmp2 = config_str(img, "bootloader", NULL)) != NULL) {
            if (!strcmp(tmp2, "syslinux")) {
                if (is_vfat) {
                    if (do_syslinux(dest_part, dinfo, test)) {
                        ALOGE("Error installing SYSLINUX onto partition.");
                        goto fail;
                    }
                } else {
                    ALOGE("SYSLINUX cannot work with non-FAT filesystem.");
                    goto fail;
                }
            } else {
                ALOGE("Unknown bootloader type '%s'", tmp);
                goto fail;
            }
        }

        goto done;
    }

    /* since we didn't mkfs above, all the rest of the options assume
     * there's a filename involved */
    if (!filename) {
        ALOGE("Filename is required for image %s", img->name);
        goto fail;
    }

    /* process the 'flags' image parameter */
    if ((tmp = config_str(img, "flags", NULL)) != NULL) {
        char *flagstr, *flagstr_orig;

        if (!(flagstr = flagstr_orig = strdup(tmp))) {
            ALOGE("Cannot allocate memory for dup'd flags string");
            goto fail;
        }
        while ((tmp = strsep(&flagstr, ","))) {
            if (!strcmp(tmp, "resize"))
                flags |= INSTALL_FLAG_RESIZE;
            else if (!strcmp(tmp, "addjournal"))
                flags |= INSTALL_FLAG_ADDJOURNAL;
            else {
                ALOGE("Unknown flag '%s' for image %s", tmp, img->name);
                free(flagstr_orig);
                goto fail;
            }
        }
        free(flagstr_orig);
    }
    if (footer_size > 0 && part_size > footer_size) {
        /* Resize the partition to a size so that we will leave some space
         * free at the end of it, requested by 'footer'. */
        part_size -= footer_size;
        /* Also, make sure that the RESIZE flag is enabled. */
        flags |= INSTALL_FLAG_RESIZE;
        ALOGI("[%s] Using footer of size %dKB. Original size: %dKB, new size: %dKB",
                partname, footer_size, part_size + footer_size, part_size);
    }

    /* process the 'type' image parameter */
    if (!(tmp = config_str(img, "type", NULL))) {
        ALOGE("Type is required for image %s", img->name);
        goto fail;
    } else if (!strcmp(tmp, "raw")) {
        type = INSTALL_IMAGE_RAW;
    } else if (!strcmp(tmp, "ext2")) {
        type = INSTALL_IMAGE_EXT2;
    } else if (!strcmp(tmp, "ext3")) {
        type = INSTALL_IMAGE_EXT3;
    } else if (!strcmp(tmp, "ext4")) {
        type = INSTALL_IMAGE_EXT4;
    } else {
        ALOGE("Unknown image type '%s' for image %s", tmp, img->name);
        goto fail;
    }

    /* at this point we MUST either have a partition in 'pinfo' or a raw
     * 'offset', otherwise quit */
    if (!pinfo && (offset == (loff_t)-1)) {
        ALOGE("Offset to write into the disk is unknown for %s", img->name);
        goto fail;
    }

    if (!pinfo && (type != INSTALL_IMAGE_RAW)) {
        ALOGE("Only raw images can specify direct offset on the disk. Please"
             " specify the target partition name instead. (%s)", img->name);
        goto fail;
    }

    switch(type) {
        case INSTALL_IMAGE_RAW:
            if (write_raw_image(dinfo->device, filename, offset, test))
                goto fail;
            break;

        case INSTALL_IMAGE_EXT3:
            /* makes the error checking in the imager function easier */
            if (flags & INSTALL_FLAG_ADDJOURNAL) {
                ALOGW("addjournal flag is meaningless for ext3 images");
                flags &= ~INSTALL_FLAG_ADDJOURNAL;
            }
            /* ...fall through... */

        case INSTALL_IMAGE_EXT4:
            /* fallthru */

        case INSTALL_IMAGE_EXT2:
            if (process_ext2_image(dest_part, filename, flags, test, part_size))
                goto fail;
            break;

        default:
            ALOGE("Unknown image type: %d", type);
            goto fail;
    }

done:
    func_ret = 0;

fail:
    if (dest_part)
        free(dest_part);
    return func_ret;
}

int
main(int argc, char *argv[])
{
    char *disk_conf_file = "/system/etc/disk_layout.conf";
    char *inst_conf_file = "/system/etc/installer.conf";
    char *inst_data_dir = "/data";
    char *inst_data_dev = NULL;
    char *data_fstype = "ext4";
    cnode *config;
    cnode *images;
    cnode *img;
    unsigned int cnt;
    struct disk_info *dinfo;
    int dump = 0;
    int test = 0;
    int x;
    FILE *fp;

    while ((x = getopt (argc, argv, "thdc:l:p:")) != EOF) {
        switch (x) {
            case 'h':
                return usage();
            case 'c':
                inst_conf_file = optarg;
                break;
            case 'l':
                disk_conf_file = optarg;
                break;
            case 't':
                test = 1;
                break;
            case 'p':
                inst_data_dev = optarg;
                break;
            case 'd':
                dump = 1;
                break;
            default:
                fprintf(stderr, "Unknown argument: %c\n", (char)optopt);
                return usage();
        }
    }

    /* If the user asked us to wait for data device, wait for it to appear,
     * and then mount it onto /data */
    if (inst_data_dev && !dump) {
        struct stat filestat;

        ALOGI("Waiting for device: %s", inst_data_dev);
        while (stat(inst_data_dev, &filestat))
            sleep(1);
        ALOGI("Device %s ready", inst_data_dev);
        if (mount(inst_data_dev, inst_data_dir, data_fstype, MS_RDONLY, NULL)) {
            ALOGE("Could not mount %s on %s as %s", inst_data_dev, inst_data_dir,
                 data_fstype);
            return 1;
        }
    }

    /* Read and process the disk configuration */
    if (!(dinfo = load_diskconfig(disk_conf_file, NULL))) {
        ALOGE("Errors encountered while loading disk conf file %s",
             disk_conf_file);
        return 1;
    }

    if (process_disk_config(dinfo)) {
        ALOGE("Errors encountered while processing disk config from %s",
             disk_conf_file);
        return 1;
    }

    /* Was all of this for educational purposes? If so, quit. */
    if (dump) {
        dump_disk_config(dinfo);
        return 0;
    }

    /* This doesnt do anything but load the config file */
    if (!(config = read_conf_file(inst_conf_file)))
        return 1;

    /* Will erase any previously existing GPT partition table,
     * but for our purposes here we don't care */
    if (!(fp = fopen(dinfo->device, "w+"))) {
        ALOGE("Can't open disk device %s", dinfo->device);
        return 1;
    }
    if (fseek(fp, dinfo->sect_size, SEEK_SET) < 0) {
        ALOGE("fseek: %s", strerror(errno));
        return 1;
    }
    for (cnt = 0; cnt < (dinfo->skip_lba - 1) * dinfo->sect_size; cnt++) {
        if (fputc('\0', fp) == EOF) {
            ALOGE("Failed to zero out space before first partition");
            return 1;
        }
    }
    if (fclose(fp)) {
        ALOGE("fclose: %s", strerror(errno));
        return 1;
    }

    /* First, partition the drive */
    if (apply_disk_config(dinfo, test))
        return 1;

    /* Now process the installer config file and write the images to disk */
    if (!(images = config_find(config, "images"))) {
        ALOGE("Invalid configuration file %s. Missing 'images' section",
             inst_conf_file);
        return 1;
    }

    for (cnt = 0, img = images->first_child; img; img = img->next, cnt++) {
        if (process_image_node(img, dinfo, test)) {
            ALOGE("Unable to write data to partition. Try running 'installer' again.");
            return 1;
        }
    }

    /*
     * We have to do the apply() twice. We must do it once before the image
     * writes to layout the disk partitions so that we can write images to
     * them. We then do the apply() again in case one of the images
     * replaced the MBR with a new bootloader, and thus messed with
     * partition table.
     */
    if (apply_disk_config(dinfo, test))
        return 1;

    ALOGI("Done processing installer config. Configured %d images", cnt);
    ALOGI("Type 'reboot' or reset to run new image");
    return 0;
}
