/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2017  Jolla Ltd. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#ifndef __OFONO_STORAGE_H
#define __OFONO_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

const char *ofono_config_dir(void);
const char *ofono_storage_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_STORAGE_H */
