/*
 * NVMe over Fabrics Distributed Endpoint Manager (NVMe-oF DEM).
 * Copyright (c) 2017 Intel Corporation., Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

void show_ctrl_data(json_t *parent, int formatted);
void show_ctrl_list(json_t *parent, int formatted);
void show_host_data(json_t *parent, int formatted);
void show_host_list(json_t *parent, int formatted);
void show_config(json_t *parent, int formatted);

#ifndef UNUSED
#define UNUSED(x) ((void) x)
#endif
