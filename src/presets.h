#pragma once

#include "settings_sync.h"
#include <stdbool.h>

#define PRESETS_MAX       8
#define PRESET_NAME_MAX  48   /* bytes, including null terminator */

typedef struct {
    char name[PRESET_NAME_MAX];
} preset_info_t;

/* Load preset names from NVS.  Returns count (0–PRESETS_MAX). */
int  presets_list(preset_info_t *infos, int max);

/* Save current settings under name (creates or overwrites). */
bool presets_save(const char *name, const settings_t *s);

/* Load settings for the given name into *s_out.  Returns false if not found. */
bool presets_load(const char *name, settings_t *s_out);

/* Delete a named preset.  Returns false if not found. */
bool presets_delete(const char *name);

/* Set / get the boot-default preset name.
   presets_set_default("") clears the default. */
bool presets_set_default(const char *name);
bool presets_get_default(char *out, int out_size);

/* Called once at boot: if a default preset is configured, apply it. */
void presets_apply_default(void);

/* Serialise the full preset list as JSON into buf.
   Returns bytes written (not including null terminator). */
int  presets_to_json(char *buf, int buf_size);
