// SPDX-License-Identifier: DUAL GPL-2.0/BSD
/*
 * NVMe over Fabrics Distributed Endpoint Management (NVMe-oF DEM).
 * Copyright (c) 2017-2019 Intel Corporation, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "common.h"
#include "tags.h"

#define CFS_PATH		"/sys/kernel/config/nvmet/"
#define CFS_SUBSYS		"subsystems/"
#define CFS_NS			"namespaces/"
#define CFS_DEV_PATH		"device_path"
#define CFS_ALLOW_ANY		"attr_allow_any_host"
#define CFS_ENABLE		"enable"
#define CFS_PORTS		"ports/"
#define CFS_TR_ADRFAM		"addr_adrfam"
#define CFS_TR_TYPE		"addr_trtype"
#define CFS_TREQ		"addr_treq"
#define CFS_TR_ADDR		"addr_traddr"
#define CFS_TR_SVCID		"addr_trsvcid"
#define CFS_HOSTS		"hosts/"
#define CFS_ALLOWED		"allowed_hosts/"

#define SYSFS_PATH		"/sys/class/nvme"
#define SYSFS_PREFIX		"nvme"
#define SYSFS_PREFIX_LEN	4
#define SYSFS_DEVICE		SYSFS_PREFIX "%dn%d"
#define SYSFS_TRANSPORT		"transport"
#define SYSFS_PCIE		"pcie"
#define NULL_BLK_DEVICE		"/dev/nullb0"
#define NVME_DEVICE		"/dev/" SYSFS_DEVICE

#define TRUE			'1'
#define FALSE			'0'

#define REQUIRED		"required"
#define NOT_REQUIRED		"not required"
#define NOT_SPECIFIED		"not specified"

#define MAXPATHLEN		512
#define MAXSTRLEN		64

#define write_chr(fn, ch)				\
	do {						\
		fd = fopen(fn, "w");			\
		if (fd) {				\
			fputc(ch, fd);			\
			fclose(fd);			\
		}					\
	} while (0)
#define write_str(fn, s)				\
	do {						\
		fd = fopen(fn, "w");			\
		if (fd) {				\
			fputs(s, fd);			\
			fclose(fd);			\
		}					\
	} while (0)
#define read_str(fn, s)					\
	do {						\
		s[0] = 0;				\
		s[MAXSTRLEN - 1] = 0;			\
		fd = fopen(fn, "r");			\
		if (fd) {				\
			char *nl;			\
			fgets(s, MAXSTRLEN - 1, fd);	\
			nl = strchr(s, '\n');		\
			if (nl)				\
				*nl = 0;		\
			fclose(fd);			\
		}					\
	} while (0)

static void cfgfs_stop_targets(void)
{
}

static int cfgfs_start_targets(void)
{
	struct portid		*xport;
	char			 cmd[32];
	int			 ret;

	system("modprobe configfs");

#ifdef CONFIG_DEBUG
	system("modprobe null_blk");
#endif

	list_for_each_entry(xport, interfaces, node) {
		sprintf(cmd, "modprobe nvmet-%s", xport->type);
		ret = system(cmd);
		if (ret)
			print_info("modprobe nvmet-%s failed: %d", cmd, ret);
	}

	return 0;
}

static void delete_all_allowed_hosts(void)
{
	char			path[MAXPATHLEN];
	DIR			*dir;
	struct dirent		*entry;

	dir = opendir(CFS_ALLOWED);
	if (dir) {
		for_each_dir(entry, dir) {
			sprintf(path, CFS_ALLOWED "%s", entry->d_name);
			remove(path);
		}
		closedir(dir);
	}

}

static void delete_all_ns(void)
{
	char			path[MAXPATHLEN];
	FILE			*fd;
	DIR			*dir;
	struct dirent		*entry;

	dir = opendir(CFS_NS);
	if (dir) {
		for_each_dir(entry, dir) {
			sprintf(path, CFS_NS "%s", entry->d_name);
			chdir(path);
			write_chr(CFS_ENABLE, FALSE);
			chdir("../..");
			rmdir(path);
		}
		closedir(dir);
	}
}

static void unlink_all_ports(char *subsys)
{
	char			path[MAXPATHLEN];
	DIR			*dir;
	DIR			*portdir;
	struct dirent		*entry;
	struct dirent		*portentry;

	chdir(CFS_PATH);

	dir = opendir(CFS_PORTS);
	if (dir) {
		for_each_dir(entry, dir) {
			sprintf(path, CFS_PORTS "%s/" CFS_SUBSYS,
				entry->d_name);
			portdir = opendir(path);
			if (unlikely(!portdir))
				continue;
			for_each_dir(portentry, portdir)
				if (!strcmp(portentry->d_name, subsys)) {
					sprintf(path,
						CFS_PORTS "%s/" CFS_SUBSYS "%s",
						entry->d_name, subsys);
					remove(path);
				}
		}
		closedir(dir);
	}
}

/* replace ~ with * in bash
 * # cd /sys/kernel/config/nvmet/subsystems/
 * # rm ${SUBSYSTEM}/hosts/~
 * # echo 0 > ${SUBSYSTEM}/namespaces/~/enable
 * # rmdir ${SUBSYSTEM}/namespaces/~
 * # rm ../ports/~/subsystems/${SUBSYSTEM}
 * # rmdir ${SUBSYSTEM}
 */
static int cfgfs_delete_subsys(char *subsys)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	sprintf(path, CFS_PATH CFS_SUBSYS "%s", subsys);
	ret = chdir(path);
	if (ret)
		return 0;

	delete_all_allowed_hosts();
	delete_all_ns();
	unlink_all_ports(subsys);

	chdir(CFS_PATH CFS_SUBSYS);
	rmdir(subsys);

	chdir(dir);
	return 0;
}

/*
 * # cd /sys/kernel/config/nvmet/subsystems/
 * # echo creating ${SUBSYSTEM} NSID ${NSID} for device ${DEV}
 * # [ -e ${SUBSYSTEM} ] || mkdir ${SUBSYSTEM}
 * # echo -n 1 > ${SUBSYSTEM}/attr_allow_any_host
 */
static int cfgfs_create_subsys(char *subsys, int allowany)
{
	char			 dir[MAXPATHLEN];
	FILE			*fd;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_SUBSYS);
	if (ret)
		return -errno;

	ret = mkdir(subsys, 0x755);
	if (ret && errno == EEXIST)
		ret = 0;
	else if (ret)
		goto err;

	chdir(subsys);

	write_chr(CFS_ALLOW_ANY, allowany ? TRUE : FALSE);

	goto out;
err:
	ret = -errno;
out:
	chdir(dir);
	return ret;
}

/*
 * # cd /sys/kernel/config/nvmet/subsystems/
 * # mkdir ${SUBSYSTEM}/namespaces/${NSID}
 * # echo -n ${DEV} > ${SUBSYSTEM}/namespaces/${NSID}/device_path
 * # echo -n 1 > ${SUBSYSTEM}/namespaces/${NSID}/enable
 */
static int cfgfs_create_ns(char *subsys, int nsid, int devid, int devnsid)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	FILE			*fd;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_SUBSYS);
	if (ret)
		return -errno;

	sprintf(path, "%s/" CFS_NS "%d", subsys, nsid);
	ret = mkdir(path, 0x755);
	if (ret && errno == EEXIST)
		ret = 0;
	else if (ret)
		goto err;

	chdir(path);

	if (devid == NULLB_DEVID)
		write_str(CFS_DEV_PATH, NULL_BLK_DEVICE);
	else {
		sprintf(path, NVME_DEVICE, devid, devnsid);
		write_str(CFS_DEV_PATH, path);
	}

	write_chr(CFS_ENABLE, TRUE);

	goto out;
err:
	ret = -errno;
out:
	chdir(dir);
	return ret;
}

/*
 * # cd /sys/kernel/config/nvmet/subsystems/
 * # echo -n 0 > ${SUBSYSTEM}/namespaces/${NSID}/enable
 * # rmdir ${SUBSYSTEM}/namespaces/${NSID}
 */
static int cfgfs_delete_ns(char *subsys, int nsid)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	FILE			*fd;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_SUBSYS);
	if (ret)
		return 0;

	sprintf(path, "%s/" CFS_NS "%d", subsys, nsid);
	ret = chdir(path);
	if (ret)
		goto out;

	write_chr(CFS_ENABLE, FALSE);

	chdir("../../..");
	rmdir(path);
out:
	chdir(dir);
	return 0;
}

/*
 * # cd /sys/kernel/config/nvmet/hosts
 * # mkdir ${HOSTNQN}
 */
static int cfgfs_create_host(char *host)
{
	char			 dir[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_HOSTS);
	if (ret)
		return -errno;

	ret = mkdir(host, 0x755);
	if (ret)
		ret = (errno == EEXIST) ? 0 : -errno;
	chdir(dir);
	return ret;
}

/*
 * # cd /sys/kernel/config/nvmet/hosts
 * # rmdir ${HOSTNQN}
 */
static int cfgfs_delete_host(char *host)
{
	char			 dir[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_HOSTS);
	if (ret)
		return 0;

	rmdir(host);

	chdir(dir);
	return 0;
}

/*
 * # cd /sys/kernel/config/nvmet/ports
 * # mkdir ${NVME_PORT}
 * # echo -n ipv4 > ${NVME_PORT}/addr_adrfam
 * # echo -n rdma > ${NVME_PORT}/addr_trtype
 * # echo -n not required > ${NVME_PORT}/addr_treq
 * # echo -n ${TARGET} > ${NVME_PORT}/addr_traddr
 * # echo -n ${PORT} > ${NVME_PORT}/addr_trsvcid
 */
static int cfgfs_create_portid(int portid, char *fam, char *typ, int req,
			       char *addr, int svcid)
{
	char			 dir[MAXPATHLEN];
	char			 str[8];
	char			 val[MAXSTRLEN];
	FILE			*fd;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_PORTS);
	if (ret)
		return -errno;

	snprintf(str, sizeof(str) - 1, "%d", portid);
	ret = mkdir(str, 0x755);
	if (ret && errno != EEXIST) {
		ret = -errno;
		goto out;
	}

	chdir(str);

	write_str(CFS_TR_ADRFAM, fam);
	write_str(CFS_TR_TYPE, typ);
	write_str(CFS_TR_ADDR, addr);
	if (req == NVMF_TREQ_REQUIRED)
		write_str(CFS_TREQ, REQUIRED);
	else if (req == NVMF_TREQ_NOT_REQUIRED)
		write_str(CFS_TREQ, NOT_REQUIRED);
	else
		write_str(CFS_TREQ, NOT_SPECIFIED);

	snprintf(str, sizeof(str) - 1, "%d", svcid);
	write_str(CFS_TR_SVCID, str);

	if (!ret)
		goto out;

	ret = -EBUSY;

	read_str(CFS_TR_ADRFAM, val);
	if (strcmp(val, fam))
		goto out;
	read_str(CFS_TR_TYPE, val);
	if (strcmp(val, typ))
		goto out;
	read_str(CFS_TR_ADDR, val);
	if (strcmp(val, addr))
		goto out;
	read_str(CFS_TREQ, val);
	if (req == NVMF_TREQ_REQUIRED) {
		if (strcmp(val, REQUIRED))
			goto out;
	} else if (req == NVMF_TREQ_NOT_REQUIRED) {
		if (strcmp(val, NOT_REQUIRED))
			goto out;
	} else {
		if (strcmp(val, NOT_SPECIFIED))
			goto out;
	}

	snprintf(str, sizeof(str) - 1, "%d", svcid);
	read_str(CFS_TR_SVCID, val);
	if (strcmp(val, str))
		goto out;

	ret = 0;
out:
	chdir(dir);
	return ret;
}

/* replace ~ with * in bash
 * # cd /sys/kernel/config/nvmet/ports
 * # rm -f ${NVME_PORT}/subsystems/~
 * # rmdir ${NVME_PORT}
 */
static int cfgfs_delete_portid(int portid)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	char			 file[MAXPATHLEN+256];
	DIR			*subdir;
	struct dirent		*entry;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_PORTS);
	if (ret)
		return 0;

	snprintf(path, sizeof(path) - 1, "%d/" CFS_SUBSYS, portid);

	subdir = opendir(path);
	if (!subdir)
		goto out;

	for_each_dir(entry, subdir) {
//		sprintf(file, "%s/%s", path, entry->d_name);
		unsigned long l = sprintf(file, "%s/%s", path, entry->d_name);
		if (l >= sizeof(path))
			file[sizeof(path)-1] = 0;
		remove(file);
	}
	closedir(subdir);

	snprintf(path, sizeof(path) - 1, "%d", portid);
	rmdir(path);
out:
	chdir(dir);
	return 0;
}

/*
 * # cd /sys/kernel/config/nvmet/subsystems/
 * # ln -s ../../hosts/${HOSTNQN} ${SUBSYSTEM}/hosts/${HOSTNQN}
 */
static int cfgfs_link_host_to_subsys(char *subsys, char *host)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	char			 link[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_SUBSYS);
	if (ret)
		return -errno;

	sprintf(path, CFS_PATH CFS_HOSTS "%s", host);
	sprintf(link, "%s/" CFS_ALLOWED "%s", subsys, host);

	ret = symlink(path, link);
	if (ret)
		ret = (errno == EEXIST) ? 0 : -errno;

	chdir(dir);
	return ret;
}

/*
 * # cd /sys/kernel/config/nvmet/subsystems
 * # rm ${SUBSYSTEM}/hosts/{HOSTNQN}
 */
static int cfgfs_unlink_host_from_subsys(char *subsys, char *host)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_SUBSYS);
	if (ret)
		return -errno;

	sprintf(path, "%s/" CFS_ALLOWED "%s", subsys, host);
	ret = remove(path);
	if (ret)
		ret = (errno == ENOENT) ? 0 : -errno;

	chdir(dir);
	return ret;
}

/*
 * # cd /sys/kernel/config/nvmet/ports
 * # ln -s ../subsystems/${SUBSYSTEM} ${NVME_PORT}/subsystems/${SUBSYSTEM}
 */
static int cfgfs_link_port_to_subsys(char *subsys, int portid)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	char			 link[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_PORTS);
	if (ret)
		return -errno;

	sprintf(path, CFS_PATH CFS_SUBSYS "%s", subsys);
	sprintf(link, "%d/" CFS_SUBSYS "%s", portid, subsys);

	ret = symlink(path, link);
	if (ret)
		ret = (errno == EEXIST) ? 0 : -errno;

	chdir(dir);
	return ret;
}

/*
 * # cd /sys/kernel/config/nvmet/ports
 * # rm ${NVME_PORT}/subsystems/${SUBSYSTEM}
 */
static int cfgfs_unlink_port_from_subsys(char *subsys, int portid)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH CFS_PORTS);
	if (ret)
		return -errno;

	sprintf(path, "%d/" CFS_SUBSYS "%s", portid, subsys);
	ret = remove(path);
	if (ret)
		ret = (errno == ENOENT) ? 0 : -errno;

	chdir(dir);
	return ret;
}

/* replace ~ with * in bash
 * # cd /sys/kernel/config/nvmet
 * # rm -f subsystems/~/hosts/~
 * # for i in subsystems/~/namespaces/~/enable ; do echo 0 > $i ; done
 * # rm -f ports/~/subsystems/~
 * # rmdir subsystems/~/namespaces/~
 * # rmdir subsystems/~
 * # rmdir ports/~
 * # rmdir hosts/~
 */
static void cfgfs_reset_config(void)
{
	char			 dir[MAXPATHLEN];
	DIR			*subdir;
	struct dirent		*entry;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(CFS_PATH);
	if (ret)
		goto out;

	subdir = opendir(CFS_PATH CFS_SUBSYS);
	if (!subdir)
		goto ports;

	for_each_dir(entry, subdir)
		cfgfs_delete_subsys(entry->d_name);
	closedir(subdir);

ports:
	subdir = opendir(CFS_PATH CFS_PORTS);
	if (!subdir)
		goto hosts;

	for_each_dir(entry, subdir)
		cfgfs_delete_portid(atoi(entry->d_name));
	closedir(subdir);

hosts:
	subdir = opendir(CFS_PATH CFS_HOSTS);
	if (!subdir)
		goto out;

	for_each_dir(entry, subdir)
		cfgfs_delete_host(entry->d_name);
	closedir(subdir);
out:
	chdir(dir);
}

static int create_device(char *name)
{
	struct nsdev		*device;

	device = malloc(sizeof(*device));
	if (!device) {
		free_devices();
		return -ENOMEM;
	}

	if (name) {
		sscanf(name, SYSFS_DEVICE, &device->devid, &device->nsid);
		print_debug("adding device nvme%dn%d",
				device->devid, device->nsid);
	} else {
		device->devid = NULL_BLK_DEVID;
		device->nsid = 0;
		print_debug("adding device nullb0");
	}

	list_add_tail(&device->node, devices);

	return 0;
}

static int cfgfs_enumerate_devices(void)
{
	char			 dir[MAXPATHLEN];
	char			 path[MAXPATHLEN];
	char			 buf[5];
	DIR			*subdir;
	DIR			*nvmedir;
	struct dirent		*entry;
	struct dirent		*subentry;
	FILE			*fd;
	int			 cnt = 0;
	int			 ret;

	getcwd(dir, sizeof(dir));

	ret = chdir(SYSFS_PATH);
	if (ret)
		goto null_blk;

	nvmedir = opendir(".");
	if (unlikely(!nvmedir))
		return -errno;

	for_each_dir(entry, nvmedir) {
		sprintf(path, "%s/" SYSFS_TRANSPORT, entry->d_name);
		fd = fopen(path, "r");
		if (!fd)
			continue;

		fgets(buf, sizeof(buf), fd);
		if (strncmp(SYSFS_PCIE, buf, sizeof(buf)) == 0) {
			subdir = opendir(entry->d_name);
			if (unlikely(!subdir)) {
				fclose(fd);
				continue;
			}

			for_each_dir(subentry, subdir)
				if (strncmp(subentry->d_name, SYSFS_PREFIX,
						SYSFS_PREFIX_LEN) == 0) {
					ret = create_device(subentry->d_name);
					if (ret)
						goto out;
					cnt++;
				}
			closedir(subdir);
		}
		fclose(fd);
	}
	closedir(nvmedir);

null_blk:
	ret = access(NULL_BLK_DEVICE, R_OK | W_OK);
	if (!ret) {
		ret = create_device(NULL);
		if (ret)
			goto out;
		cnt++;
	}
out:
	chdir(dir);
	return cnt;
}

static struct ops cfgfs_ops = {
	.delete_subsys			= cfgfs_delete_subsys,
	.create_subsys			= cfgfs_create_subsys,
	.create_ns			= cfgfs_create_ns,
	.delete_ns			= cfgfs_delete_ns,
	.create_host			= cfgfs_create_host,
	.delete_host			= cfgfs_delete_host,
	.create_portid			= cfgfs_create_portid,
	.delete_portid			= cfgfs_delete_portid,
	.link_host_to_subsys		= cfgfs_link_host_to_subsys,
	.unlink_host_from_subsys	= cfgfs_unlink_host_from_subsys,
	.link_port_to_subsys		= cfgfs_link_port_to_subsys,
	.unlink_port_from_subsys	= cfgfs_unlink_port_from_subsys,
	.enumerate_devices		= cfgfs_enumerate_devices,
	.reset_config			= cfgfs_reset_config,
	.start_targets			= cfgfs_start_targets,
	.stop_targets			= cfgfs_stop_targets,
};

struct ops *cfgfs_register_ops(void)
{
	return &cfgfs_ops;
}
