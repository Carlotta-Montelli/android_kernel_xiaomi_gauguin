#ifndef __KSU_H_APK_V2_SIGN
#define __KSU_H_APK_V2_SIGN

#include <linux/types.h>
#include "ksu.h"

bool ksu_is_manager_apk(char *path);

// Get dynamic sign configuration for signature verification
bool ksu_get_dynamic_sign_config(unsigned int *size, const char **hash);

#endif