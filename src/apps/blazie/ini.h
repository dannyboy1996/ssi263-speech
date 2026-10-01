/* ini.h -- a small settings file: [section] lines, key = value lines, comment lines starting ; or #.  Read whole,
 * changed in memory, written back with every line the person wrote kept (comments, order, settings this program
 * does not know).  No comments after a value: a value may hold ; (the advance bar's "a ;").  Portable C.
 */
#ifndef BLAZIE_INI_H
#define BLAZIE_INI_H

typedef struct ini ini;

/* the file's lines (a missing file: none); NULL only when out of memory */
ini *ini_load(const char *path);
void ini_free(ini *f);
/* the value, or dflt when the setting is not there; it stays good through the next seven calls */
const char *ini_get(const ini *f, const char *section, const char *key, const char *dflt);
int ini_get_int(const ini *f, const char *section, const char *key, int dflt);
/* sets the value: the line changed where it is, or added at the end of its section (the section added if new) */
void ini_set(ini *f, const char *section, const char *key, const char *value);
/* the section's settings in order: index 0.. while it returns 1 */
int ini_entry(const ini *f, const char *section, int index, char *key, int key_cap, char *value, int value_cap);
/* 1 on success */
int ini_save(const ini *f, const char *path);

#endif
