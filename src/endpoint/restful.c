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

#include <mongoose.h>
#include <jansson.h>

#include "common.h"
#include "tags.h"

static const struct mg_str s_get_method = MG_MK_STR("GET");
static const struct mg_str s_post_method = MG_MK_STR("POST");
static const struct mg_str s_delete_method = MG_MK_STR("DELETE");

#define MIN_PORTID			1
#define MAX_PORTID			0xfffe

#define HTTP_HDR			"HTTP/1.1"
#define SMALL_RSP			128
#define LARGE_RSP			512
#define BODY_SZ				1024

#define HTTP_OK				200
#define HTTP_ERR_NOT_FOUND		400
#define HTTP_ERR_PAGE_NOT_FOUND		404
#define HTTP_ERR_CONFLICT		409
#define HTTP_ERR_INTERNAL		500
#define HTTP_ERR_NOT_IMPLEMENTED	501

#define JSTAG				"\"%s\":"
#define JSSTR				"\"%s\":\"%s\""
#define JSINT				"\"%s\":%d"

static int is_equal(const struct mg_str *s1, const struct mg_str *s2)
{
	return s1->len == s2->len && memcmp(s1->p, s2->p, s2->len) == 0;
}

static inline int http_error(int err)
{
	if (err == 0)
		return 0;

	if (err == -ENOENT)
		return HTTP_ERR_NOT_FOUND;

	if (err == -EEXIST)
		return HTTP_ERR_CONFLICT;

	return HTTP_ERR_INTERNAL;
}

static inline const char *http_error_str(int err)
{
	if (err == HTTP_ERR_NOT_FOUND)
		return "Bad Request";

	if (err == HTTP_ERR_CONFLICT)
		return "Conflict";

	if (err == HTTP_ERR_PAGE_NOT_FOUND)
		return "Not Found";

	if (err == HTTP_ERR_NOT_IMPLEMENTED)
		return "Not Implemented";

	return "Internal Server Error";
}

static int bad_request(char *resp)
{
	strcpy(resp, "Method Not Implemented");

	return HTTP_ERR_NOT_IMPLEMENTED;
}

static int parse_uri(char *p, int depth, char *part[])
{
	int			 i = -1;

	depth++;
	for (; depth && *p; p++)
		if (*p == '/') {
			*p = '\0';
			i++, depth--;
			if (depth)
				part[i] = &p[1];
		}

	return i;
}

static void get_nsdev(char *resp)
{
	struct nsdev		*dev;
	int			 len;
	int			 once = 0;

	len = sprintf(resp, "{ " JSTAG " [", TAG_NSDEVS);
	resp += len;

	list_for_each_entry(dev, devices, node) {
		len = sprintf(resp, "%s{", once ? "," : "");
		resp += len;
		len = sprintf(resp, JSINT ",", TAG_DEVID, dev->devid);
		resp += len;
		len = sprintf(resp, JSINT "}", TAG_DEVNSID, dev->nsid);
		resp += len;
		once = 1;
	}

	sprintf(resp, "]}");
}

static void get_interface(char *resp)
{
	struct portid		*iface;
	int			 len;
	int			 once = 0;

	len = sprintf(resp, "{ " JSTAG " [", TAG_INTERFACES);
	resp += len;

	list_for_each_entry(iface, interfaces, node) {
		len = sprintf(resp, "%s{", once ? "," : "");
		resp += len;
		len = sprintf(resp, JSSTR ",", TAG_TYPE, iface->type);
		resp += len;
		len = sprintf(resp, JSSTR ",", TAG_FAMILY, iface->family);
		resp += len;
		len = sprintf(resp, JSSTR "}", TAG_ADDRESS, iface->address);
		resp += len;
		once = 1;
	}

	sprintf(resp, "]}");
}

static int redfish_get_request(char *p[], int n, char *resp)
{
	if (n != 1)
		goto bad;

	if (strcmp(p[0], URI_NSDEV) == 0)
		get_nsdev(resp);
	else if (strcmp(p[0], URI_INTERFACE) == 0)
		get_interface(resp);
	else
		goto bad;

	return 0;
bad:
	return bad_request(resp);
}

static int get_request(char *p[], int n, char *resp)
{
	if (n != 1)
		goto bad;

	if (strcmp(p[0], URI_NSDEV) == 0)
		get_nsdev(resp);
	else if (strcmp(p[0], URI_INTERFACE) == 0)
		get_interface(resp);
	else
		goto bad;

	return 0;
bad:
	return bad_request(resp);
}

static int post_portid(char *port, struct mg_str *body, char *resp)
{
	json_t			*new;
	json_t			*obj;
	json_error_t		 error;
	char			*data;
	char			*fam;
	char			*typ;
	char			*addr;
	int			 portid;
	int			 treq = 0;
	int			 svcid;
	int			 ret = 0;

	portid = atoi(port);
	if (portid < MIN_PORTID || portid > MAX_PORTID) {
		sprintf(resp, "invalid data");
		return http_error(-EINVAL);
	}

	data = malloc(body->len + 1);
	if (!data) {
		strcpy(resp, "No memory!");
		return http_error(-ENOMEM);
	}
	strncpy(data, body->p, body->len);
	data[body->len] = 0;

	new = json_loads(data, JSON_DECODE_ANY, &error);
	if (!new) {
		free(data);
		sprintf(resp, "invalid json syntax");
		return http_error(-EINVAL);
	}

	obj = json_object_get(new, TAG_TREQ);
	if (obj)
		treq = json_integer_value(obj) ? 1 : 0;

	obj = json_object_get(new, TAG_TYPE);
	if (!obj)
		goto err;

	typ = (char *) json_string_value(obj);

	obj = json_object_get(new, TAG_FAMILY);
	if (!obj)
		goto err;

	fam = (char *) json_string_value(obj);

	obj = json_object_get(new, TAG_ADDRESS);
	if (!obj)
		goto err;

	addr = (char *) json_string_value(obj);

	obj = json_object_get(new, TAG_TRSVCID);
	if (!obj)
		goto err;

	svcid = json_integer_value(obj);

	if (ops->create_portid(portid, fam, typ, treq, addr, svcid))
		goto err;

	sprintf(resp, "Configured portid %d", portid);
	goto out;
err:
	sprintf(resp, "invalid data");
	ret = http_error(-EINVAL);
out:
	json_decref(new);
	free(data);

	return ret;
}

static int post_subsys(char *subsys, struct mg_str *body, char *resp)
{
	json_t			*new;
	json_t			*obj;
	json_error_t		 error;
	char			*data;
	int			 allowany = 0;
	int			 ret = 0;

	data = malloc(body->len + 1);
	if (!data) {
		strcpy(resp, "No memory!");
		return http_error(-ENOMEM);
	}
	strncpy(data, body->p, body->len);
	data[body->len] = 0;

	new = json_loads(data, JSON_DECODE_ANY, &error);
	if (!new) {
		free(data);
		sprintf(resp, "invalid json syntax");
		return http_error(-EINVAL);
	}

	obj = json_object_get(new, TAG_SUBNQN);
	if (obj && !subsys)
		subsys = (char *) json_string_value(obj);
	if (!subsys)
		goto err;

	obj = json_object_get(new, TAG_ALLOW_ANY);
	if (obj)
		allowany = json_integer_value(obj) ? 1 : 0;

	if (ops->create_subsys(subsys, allowany))
		goto err;

	sprintf(resp, "Configured subsys %s", subsys);
	goto out;
err:
	sprintf(resp, "invalid data");
	ret = http_error(-EINVAL);
out:
	json_decref(new);
	free(data);

	return ret;
}

static int post_ns(char *subsys, char *ns, struct mg_str *body, char *resp)
{
	json_t			*new;
	json_t			*obj;
	json_error_t		 error;
	char			*data;
	int			 nsid;
	int			 devid;
	int			 devnsid;
	int			 ret = 0;

	data = malloc(body->len + 1);
	if (!data) {
		strcpy(resp, "No memory!");
		return http_error(-ENOMEM);
	}
	strncpy(data, body->p, body->len);
	data[body->len] = 0;

	new = json_loads(data, JSON_DECODE_ANY, &error);
	if (!new) {
		free(data);
		sprintf(resp, "invalid json syntax");
		return http_error(-EINVAL);
	}

	obj = json_object_get(new, TAG_NSID);
	if (obj && !ns)
		nsid = json_integer_value(obj);
	else if (ns)
		nsid = atoi(ns);
	else
		goto err;

	obj = json_object_get(new, TAG_DEVID);
	if (!obj)
		goto err;
	devid = json_integer_value(obj);

	obj = json_object_get(new, TAG_DEVNSID);
	if (!obj && devid != NULLB_DEVID)
		goto err;
	if (obj)
		devnsid = json_integer_value(obj);
	else
		devnsid = 0;

	if (ops->create_ns(subsys, nsid, devid, devnsid))
		goto err;

	sprintf(resp, "Configured nsid %d", nsid);
	goto out;
err:
	sprintf(resp, "invalid data");
	ret = http_error(-EINVAL);
out:
	json_decref(new);
	free(data);

	return ret;
}

static int post_host(char *host, struct mg_str *body, char *resp)
{
	json_t			*new = NULL;
	json_t			*obj;
	json_error_t		 error;
	char			*data;
	int			 ret = 0;

	if (!host) {
		data = malloc(body->len + 1);
		if (!data) {
			strcpy(resp, "No memory!");
			return http_error(-ENOMEM);
		}
		strncpy(data, body->p, body->len);
		data[body->len] = 0;

		new = json_loads(data, JSON_DECODE_ANY, &error);
		if (!new) {
			free(data);
			sprintf(resp, "invalid json syntax");
			return http_error(-EINVAL);
		}

		obj = json_object_get(new, TAG_HOSTNQN);
		if (!obj)
			goto err;

		host = (char *) json_string_value(obj);
	}

	if (ops->create_host(host))
		goto err;

	sprintf(resp, "Configured host %s", host);
	goto out;
err:
	sprintf(resp, "invalid data");
	ret = http_error(-EINVAL);
out:
	if (new) {
		json_decref(new);
		free(data);
	}

	return ret;
}

static int link_host(char *subsys, struct mg_str *body, char *resp)
{
	json_t			*new = NULL;
	json_t			*obj;
	json_error_t		 error;
	char			*data;
	char			*host;
	int			 ret = 0;

	data = malloc(body->len + 1);
	if (!data) {
		strcpy(resp, "No memory!");
		return http_error(-ENOMEM);
	}
	strncpy(data, body->p, body->len);
	data[body->len] = 0;

	new = json_loads(data, JSON_DECODE_ANY, &error);
	if (!new) {
		free(data);
		sprintf(resp, "invalid json syntax");
		return http_error(-EINVAL);
	}

	obj = json_object_get(new, TAG_HOSTNQN);
	if (!obj)
		goto err;

	host = (char *) json_string_value(obj);

	ret = ops->link_host_to_subsys(subsys, host);
	if (ret)
		goto err;

	sprintf(resp, "Configured host %s for subsys %s", host, subsys);
	goto out;
err:
	sprintf(resp, "invalid data");
	ret = http_error(-EINVAL);
out:
	json_decref(new);
	free(data);

	return ret;
}

static int link_portid(char *subsys, struct mg_str *body, char *resp)
{
	json_t			*new = NULL;
	json_t			*obj;
	json_error_t		 error;
	char			*data;
	int			 portid;
	int			 ret = 0;

	data = malloc(body->len + 1);
	if (!data) {
		strcpy(resp, "No memory!");
		return http_error(-ENOMEM);
	}
	strncpy(data, body->p, body->len);
	data[body->len] = 0;

	new = json_loads(data, JSON_DECODE_ANY, &error);
	if (!new) {
		free(data);
		sprintf(resp, "invalid json syntax");
		return http_error(-EINVAL);
	}

	obj = json_object_get(new, TAG_PORTID);
	if (!obj)
		goto err;

	portid = json_integer_value(obj);

	ret = ops->link_port_to_subsys(subsys, portid);
	if (ret)
		goto err;

	sprintf(resp, "Enabled subsys %s on portid %d", subsys, portid);
	goto out;
err:
	sprintf(resp, "invalid data");
	ret = http_error(-EINVAL);
out:
	json_decref(new);
	free(data);

	return ret;
}

static int post_request(char *p[], int n, struct mg_str *body, char *resp)
{
	int			 ret;

	if (!body->len) {
		if (n == 1 && strcmp(p[0], METHOD_SHUTDOWN) == 0) {
			shutdown_dem();
			strcpy(resp, "DEM Endpoint Config shutting down");
			ret = 0;
		} else
			goto bad;
	} else if (n == 1) {
		if (strcmp(p[0], URI_SUBSYSTEM) == 0)
			ret = post_subsys(NULL, body, resp);
		else if (strcmp(p[0], URI_PORTID) == 0)
			ret = post_portid(NULL, body, resp);
		else if (strcmp(p[0], URI_HOST) == 0)
			ret = post_host(NULL, body, resp);
		else
			goto bad;
	} else if (n == 2) {
		if (strcmp(p[0], URI_SUBSYSTEM) == 0)
			ret = post_subsys(p[1], body, resp);
		else if (strcmp(p[0], URI_PORTID) == 0)
			ret = post_portid(p[1], body, resp);
		else
			goto bad;
	} else if (n == 3) {
		if (strcmp(p[0], URI_SUBSYSTEM))
			goto bad;
		else if (strcmp(p[2], URI_NAMESPACE) == 0)
			ret = post_ns(p[1], NULL, body, resp);
		else if (strcmp(p[2], URI_HOST) == 0)
			ret = link_host(p[1], body, resp);
		else if (strcmp(p[2], URI_PORTID) == 0)
			ret = link_portid(p[1], body, resp);
		else
			goto bad;
	} else if (n == 4) {
		if (strcmp(p[0], URI_SUBSYSTEM))
			goto bad;
		else if (strcmp(p[2], URI_NAMESPACE))
			goto bad;
		else
			ret = post_ns(p[1], p[3], body, resp);
	} else
		goto bad;

	return ret;
bad:
	return bad_request(resp);
}

static int del_portid(char *port, char *resp)
{
	int			 ret = 0;

	ret = ops->delete_portid(atoi(port));
	if (ret)
		sprintf(resp, "Unable to delete portid %s", port);
	else
		sprintf(resp, "Deleted portid %s", port);

	return ret;
}

static int del_subsys(char *subsys, char *resp)
{
	int			 ret = 0;

	ret = ops->delete_subsys(subsys);
	if (ret)
		sprintf(resp, "Unable to delete subsys %s", subsys);
	else
		sprintf(resp, "Deleted subsys %s", subsys);

	return ret;
}

static int del_ns(char *subsys, char *ns, char *resp)
{
	int			 ret = 0;

	ret = ops->delete_ns(subsys, atoi(ns));
	if (ret)
		sprintf(resp, "Unable to delete ns %s from subsys %s",
			ns, subsys);
	else
		sprintf(resp, "Deleted ns %s from subsys %s", ns, subsys);

	return ret;
}

static int del_host(char *host, char *resp)
{
	int			 ret = 0;

	ret = ops->delete_host(host);
	if (ret)
		sprintf(resp, "Unable to delete host %s", host);
	else
		sprintf(resp, "Delete host %s", host);

	return ret;
}

static int unlink_host(char *subsys, char *host, char *resp)
{
	int			 ret = 0;

	ret = ops->unlink_host_from_subsys(subsys, host);
	if (ret)
		sprintf(resp, "Unable to delete host %s from subsys %s",
			host, subsys);
	else
		sprintf(resp, "Deleted host %s from subsys %s",
			host, subsys);

	return ret;
}

static int unlink_portid(char *subsys, char *portid, char *resp)
{
	int			 ret = 0;

	ret = ops->unlink_port_from_subsys(subsys, atoi(portid));
	if (ret)
		sprintf(resp, "Unable to delete port %s from subsys %s",
			portid, subsys);
	else
		sprintf(resp, "Deleted port %s from subsys %s", portid, subsys);

	return ret;
}

static int delete_request(char *p[], int n, char *resp)
{
	int			 ret = 0;

	if (n == 1 && strcmp(p[0], URI_CONFIG) == 0) {
		ops->reset_config();
		strcpy(resp, "Target configuration deleted");
	} else if (n == 2) {
		if (strcmp(p[0], URI_SUBSYSTEM) == 0)
			ret = del_subsys(p[1], resp);
		else if (strcmp(p[0], URI_PORTID) == 0)
			ret = del_portid(p[1], resp);
		else if (strcmp(p[0], URI_HOST) == 0)
			ret = del_host(p[1], resp);
		else
			goto bad;
	} else if (n == 4) {
		if (strcmp(p[0], URI_SUBSYSTEM))
			goto bad;
		else if (strcmp(p[2], URI_NAMESPACE) == 0)
			ret = del_ns(p[1], p[3], resp);
		else if (strcmp(p[2], URI_PORTID) == 0)
			ret = unlink_portid(p[1], p[3], resp);
		else if (strcmp(p[2], URI_HOST) == 0)
			ret = unlink_host(p[1], p[3], resp);
		else
			goto bad;
	} else
		goto bad;

	return ret;
bad:
	return bad_request(resp);
}

static int handle_redfish_requests(char *p[], int n, struct http_message *hm,
					char *resp)
{
	int			 ret = 0;

	if (is_equal(&hm->method, &s_get_method))
		ret = redfish_get_request(p, n, resp);
//	else if (is_equal(&hm->method, &s_post_method))
//		ret = redfish_post_request(p, n, &hm->body, resp);
//	else if (is_equal(&hm->method, &s_delete_method))
//		ret = redfish_delete_request(p, n, resp);
	else
		ret = bad_request(resp);

	return ret;
}
static int handle_target_requests(char *p[], int n, struct http_message *hm,
					char *resp)
{
	int			 ret;

	if (is_equal(&hm->method, &s_get_method))
		ret = get_request(p, n, resp);
	else if (is_equal(&hm->method, &s_post_method))
		ret = post_request(p, n, &hm->body, resp);
	else if (is_equal(&hm->method, &s_delete_method))
		ret = delete_request(p, n, resp);
	else
		ret = bad_request(resp);

	return ret;
}

#define MAX_DEPTH 8

void handle_http_request(struct mg_connection *c, void *ev_data)
{
	struct http_message	*hm = (struct http_message *) ev_data;
	char			*resp = NULL;
	char			*uri = NULL;
	char			*parts[MAX_DEPTH] = { NULL };
	int			 ret;
	int			 n;

	if (!hm->uri.len) {
		ret = HTTP_ERR_PAGE_NOT_FOUND;
		goto out;
	}

	resp = malloc(BODY_SZ);
	if (!resp) {
		ret = HTTP_ERR_INTERNAL;
		goto out;
	}

	print_debug("%.*s %.*s", (int) hm->method.len, hm->method.p,
			(int) hm->uri.len, hm->uri.p);

	if (hm->body.len)
		print_debug("%.*s", (int) hm->body.len, hm->body.p);

	memset(resp, 0, BODY_SZ);

	uri = malloc(hm->uri.len + 1);
	if (!uri) {
		strcpy(resp, "No memory!");
		ret = HTTP_ERR_INTERNAL;
		goto out;
	}
	memcpy(uri, (char *) hm->uri.p, hm->uri.len);
	uri[hm->uri.len] = 0;

	n = parse_uri(uri, MAX_DEPTH, parts);
	if (n < 0) {
		sprintf(resp, "Bad page %.*s", (int) hm->uri.len, hm->uri.p);
		ret = HTTP_ERR_PAGE_NOT_FOUND;
		goto out;
	}

	ret = handle_target_requests(parts, n+1, hm, resp);
out:
	if (!ret)
		mg_printf(c, "%s %d OK", HTTP_HDR, HTTP_OK);
	else
		mg_printf(c, "%s %d %s", HTTP_HDR, ret, http_error_str(ret));

	mg_printf(c, "\r\nContent-Type: plain/text");
	if (resp) {
		mg_printf(c, "\r\nContent-Length: %ld\r\n", strlen(resp));
		mg_printf(c, "\r\n%s\r\n\r\n", resp);
	} else {
		mg_printf(c, "\r\nContent-Length: 14\r\n");
		mg_printf(c, "\r\nInternal Error\r\n\r\n");
	}

	if (uri)
		free(uri);
	if (resp)
		free(resp);

	c->flags = MG_F_SEND_AND_CLOSE;
}
