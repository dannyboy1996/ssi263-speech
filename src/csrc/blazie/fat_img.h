/* fat_img.h -- a plain FAT disk image, written and read here (MIT, no other library): the form 7-Zip, Windows tools,
 * mtools and Linux (mount -o loop) all open.
 *
 * Written: FAT16, no partition table (a "superfloppy"), 512-byte sectors, 1 KB clusters, two FATs, a 512-entry root
 * directory, 8 MB unless the files need more (then larger, up to 2 GB with larger clusters).  Every file and folder
 * gets a long name (VFAT LFN entries) beside an 8.3 alias, so names keep their case, spaces and length.  The output
 * is deterministic: the same files give the same image byte for byte (fixed serial number, the files' own times).
 * Read: FAT12, FAT16 or FAT32, with or without a partition table (the first FAT partition), long names or 8.3 names
 * (with the lower-case flags Windows sets), deleted entries and the volume label skipped.
 *
 * Names are UTF-8 here.  Portable C99.
 */
#ifndef FAT_IMG_H
#define FAT_IMG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- writing ---- */
typedef struct fat_builder fat_builder;

/* label: the volume label (up to 11 characters, upper-cased; NULL none) */
fat_builder *fat_new(const char *label);
/* a folder in folder `parent` (0: the root); its id (> 0), or -1 (out of memory, or a name already there) */
int fat_add_dir(fat_builder *b, int parent, const char *name, unsigned dos_time, unsigned dos_date);
/* a file (its bytes copied); 1, or 0 (out of memory, or the name already there) */
int fat_add_file(fat_builder *b, int parent, const char *name, const unsigned char *data, unsigned long n,
                 unsigned dos_time, unsigned dos_date, int read_only);
/* the image (malloc'd; free it), its size in *size; NULL with the reason in err */
unsigned char *fat_build(fat_builder *b, unsigned long *size, char *err, int errlen);
void fat_free(fat_builder *b);

/* ---- reading ---- */
typedef struct {
    char name[768];                 /* UTF-8 */
    int dir;                        /* 1: a folder */
    int parent;                     /* the index of its folder, -1: the root */
    int attr;                       /* the FAT attribute byte (01 read-only, 02 hidden, 04 system, 10 folder) */
    unsigned long size;
    unsigned dos_time, dos_date;    /* last written */
} fat_entry;

typedef struct fat_volume fat_volume;

/* the image's folders and files, every level (the image itself is not kept: fat_data copies from it, so keep it
   until done); NULL with the reason in err */
fat_volume *fat_read(const unsigned char *img, unsigned long size, char *err, int errlen);
int fat_count(const fat_volume *v);
const fat_entry *fat_get(const fat_volume *v, int i);
/* entry i's bytes (malloc'd; free them), *n its size; NULL if damaged (a chain shorter than the size) */
unsigned char *fat_data(const fat_volume *v, int i, unsigned long *n);
void fat_close(fat_volume *v);

/* the tests' must-fail control (an app never sets it): 1 writes every long name with a wrong checksum, so other
   tools show the 8.3 aliases instead */
extern int fat_break;

#ifdef __cplusplus
}
#endif
#endif
