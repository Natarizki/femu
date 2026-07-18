/*
 * emulate.h — femu-emulate configuration and API
 *
 * femu-emulate is the OS/system emulation frontend.
 * Flags mirror QEMU-system style but simplified:
 *
 *   femu-emulate -os ubuntu.iso -hda disk.qcow2 -vga virtio -cpu aarch64
 */

#ifndef FEMU_EMULATE_H
#define FEMU_EMULATE_H

#include "femu.h"

/* ─── VGA (display) type ──────────────────────────────────────────── */
typedef enum {
    VGA_NONE    = 0,   /* no display device                  */
    VGA_STD     = 1,   /* standard VGA (640x480)             */
    VGA_CIRRUS  = 2,   /* Cirrus Logic GD5446                */
    VGA_VMWARE  = 3,   /* VMware SVGA-II                     */
    VGA_VIRTIO  = 4,   /* VirtIO GPU (paravirtualised)       */
    VGA_QXL     = 5,   /* QXL (SPICE)                        */
} FEMUVgaType;

/* ─── Disk image type ─────────────────────────────────────────────── */
typedef enum {
    DISK_RAW    = 0,   /* flat raw binary                    */
    DISK_QCOW2  = 1,   /* QEMU Copy-On-Write v2              */
    DISK_ISO    = 2,   /* ISO 9660 (CD/DVD)                  */
} FEMUDiskType;

/* ─── Disk drive record ───────────────────────────────────────────── */
#define EMULATE_MAX_DRIVES 8

typedef struct {
    const char   *path;      /* file path                        */
    FEMUDiskType  dtype;     /* image format                     */
    bool          readonly;  /* true for ISO / OS images         */
    char          id[8];     /* "hda", "hdb", "cdrom", "os" …   */
} FEMUDrive;

/* ─── Full emulate config ─────────────────────────────────────────── */
typedef struct {
    /* Machine */
    FEMUArch    cpu_arch;          /* guest CPU arch                  */
    const char *cpu_model;         /* CPU model string (e.g. "cortex-a57") */
    uint64_t    ram_size;          /* RAM in bytes                    */
    int         smp_cores;

    /* Storage */
    FEMUDrive   drives[EMULATE_MAX_DRIVES];
    int         n_drives;

    /* Display */
    FEMUVgaType vga;
    const char *vga_name;          /* original user string            */

    /* Boot OS image (shorthand -os flag, stored as cdrom drive) */
    const char *os_image;

    /* Flags */
    bool        verbose;
    bool        debug;
    bool        no_reboot;         /* exit instead of rebooting       */
    bool        nographic;         /* disable graphical window        */
} FEMUEmulateConfig;

/* ─── API ─────────────────────────────────────────────────────────── */
FEMUVgaType emulate_vga_from_string(const char *s);
const char *emulate_vga_name(FEMUVgaType v);
FEMUDiskType emulate_disk_type_from_path(const char *path);

void emulate_dump_config(const FEMUEmulateConfig *cfg, FILE *f);
int  emulate_run(FEMUEmulateConfig *cfg);

#endif /* FEMU_EMULATE_H */
