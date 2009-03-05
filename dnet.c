/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _XOPEN_SOURCE		1024
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "elliptics.h"
#include "packet.h"
#include "interface.h"

static int dnet_transform(struct dnet_node *n, void *src, __u64 size, void *dst, unsigned int *dsize, int *ppos)
{
	int pos = 0;
	int err = 1;
	struct dnet_transform *t;

	pthread_mutex_lock(&n->tlock);
	list_for_each_entry(t, &n->tlist, tentry) {
		if (pos++ == *ppos) {
			*ppos = pos;
			err = t->init(t->priv);
			if (err)
				continue;

			err = t->update(t->priv, src, size, dst, dsize, 0);
			if (err)
				continue;
			
			err = t->final(t->priv, dst, dsize, 0);
			if (!err)
				break;
		}
	}
	pthread_mutex_unlock(&n->tlock);

	return err;
}

static int dnet_transform_file(struct dnet_node *n, char *file, loff_t offset, size_t size,
		void *dst, unsigned int *dsize, int *ppos)
{
	int err = -ENOENT;
	int fd;
	void *data;

	fd = open(file, O_RDONLY | O_LARGEFILE);
	if (fd < 0) {
		err = -errno;
		ulog_err("%s: failed to open file '%s' for the transformation",
				el_dump_id(n->id), file);
		goto err_out_exit;
	}

	if (!size) {
		struct stat st;

		err = fstat(fd, &st);
		if (err < 0) {
			err = -errno;
			ulog_err("%s: failed to stat file '%s' for the transformation",
					el_dump_id(n->id), file);
			goto err_out_close;
		}

		size = st.st_size;
	}

	data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, offset);
	if (data == MAP_FAILED) {
		err = -errno;
		ulog_err("%s: failed to map file '%s' for the transformation",
				el_dump_id(n->id), file);
		goto err_out_close;
	}

	err = dnet_transform(n, data, size, dst, dsize, ppos);
	if (err)
		goto err_out_unmap;

	munmap(data, size);
	close(fd);

	return 0;

err_out_unmap:
	munmap(data, size);
err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static int dnet_cmd_lookup(struct dnet_net_state *orig, struct el_cmd *cmd,
		struct el_attr *attr __unused, void *data __unused)
{
	struct dnet_node *n = orig->n;
	struct dnet_net_state *st;
	struct el_addr_cmd l;
	int err;

	memset(&l, 0, sizeof(struct el_addr_cmd));

	st = dnet_state_search(n, cmd->id, NULL);
	if (!st)
		st = dnet_state_get(orig);

	memcpy(&l.cmd.id, st->id, EL_ID_SIZE);
	l.cmd.size = sizeof(struct el_addr_cmd) - sizeof(struct el_cmd);
	l.cmd.trans |= DNET_TRANS_REPLY;

	memcpy(&l.addr.addr, &st->addr, st->addr_len);
	l.addr.addr_len = st->addr_len;
	l.addr.proto = n->proto;
	l.addr.sock_type = n->sock_type;

	dnet_state_put(st);

	l.a.cmd = DNET_CMD_LOOKUP;
	l.a.size = sizeof(struct el_addr_cmd) - sizeof(struct el_cmd) - sizeof(struct el_attr);

	el_convert_addr_cmd(&l);

	pthread_mutex_lock(&orig->lock);
	err = dnet_send(orig, &l, sizeof(struct el_addr_cmd));
	pthread_mutex_unlock(&orig->lock);

	return err;
}

static int dnet_cmd_reverse_lookup(struct dnet_net_state *st, struct el_cmd *cmd,
		struct el_attr *attr __unused, void *data __unused)
{
	struct dnet_node *n = st->n;
	struct el_addr_cmd a;
	int err;

	memset(&a, 0, sizeof(struct el_addr_cmd));

	memcpy(&a.cmd.id, n->id, EL_ID_SIZE);
	a.cmd.trans = cmd->trans | DNET_TRANS_REPLY;
	a.cmd.size = sizeof(struct el_addr_cmd) - sizeof(struct el_cmd);

	a.a.cmd = DNET_CMD_REVERSE_LOOKUP;
	a.a.size = sizeof(struct el_addr_cmd) - sizeof(struct el_cmd) - sizeof(struct el_attr);

	memcpy(&a.addr.addr, &n->addr, n->addr_len);
	a.addr.addr_len = n->addr_len;
	a.addr.proto = n->proto;
	a.addr.sock_type = n->sock_type;

	el_convert_addr_cmd(&a);

	pthread_mutex_lock(&st->lock);
	err = dnet_send(st, &a, sizeof(struct el_addr_cmd));
	pthread_mutex_unlock(&st->lock);

	return err;
}

static int dnet_cmd_join_client(struct dnet_net_state *orig, struct el_cmd *cmd,
		struct el_attr *attr __unused, void *data)
{
	int err, s;
	struct dnet_net_state *st;
	struct dnet_node *n = orig->n;
	struct el_addr_attr *a = data;

	el_convert_addr_attr(a);

	s = dnet_socket_create_addr(a->sock_type, a->proto, &a->addr, a->addr_len, 0);
	if (s < 0) {
		err = s;
		goto err_out_exit;
	}

	st = dnet_state_create(n, cmd->id, &a->addr, a->addr_len, s, dnet_state_process);
	if (!st) {
		err = -EINVAL;
		goto err_out_close;
	}

#if 0
	memcpy(id, st->id, EL_ID_SIZE);

	pthread_mutex_lock(&st->lock);
	memcpy(st->id, cmd->id, EL_ID_SIZE);
	pthread_mutex_unlock(&st->lock);

	err =  dnet_state_move(st);
#endif
	ulog("%s: state %s:%d.\n", el_dump_id(cmd->id),
		el_server_convert_addr(&a->addr, a->addr_len),
		el_server_convert_port(&a->addr, a->addr_len));

	return 0;

err_out_close:
	close(s);
err_out_exit:
	ulog("%s: state %s:%d -> ", el_dump_id(cmd->id),
		el_server_convert_addr(&a->addr, a->addr_len),
		el_server_convert_port(&a->addr, a->addr_len));
	uloga("%s, err: %d.\n", el_dump_id(st->id), err);
	return err;
}

static int dnet_update_history(int md, unsigned char *id, struct el_io_attr *io, int tmp)
{
	char history[EL_ID_SIZE*2+1 + sizeof(EL_HISTORY_SUFFIX) + 5];
	int fd, err;

	snprintf(history, sizeof(history), "%s%s%s", el_dump_id(id), EL_HISTORY_SUFFIX, (tmp)?".tmp":"");

	fd = openat(md, history, O_RDWR | O_CREAT | O_APPEND | O_LARGEFILE, 0644);
	if (fd < 0) {
		err = -errno;
		ulog_err("%s: failed to open history file '%s'", el_dump_id(id), history);
		goto err_out_exit;
	}

	err = write(fd, io, sizeof(struct el_io_attr));
	if (err <= 0) {
		err = -errno;
		ulog_err("%s: failed to update history file '%s'", el_dump_id(id), history);
		goto err_out_close;
	}

	close(fd);
	return 0;

err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static int dnet_cmd_write(struct dnet_net_state *st, struct el_cmd *cmd,
		struct el_attr *attr, void *data)
{
	int err, dd, md;
	struct dnet_node *n = st->n;
	char dir[3];
	struct el_io_attr *io = data;
	int oflags = O_RDWR | O_CREAT | O_LARGEFILE;
	char file[EL_ID_SIZE * 2 + 1];
	
	if (attr->size <= sizeof(struct el_io_attr)) {
		ulog("%s: wrong write attribute, size does not match "
				"IO attribute size: size: %llu, must be more than %u.\n",
				el_dump_id(cmd->id), (unsigned long long)attr->size, sizeof(struct el_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	data += sizeof(struct el_io_attr);

	el_convert_io_attr(io);

	if ((io->size != attr->size - sizeof(struct el_io_attr)) ||
			(io->size > cmd->size)){
		ulog("%s: wrong io size: %llu, must be equal to %llu.\n",
				el_dump_id(cmd->id), io->size,
				(unsigned long long)attr->size - sizeof(struct el_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!n->root) {
		ulog("%s: can not write without root dir.\n", el_dump_id(cmd->id));
		err = -EINVAL;
		goto err_out_exit;
	}

	snprintf(dir, sizeof(dir), "%02x", cmd->id[0]);

	err = mkdirat(n->rootfd, dir, 0755);
	if (err < 0) {
		if (errno != EEXIST) {
			err = -errno;
			ulog_err("%s: faliled to create dir '%s' in the root '%s'",
					el_dump_id(cmd->id), dir, n->root);
			goto err_out_exit;
		}
	}

	md = openat(n->rootfd, dir, O_RDONLY);
	if (md < 0) {
		err = -errno;
		ulog_err("%s: failed to open dir '%s/%s'",
				el_dump_id(cmd->id), n->root, dir);
		goto err_out_exit;
	}

	snprintf(file, sizeof(file), "%s", el_dump_id(cmd->id));

	if (io->flags & DNET_IO_FLAGS_APPEND)
		oflags |= O_APPEND;

	dd = openat(md, file, oflags, 0644);
	if (dd < 0) {
		err = -errno;
		ulog_err("%s: failed to open data file '%s/%s/%s'",
				el_dump_id(cmd->id), n->root, dir, file);
		goto err_out_close_md;
	}

	err = pwrite(dd, data, io->size, io->offset);
	if (err <= 0) {
		err = -errno;
		ulog_err("%s: failed to write into '%s/%s/%s'",
			el_dump_id(cmd->id), n->root, dir, file);
		goto err_out_close_dd;
	}

	if (io->flags & DNET_IO_FLAGS_UPDATE) {
		err = dnet_update_history(md, cmd->id, io, 0);
		if (err) {
			ulog("%s: failed to update history for '%s/%s/%s'",
				el_dump_id(cmd->id), n->root, dir, file);
			goto err_out_close_dd;
		}
	}

	ulog("%s: IO file: '%s/%s/%s', offset: %llu, size: %llu.\n",
			el_dump_id(cmd->id), n->root, dir, file,
			io->offset, io->size);

	close(dd);
	close(md);

	return 0;

err_out_close_dd:
	close(dd);
err_out_close_md:
	close(md);
err_out_exit:
	return err;
}

static int dnet_cmd_read(struct dnet_net_state *st, struct el_cmd *cmd,
		struct el_attr *attr, void *data)
{
	struct dnet_node *n = st->n;
	struct el_io_attr *io = data;
	char file[EL_ID_SIZE * 2 + 1 + 4];
	int dd, err;
	struct el_cmd *c;
	struct el_attr *a;
	struct el_io_attr *rio;
	size_t size;
	loff_t offset;
	__u64 total_size;

	if (attr->size != sizeof(struct el_io_attr)) {
		ulog("%s: wrong read attribute, size does not match "
				"IO attribute size: size: %llu, must be: %u.\n",
				el_dump_id(cmd->id), (unsigned long long)attr->size,
				sizeof(struct el_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	el_convert_io_attr(io);

	snprintf(file, sizeof(file), "%02x/%s", io->id[0], el_dump_id(io->id));

	dd = openat(n->rootfd, file, O_RDONLY, 0644);
	if (dd < 0) {
		err = -errno;
		ulog_err("%s: failed to open data file '%s/%s'",
				el_dump_id(io->id), n->root, file);
		goto err_out_exit;
	}

	size = io->size;
	if (io->size == 0) {
		struct stat st;

		err = fstat(dd, &st);
		if (err) {
			err = -errno;
			ulog_err("%s: failed to stat file '%s/%s'", el_dump_id(io->id), n->root, file);
			goto err_out_close_dd;
		}

		size = st.st_size;
	}

	c = malloc(sizeof(struct el_cmd) + sizeof(struct el_attr) + sizeof(struct el_io_attr));
	if (!c) {
		ulog("%s: failed to allocate reply attributes.\n", el_dump_id(io->id));
		goto err_out_close_dd;
	}

	total_size = size;
	offset = io->offset;

	while (total_size) {
		size = total_size;
		if (size > MAX_READ_TRANS_SIZE)
			size = MAX_READ_TRANS_SIZE;

		ulog("%s: offset: %llu, size: %zu, c: %p.\n", el_dump_id(io->id),
				(unsigned long long)io->offset, size, c);

		a = (struct el_attr *)(c + 1);
		rio = (struct el_io_attr *)(a + 1);

		memcpy(c->id, io->id, EL_ID_SIZE);
		c->flags = DNET_FLAGS_MORE;
		c->status = 0;
		c->size = sizeof(struct el_attr) + sizeof(struct el_io_attr) + size;
		c->trans = cmd->trans | DNET_TRANS_REPLY;

		a->cmd = DNET_CMD_READ;
		a->size = sizeof(struct el_io_attr) + size;
		a->flags = 0;

		memcpy(rio->id, io->id, EL_ID_SIZE);
		rio->size = size;
		rio->offset = offset;
		rio->flags = 0;

		el_convert_cmd(c);
		el_convert_attr(a);
		el_convert_io_attr(rio);

		err = dnet_sendfile_data(st, file, dd, offset, size,
			c, sizeof(struct el_cmd) + sizeof(struct el_attr) + sizeof(struct el_io_attr));
		if (err) {
			ulog("%s: failed to send read reply.\n", el_dump_id(io->id));
			goto err_out_free;
		}

		offset += size;
		total_size -= size;
	}

	free(c);
	close(dd);

	return 0;

err_out_free:
	free(c);
err_out_close_dd:
	close(dd);
err_out_exit:
	return err;
}

int dnet_process_cmd(struct dnet_net_state *st, struct el_cmd *cmd, void *data)
{
	int err;
	unsigned long long size = cmd->size;

	while (size) {
		struct el_attr *a = data;
		unsigned int sz;

		el_convert_attr(a);
		sz = a->size;

		if (size < sizeof(struct el_attr)) {
			ulog("%s: 1 wrong cmd: size: %llu/%llu, attr_size: %u.\n",
					el_dump_id(st->id), (unsigned long long)cmd->size, size, sz);
			err = -EPROTO;
			break;
		}

		data += sizeof(struct el_attr);
		size -= sizeof(struct el_attr);
		
		if (size < a->size) {
			ulog("%s: 2 wrong cmd: size: %llu/%llu, attr_size: %u.\n",
				el_dump_id(st->id), (unsigned long long)cmd->size, size, sz);
			err = -EPROTO;
			break;
		}

		ulog("%s: trans: %llu, size_left: %llu, starting cmd: %u, asize: %llu.\n",
			el_dump_id(cmd->id), cmd->trans, size, a->cmd, (unsigned long long)a->size);

		switch (a->cmd) {
			case DNET_CMD_LOOKUP:
				err = dnet_cmd_lookup(st, cmd, a, data);
				break;
			case DNET_CMD_REVERSE_LOOKUP:
				err = dnet_cmd_reverse_lookup(st, cmd, a, data);
				break;
			case DNET_CMD_JOIN:
				err = dnet_cmd_join_client(st, cmd, a, data);
				break;
			case DNET_CMD_WRITE:
				err = dnet_cmd_write(st, cmd, a, data);
				break;
			case DNET_CMD_READ:
				err = dnet_cmd_read(st, cmd, a, data);
				break;
			case DNET_CMD_LIST:
				err = dnet_cmd_list(st, cmd);
				break;
			default:
				err = -EPROTO;
				break;
		}

		ulog("%s: trans: %llu, size_left: %llu, completed cmd: %u, asize: %llu, err: %d.\n",
			el_dump_id(cmd->id), cmd->trans, size, a->cmd, (unsigned long long)a->size, err);

		if (err)
			break;

		if (size < sz) {
			ulog("%s: 3 wrong cmd: size: %llu/%llu, attr_size: %u.\n",
				el_dump_id(st->id), (unsigned long long)cmd->size, size, sz);
			err = -EPROTO;
			break;
		}

		data += sz;
		size -= sz;
	}

	if (cmd->flags & DNET_FLAGS_NEED_ACK) {
		struct el_cmd ack;

		memcpy(ack.id, cmd->id, EL_ID_SIZE);
		ack.trans = cmd->trans | DNET_TRANS_REPLY;
		ack.size = 0;
		ack.flags = cmd->flags;
		ack.status = err;

		el_convert_cmd(&ack);

		pthread_mutex_lock(&st->lock);
		dnet_send(st, &ack, sizeof(struct el_cmd));
		pthread_mutex_unlock(&st->lock);
	}

	return err;
}

int dnet_add_state(struct dnet_node *n, struct dnet_config *cfg)
{
	int s, err;
	struct sockaddr sa;
	int addr_len = sizeof(struct sockaddr);
	struct dnet_net_state *st, dummy;
	char buf[sizeof(struct el_cmd) + sizeof(struct el_attr)];
	struct el_addr_cmd acmd;
	struct el_cmd *cmd;
	struct el_attr *a;

	s = dnet_socket_create(cfg, &sa, &addr_len, 0);
	if (s < 0) {
		err = s;
		goto err_out_exit;
	}

	memset(buf, 0, sizeof(buf));

	cmd = (struct el_cmd *)(buf);
	a = (struct el_attr *)(cmd + 1);

	cmd->size = sizeof(struct el_attr);
	a->cmd = DNET_CMD_REVERSE_LOOKUP;

	el_convert_cmd(cmd);
	el_convert_attr(a);

	st = &dummy;
	memset(st, 0, sizeof(struct dnet_net_state));

	st->s = s;
	st->n = n;
	st->timeout = DNET_TIMEOUT;

	err = dnet_send(st, buf, sizeof(buf));
	if (err) {
		ulog_err("%s: failed to send reverse lookup message to %s:%d, err: %d",
				el_dump_id(n->id),
				el_server_convert_addr(&sa, addr_len),
				el_server_convert_port(&sa, addr_len), err);
		goto err_out_sock_close;
	}

	err = dnet_recv(st, &acmd, sizeof(acmd));
	if (err < 0) {
		ulog_err("%s: failed to receive reverse lookup response from %s:%d, err: %d",
				el_dump_id(n->id),
				el_server_convert_addr(&sa, addr_len),
				el_server_convert_port(&sa, addr_len), err);
		goto err_out_sock_close;
	}

	el_convert_addr_cmd(&acmd);

	ulog("%s: reverse lookup: ", el_dump_id(n->id));
	uloga("%s -> %s:%d.\n", el_dump_id(acmd.cmd.id),
		el_server_convert_addr(&acmd.addr.addr, acmd.addr.addr_len),
		el_server_convert_port(&acmd.addr.addr, acmd.addr.addr_len));

	st = dnet_state_create(n, acmd.cmd.id, &acmd.addr.addr, acmd.addr.addr_len, s, dnet_state_process);
	if (!st)
		goto err_out_sock_close;

	return 0;

err_out_sock_close:
	close(s);
err_out_exit:
	return err;
}

int dnet_join(struct dnet_node *n)
{
	struct el_addr_cmd a;
	int err = 0;
	struct dnet_net_state *st;

	if (!n->root) {
		ulog("%s: can not join without root directory to store data.\n", el_dump_id(n->id));
		return -EINVAL;
	}

	/*
	 * Need to sync local content.
	 */
	err = dnet_recv_list(n);
	if (err) {
		ulog("%s: content sync failed, error: %d.\n", el_dump_id(n->id), err);
		return err;
	}

	memset(&a, 0, sizeof(struct el_addr_cmd));

	memcpy(a.cmd.id, n->id, EL_ID_SIZE);
	a.cmd.size = sizeof(struct el_addr_cmd) - sizeof(struct el_cmd);

	a.a.cmd = DNET_CMD_JOIN;
	a.a.size = sizeof(struct el_addr_cmd) - sizeof(struct el_cmd) - sizeof(struct el_attr);

	memcpy(&a.addr.addr, &n->addr, n->addr_len);
	a.addr.addr_len = n->addr_len;
	a.addr.sock_type = n->sock_type;
	a.addr.proto = n->proto;

	el_convert_addr_cmd(&a);

	pthread_mutex_lock(&n->state_lock);
	list_for_each_entry(st, &n->state_list, state_entry) {
		if (st == n->st)
			continue;

		err = dnet_send(st, &a, sizeof(struct el_addr_cmd));
		if (err) {
			ulog("%s: failed to update state", el_dump_id(n->id));
			uloga(" %s -> %s:%d.\n", el_dump_id(st->id),
				el_server_convert_addr(&st->addr, st->addr_len),
				el_server_convert_port(&st->addr, st->addr_len));
			break;
		}
	}
	pthread_mutex_unlock(&n->state_lock);

	return err;
}

int dnet_setup_root(struct dnet_node *n, char *root)
{
	if (n->root) {
		free(n->root);
		close(n->rootfd);
	}

	n->root = strdup(root);
	if (!n->root) {
		ulog("%s: failed to duplicate root string '%s'.\n", el_dump_id(n->id), root);
		return -ENOMEM;
	}

	n->rootfd = open(n->root, O_RDONLY);
	if (n->rootfd < 0) {
		int err = -errno;
		ulog_err("%s: failed to open root '%s' for writing", el_dump_id(n->id), root);
		n->rootfd = 0;
		return err;
	}

	return 0;
}

static int dnet_write_complete(struct dnet_net_state *st __unused, struct el_cmd *cmd,
		struct el_attr *attr __unused, void *priv)
{
	char *file = priv;

	ulog("%s: completed: file: '%s', status: %d.\n",
		el_dump_id(cmd->id), file, cmd->status);

	return cmd->status;
}

static struct dnet_trans *dnet_io_trans_create(struct dnet_node *n, unsigned char *id,
		int attr_cmd, struct el_io_attr *ioattr,
		int (* complete)(struct dnet_net_state *, struct el_cmd *, struct el_attr *, void *),
		void *priv)
{
	struct dnet_trans *t;
	int err;
	struct el_attr *a;
	struct el_io_attr *io;
	struct el_cmd *cmd;
	__u64 size = ioattr->size;

	t = malloc(sizeof(struct dnet_trans) +
			sizeof(struct el_attr) +
			sizeof(struct el_io_attr) +
			sizeof(struct el_cmd));
	if (!t) {
		err = -ENOMEM;

		if (complete)
			complete(NULL, NULL, NULL, priv);
		free(priv);
		goto err_out_exit;
	}
	t->data = NULL;
	t->st = NULL;
	t->complete = complete;
	t->priv = priv;

	cmd = (struct el_cmd *)(t + 1);
	a = (struct el_attr *)(cmd + 1);
	io = (struct el_io_attr *)(a + 1);

	memcpy(cmd->id, id, EL_ID_SIZE);
	cmd->size = sizeof(struct el_attr) + sizeof(struct el_io_attr) + size;
	cmd->flags = DNET_FLAGS_NEED_ACK;
	cmd->status = 0;

	a->cmd = attr_cmd;
	a->size = sizeof(struct el_io_attr) + size;
	a->flags = 0;

	memcpy(io, ioattr, sizeof(struct el_io_attr));

	t->st = dnet_state_get_first(n, n->st);
	if (!t->st) {
		ulog("%s: failed to find a state.\n", el_dump_id(cmd->id));
		goto err_out_destroy;
	}

	err = dnet_trans_insert(t);
	if (err)
		goto err_out_destroy;

	cmd->trans = t->trans;
	el_convert_cmd(cmd);
	el_convert_attr(a);
	el_convert_io_attr(io);

	return t;
	
err_out_destroy:
	dnet_trans_destroy(t);
err_out_exit:
	return NULL;
}

int dnet_write_file(struct dnet_node *n, char *file)
{
	int fd, err, pos = 0, len = strlen(file);
	struct dnet_trans *t;
	struct el_io_attr io;
	unsigned char file_id[EL_ID_SIZE];
	struct stat stat;
	off_t size;
	struct dnet_net_state *st;
	int error = -ENOENT;

	fd = openat(n->rootfd, file, O_RDONLY | O_LARGEFILE);
	if (fd < 0) {
		err = -errno;
		ulog_err("%s: failed to open file '%s'", el_dump_id(n->id), file);
		goto err_out_exit;
	}

	err = fstatat(n->rootfd, file, &stat, AT_SYMLINK_NOFOLLOW);
	if (err) {
		err = -errno;
		ulog_err("%s: failed to stat file '%s'", el_dump_id(n->id), file);
		goto err_out_close;
	}

	size = stat.st_size;

	while (1) {
		unsigned int rsize = EL_ID_SIZE;

		err = dnet_transform(n, file, len, file_id, &rsize, &pos);
		if (err) {
			if (err > 0)
				break;
			continue;
		}
		pos--;

		rsize = EL_ID_SIZE;
		err = dnet_transform_file(n, file, 0, 0, io.id, &rsize, &pos);
		if (err)
			continue;

		io.size = size;
		io.offset = 0;
		io.flags = DNET_IO_FLAGS_UPDATE;

		t = dnet_io_trans_create(n, file_id, DNET_CMD_WRITE, &io, dnet_write_complete, strdup(file));
		if (!t) {
			ulog("%s: failed to create transaction.\n", el_dump_id(io.id));
			continue;
		}

		st = t->st;
		ulog("file: '%s' -> %s.\n", file, el_dump_id(st->id));

		err = dnet_sendfile_data(st, file, fd, 0, size,
			t+1, sizeof(struct el_attr) + sizeof(struct el_io_attr) + sizeof(struct el_cmd));
		if (!err)
			error = 0;
	}

	ulog("%s: file: '%s', size: %lu.\n", el_dump_id(io.id), file, stat.st_size);

	close(fd);

	return error;

err_out_close:
	close(fd);
err_out_exit:
	return err;
}

int dnet_write_object(struct dnet_node *n, unsigned char *id, struct el_io_attr *io,
	int (* complete)(struct dnet_net_state *, struct el_cmd *, struct el_attr *, void *),
	void *priv, void *data)
{
	struct dnet_trans *t;
	struct dnet_net_state *st;
	int err;

	t = dnet_io_trans_create(n, id, DNET_CMD_WRITE, io, complete, priv);
	if (!t) {
		ulog("%s: failed to create transaction.\n", el_dump_id(id));
		goto err_out_exit;
	}

	st = t->st;

	ulog("Write transaction: object: %s ", el_dump_id(id));
	uloga("update: %s, offset: %llu, size: %llu -> ", el_dump_id(io->id), io->offset, io->size);
	uloga("%s.\n", el_dump_id(st->id));

	pthread_mutex_lock(&st->lock);

	err = dnet_send(st, t+1, sizeof(struct el_attr) + sizeof(struct el_io_attr) + sizeof(struct el_cmd));
	if (err)
		goto err_out_unlock;

	err = dnet_send(st, data, io->size);
	if (err)
		goto err_out_unlock;

	pthread_mutex_unlock(&st->lock);

	return 0;

err_out_unlock:
	pthread_mutex_unlock(&st->lock);
	dnet_trans_destroy(t);
err_out_exit:
	return err;
}

int dnet_update_file(struct dnet_node *n, char *file, off_t offset, void *data, unsigned int size, int append)
{
	unsigned char id[EL_ID_SIZE];
	struct el_io_attr io;
	int err, pos = 0;
	int len = strlen(file);
	int error = -ENOENT;

	while (1) {
		unsigned int rsize = EL_ID_SIZE;
		err = dnet_transform(n, file, len, id, &rsize, &pos);
		if (err) {
			if (err > 0)
				break;
			continue;
		}

		pos--;
		err = dnet_transform(n, data, size, io.id, &rsize, &pos);
		if (err) {
			if (err > 0)
				break;
			continue;
		}

		io.size = size;
		io.offset = offset;
		io.flags = DNET_IO_FLAGS_UPDATE;

		if (append)
			io.flags |= DNET_IO_FLAGS_APPEND;

		err = dnet_write_object(n, id, &io, dnet_write_complete, strdup(file), data);
		if (err)
			continue;

		err = dnet_write_object(n, io.id, &io, dnet_write_complete, strdup(file), data);
		if (err)
			continue;

		error = 0;
	}

	return error;
}

int dnet_read_complete(struct dnet_net_state *st, struct el_cmd *cmd, struct el_attr *a, void *priv)
{
	int fd, err;
	struct dnet_io_completion *c = priv;
	struct el_io_attr *io;
	void *data;

	if (!cmd)
		return -ENOMEM;

	if (cmd->status != 0 || cmd->size == 0)
		goto out;

	if (cmd->flags & DNET_FLAGS_DESTROY) {
	}

	if (cmd->size <= sizeof(struct el_attr) + sizeof(struct el_io_attr)) {
		ulog("%s: read completion error: wrong size: cmd_size: %llu, must be more than %u.\n",
				el_dump_id(cmd->id), (unsigned long long)cmd->size,
				sizeof(struct el_attr) + sizeof(struct el_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!a) {
		ulog("%s: no attributes but command size is not null.\n", el_dump_id(cmd->id));
		err = -EINVAL;
		goto err_out_exit;
	}

	io = (struct el_io_attr *)(a + 1);
	data = io + 1;

	el_convert_attr(a);
	el_convert_io_attr(io);

	fd = openat(st->n->rootfd, c->file, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		err = -errno;
		ulog_err("%s: failed to open read completion file '%s'", el_dump_id(cmd->id), c->file);
		goto err_out_exit;
	}

	err = pwrite(fd, data, io->size, io->offset);
	if (err <= 0) {
		err = -errno;
		ulog_err("%s: failed to write data into completion file '%s'", el_dump_id(cmd->id), c->file);
		goto err_out_close;
	}

	close(fd);
	ulog("%s: read completed: file: '%s', offset: %llu, size: %llu, status: %d.\n",
			el_dump_id(cmd->id), c->file, (unsigned long long)io->offset,
			(unsigned long long)io->size, cmd->status);

out:
	return cmd->status;

err_out_close:
	close(fd);
	ulog("%s: read completed: file: '%s', offset: %llu, size: %llu, status: %d, err: %d.\n",
			el_dump_id(cmd->id), c->file, (unsigned long long)io->offset,
			(unsigned long long)io->size, cmd->status, err);
err_out_exit:
	return err;
}

int dnet_read_object(struct dnet_node *n, struct el_io_attr *io,
	int (* complete)(struct dnet_net_state *, struct el_cmd *, struct el_attr *, void *), void *priv)
{
	struct dnet_trans *t;
	struct dnet_net_state *st;
	int err;

	t = dnet_io_trans_create(n, io->id, DNET_CMD_READ, io, complete, priv);
	if (!t) {
		ulog("%s: failed to create transaction.\n", el_dump_id(io->id));
		goto err_out_exit;
	}

	st = t->st;
	ulog("Read transaction: object: %s, ", el_dump_id(io->id));
	uloga("offset: %llu, size: %llu, trans: %llu <- ", (unsigned long long)io->offset,
			(unsigned long long)io->size, (unsigned long long)t->trans);
	uloga("%s.\n", el_dump_id(st->id));

	pthread_mutex_lock(&st->lock);

	err = dnet_send(st, t+1, sizeof(struct el_attr) + sizeof(struct el_io_attr) + sizeof(struct el_cmd));
	if (err)
		goto err_out_unlock;

	pthread_mutex_unlock(&st->lock);

	return 0;

err_out_unlock:
	pthread_mutex_unlock(&st->lock);
	dnet_trans_destroy(t);
err_out_exit:
	return err;
}

int dnet_read_file(struct dnet_node *n, char *file, __u64 offset, __u64 size)
{
	struct el_io_attr io;
	int err, len = strlen(file), pos = 0;
	struct dnet_io_completion *c;

	io.size = size;
	io.offset = offset;
	io.flags = 0;

	while (1) {
		unsigned int rsize = EL_ID_SIZE;

		err = dnet_transform(n, file, len, io.id, &rsize, &pos);
		if (err) {
			if (err > 0)
				break;
			continue;
		}

		c = malloc(sizeof(struct dnet_io_completion) + len + 1);
		if (!c) {
			ulog("%s: failed to allocate IO completion structure for '%s' file reading.\n",
					el_dump_id(io.id), file);
			err = -ENOMEM;
			goto err_out_exit;
		}

		c->offset = offset;
		c->size = size;
		c->file = (char *)(c + 1);
		sprintf(c->file, "%s", file);

		err = dnet_read_object(n, &io, dnet_read_complete, c);
		if (err)
			continue;

		break;
	}

	return 0;

err_out_exit:
	return err;
}

int dnet_add_transform(struct dnet_node *n, void *priv, char *name,
	int (* init)(void *priv),
	int (* update)(void *priv, void *src, __u64 size,
		void *dst, unsigned int *dsize, unsigned int flags),
	int (* final)(void *priv, void *dst, unsigned int *dsize, unsigned int flags))
{
	struct dnet_transform *t;
	int err = 0;

	if (!n || !init || !update || !final || !name) {
		err = -EINVAL;
		goto err_out_exit;
	}

	pthread_mutex_lock(&n->tlock);
	list_for_each_entry(t, &n->tlist, tentry) {
		if (!strncmp(name, t->name, EL_MAX_NAME_LEN)) {
			err = -EEXIST;
			goto err_out_unlock;
		}
	}

	t = malloc(sizeof(struct dnet_transform));
	if (!t) {
		err = -ENOMEM;
		goto err_out_unlock;
	}

	memset(t, 0, sizeof(struct dnet_transform));

	snprintf(t->name, sizeof(t->name), "%s", name);
	t->init = init;
	t->update = update;
	t->final = final;
	t->priv = priv;

	list_add_tail(&t->tentry, &n->tlist);

	pthread_mutex_unlock(&n->tlock);

	return 0;

err_out_unlock:
	pthread_mutex_unlock(&n->tlock);
err_out_exit:
	return err;
}

int dnet_remove_transform(struct dnet_node *n, char *name)
{
	struct dnet_transform *t, *tmp;
	int err = -ENOENT;

	if (!n)
		return -EINVAL;

	pthread_mutex_lock(&n->tlock);
	list_for_each_entry_safe(t, tmp, &n->tlist, tentry) {
		if (!strncmp(name, t->name, EL_MAX_NAME_LEN)) {
			err = 0;
			break;
		}
	}

	if (!err) {
		list_del(&t->tentry);
		free(t);
	}
	pthread_mutex_unlock(&n->tlock);

	return err;
}
