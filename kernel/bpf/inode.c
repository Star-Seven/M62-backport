/*
 * Minimal file system backend for holding eBPF maps and programs,
 * used by bpf(2) object pinning.
 *
 * Authors:
 *
 *	Daniel Borkmann <daniel@iogearbox.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/magic.h>
#include <linux/major.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/parser.h>
#include <linux/filter.h>
#include <linux/bpf.h>
#include <linux/bpf_trace.h>

enum bpf_type {
	BPF_TYPE_UNSPEC	= 0,
	BPF_TYPE_PROG,
	BPF_TYPE_MAP,
};

static void *bpf_any_get(void *raw, enum bpf_type type)
{
	switch (type) {
	case BPF_TYPE_PROG:
		raw = bpf_prog_inc(raw);
		break;
	case BPF_TYPE_MAP:
		raw = bpf_map_inc(raw, true);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

	return raw;
}

static void bpf_any_put(void *raw, enum bpf_type type)
{
	switch (type) {
	case BPF_TYPE_PROG:
		bpf_prog_put(raw);
		break;
	case BPF_TYPE_MAP:
		bpf_map_put_with_uref(raw);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static void *bpf_fd_probe_obj(u32 ufd, enum bpf_type *type)
{
	void *raw;

	*type = BPF_TYPE_MAP;
	raw = bpf_map_get_with_uref(ufd);
	if (IS_ERR(raw)) {
		*type = BPF_TYPE_PROG;
		raw = bpf_prog_get(ufd);
	}

	return raw;
}

static const struct inode_operations bpf_dir_iops;

static const struct inode_operations bpf_prog_iops = { };
static const struct inode_operations bpf_map_iops  = { };

static struct inode *bpf_get_inode(struct super_block *sb,
				   const struct inode *dir,
				   umode_t mode)
{
	struct inode *inode;

	switch (mode & S_IFMT) {
	case S_IFDIR:
	case S_IFREG:
	case S_IFLNK:
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOSPC);

	inode->i_ino = get_next_ino();
	inode->i_atime = current_time(inode);
	inode->i_mtime = inode->i_atime;
	inode->i_ctime = inode->i_atime;

	inode_init_owner(inode, dir, mode);

	return inode;
}

static int bpf_inode_type(const struct inode *inode, enum bpf_type *type)
{
	*type = BPF_TYPE_UNSPEC;
	if (inode->i_op == &bpf_prog_iops)
		*type = BPF_TYPE_PROG;
	else if (inode->i_op == &bpf_map_iops)
		*type = BPF_TYPE_MAP;
	else
		return -EACCES;

	return 0;
}

static void bpf_dentry_finalize(struct dentry *dentry, struct inode *inode,
				struct inode *dir)
{
	d_instantiate(dentry, inode);
	dget(dentry);

	dir->i_mtime = current_time(dir);
	dir->i_ctime = dir->i_mtime;
}

static int bpf_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;

	inode = bpf_get_inode(dir->i_sb, dir, mode | S_IFDIR);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &bpf_dir_iops;
	inode->i_fop = &simple_dir_operations;

	inc_nlink(inode);
	inc_nlink(dir);

	bpf_dentry_finalize(dentry, inode, dir);
	return 0;
}

struct map_iter {
	void *key;
	bool done;
};
static struct map_iter *map_iter(struct seq_file *m)
{
	return m->private;
}
static struct bpf_map *seq_file_to_map(struct seq_file *m)
{
	return file_inode(m->file)->i_private;
}
static void map_iter_free(struct map_iter *iter)
{
	if (iter) {
		kfree(iter->key);
		kfree(iter);
	}
}
static struct map_iter *map_iter_alloc(struct bpf_map *map)
{
	struct map_iter *iter;
	iter = kzalloc(sizeof(*iter), GFP_KERNEL | __GFP_NOWARN);
	if (!iter)
		goto error;
	iter->key = kzalloc(map->key_size, GFP_KERNEL | __GFP_NOWARN);
	if (!iter->key)
		goto error;
	return iter;
error:
	map_iter_free(iter);
	return NULL;
}
static void *map_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct bpf_map *map = seq_file_to_map(m);
	void *key = map_iter(m)->key;
	if (map_iter(m)->done)
		return NULL;
	if (unlikely(v == SEQ_START_TOKEN))
		goto done;
	if (map->ops->map_get_next_key(map, key, key)) {
		map_iter(m)->done = true;
		return NULL;
	}
done:
	++(*pos);
	return key;
}
static void *map_seq_start(struct seq_file *m, loff_t *pos)
{
	if (map_iter(m)->done)
		return NULL;
	return *pos ? map_iter(m)->key : SEQ_START_TOKEN;
}
static void map_seq_stop(struct seq_file *m, void *v)
{
}
static int map_seq_show(struct seq_file *m, void *v)
{
	struct bpf_map *map = seq_file_to_map(m);
	void *key = map_iter(m)->key;
	if (unlikely(v == SEQ_START_TOKEN)) {
		seq_puts(m, "# WARNING!! The output is for debug purpose only\n");
		seq_puts(m, "# WARNING!! The output format will change\n");
	} else {
		map->ops->map_seq_show_elem(map, key, m);
	}
	return 0;
}
static const struct seq_operations bpffs_map_seq_ops = {
	.start	= map_seq_start,
	.next	= map_seq_next,
	.show	= map_seq_show,
	.stop	= map_seq_stop,
};
static int bpffs_map_open(struct inode *inode, struct file *file)
{
	struct bpf_map *map = inode->i_private;
	struct map_iter *iter;
	struct seq_file *m;
	int err;
	iter = map_iter_alloc(map);
	if (!iter)
		return -ENOMEM;
	err = seq_open(file, &bpffs_map_seq_ops);
	if (err) {
		map_iter_free(iter);
		return err;
	}
	m = file->private_data;
	m->private = iter;
	return 0;
}
static int bpffs_map_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	map_iter_free(map_iter(m));
	return seq_release(inode, file);
}
/* bpffs_map_fops should only implement the basic
 * read operation for a BPF map.  The purpose is to
 * provide a simple user intuitive way to do
 * "cat bpffs/pathto/a-pinned-map".
 *
 * Other operations (e.g. write, lookup...) should be realized by
 * the userspace tools (e.g. bpftool) through the
 * BPF_OBJ_GET_INFO_BY_FD and the map's lookup/update
 * interface.
 */
static const struct file_operations bpffs_map_fops = {
	.open		= bpffs_map_open,
	.read		= seq_read,
	.release	= bpffs_map_release,
};

static int bpf_mkobj_ops(struct dentry *dentry, umode_t mode, void *raw,
			 const struct inode_operations *iops,
			 const struct file_operations *fops)
{
	struct inode *dir = dentry->d_parent->d_inode;
	struct inode *inode = bpf_get_inode(dir->i_sb, dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = iops;
	inode->i_private = raw;
	inode->i_fop = fops;

	bpf_dentry_finalize(dentry, inode, dir);
	return 0;
}

static int bpf_mkprog(struct dentry *dentry, umode_t mode, void *arg)
{
	return bpf_mkobj_ops(dentry, mode, arg, &bpf_prog_iops, NULL);
}

static int bpf_mkmap(struct dentry *dentry, umode_t mode, void *arg)
{
	struct bpf_map *map = arg;
	return bpf_mkobj_ops(dentry, mode, arg, &bpf_map_iops,
			     map->btf ? &bpffs_map_fops : NULL);
}

static struct dentry *
bpf_lookup(struct inode *dir, struct dentry *dentry, unsigned flags)
{
	if (strchr(dentry->d_name.name, '.'))
		return ERR_PTR(-EPERM);

	return simple_lookup(dir, dentry, flags);
}

static int bpf_symlink(struct inode *dir, struct dentry *dentry,
		       const char *target)
{
	char *link = kstrdup(target, GFP_USER | __GFP_NOWARN);
	struct inode *inode;

	if (!link)
		return -ENOMEM;

	inode = bpf_get_inode(dir->i_sb, dir, S_IRWXUGO | S_IFLNK);
	if (IS_ERR(inode)) {
		kfree(link);
		return PTR_ERR(inode);
	}

	inode->i_op = &simple_symlink_inode_operations;
	inode->i_link = link;

	bpf_dentry_finalize(dentry, inode, dir);
	return 0;
}

static const struct inode_operations bpf_dir_iops = {
	.lookup		= bpf_lookup,
	.mkdir		= bpf_mkdir,
	.symlink	= bpf_symlink,
	.rmdir		= simple_rmdir,
	.rename		= simple_rename,
	.link		= simple_link,
	.unlink		= simple_unlink,
};

static int bpf_obj_do_pin(const struct filename *pathname, void *raw,
			  enum bpf_type type)
{
	struct dentry *dentry;
	struct inode *dir;
	struct path path;
	umode_t mode;
	int ret;

	dentry = kern_path_create(AT_FDCWD, pathname->name, &path, 0);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	mode = S_IFREG | ((S_IRUSR | S_IWUSR) & ~current_umask());
	ret = security_path_mknod(&path, dentry, mode, 0);
	if (ret)
		goto out;

	dir = d_inode(path.dentry);
	if (dir->i_op != &bpf_dir_iops) {
		ret = -EPERM;
		goto out;
	}

	switch (type) {
	case BPF_TYPE_PROG:
		ret = vfs_mkobj(dentry, mode, bpf_mkprog, raw);
		break;
	case BPF_TYPE_MAP:
		ret = vfs_mkobj(dentry, mode, bpf_mkmap, raw);
		break;
	default:
		ret = -EPERM;
	}
out:
	done_path_create(&path, dentry);
	return ret;
}

int bpf_obj_pin_user(u32 ufd, const char __user *pathname)
{
	struct filename *pname;
	enum bpf_type type;
	void *raw;
	int ret;

	pname = getname(pathname);
	if (IS_ERR(pname))
		return PTR_ERR(pname);

	raw = bpf_fd_probe_obj(ufd, &type);
	if (IS_ERR(raw)) {
		ret = PTR_ERR(raw);
		goto out;
	}

	ret = bpf_obj_do_pin(pname, raw, type);
	if (ret != 0)
		bpf_any_put(raw, type);
	if ((trace_bpf_obj_pin_prog_enabled() ||
	     trace_bpf_obj_pin_map_enabled()) && !ret) {
		if (type == BPF_TYPE_PROG)
			trace_bpf_obj_pin_prog(raw, ufd, pname);
		if (type == BPF_TYPE_MAP)
			trace_bpf_obj_pin_map(raw, ufd, pname);
	}
out:
	putname(pname);
	return ret;
}

static void *bpf_obj_do_get(const struct filename *pathname,
			    enum bpf_type *type, int flags)
{
	struct inode *inode;
	struct path path;
	void *raw;
	int ret;

	ret = kern_path(pathname->name, LOOKUP_FOLLOW, &path);
	if (ret)
		return ERR_PTR(ret);

	inode = d_backing_inode(path.dentry);
	ret = inode_permission(inode, ACC_MODE(flags));
	if (ret)
		goto out;

	ret = bpf_inode_type(inode, type);
	if (ret)
		goto out;

	raw = bpf_any_get(inode->i_private, *type);
	if (!IS_ERR(raw))
		touch_atime(&path);

	path_put(&path);
	return raw;
out:
	path_put(&path);
	return ERR_PTR(ret);
}

int bpf_obj_get_user(const char __user *pathname, int flags)
{
	enum bpf_type type = BPF_TYPE_UNSPEC;
	struct filename *pname;
	int ret = -ENOENT;
	int f_flags;
	void *raw;

	f_flags = bpf_get_file_flag(flags);
	if (f_flags < 0)
		return f_flags;

	pname = getname(pathname);
	if (IS_ERR(pname))
		return PTR_ERR(pname);

	raw = bpf_obj_do_get(pname, &type, f_flags);
	if (IS_ERR(raw)) {
		ret = PTR_ERR(raw);
		goto out;
	}

	if (type == BPF_TYPE_PROG)
		ret = bpf_prog_new_fd(raw);
	else if (type == BPF_TYPE_MAP)
		ret = bpf_map_new_fd(raw, f_flags);
	else
		goto out;

	if (ret < 0) {
		bpf_any_put(raw, type);
	} else if (trace_bpf_obj_get_prog_enabled() ||
		   trace_bpf_obj_get_map_enabled()) {
		if (type == BPF_TYPE_PROG)
			trace_bpf_obj_get_prog(raw, ret, pname);
		if (type == BPF_TYPE_MAP)
			trace_bpf_obj_get_map(raw, ret, pname);
	}
out:
	putname(pname);
	return ret;
}

static struct bpf_prog *__get_prog_inode(struct inode *inode, enum bpf_prog_type type)
{
	struct bpf_prog *prog;
	int ret = inode_permission(inode, MAY_READ);
	if (ret)
		return ERR_PTR(ret);

	if (inode->i_op == &bpf_map_iops)
		return ERR_PTR(-EINVAL);
	if (inode->i_op != &bpf_prog_iops)
		return ERR_PTR(-EACCES);

	prog = inode->i_private;

	ret = security_bpf_prog(prog);
	if (ret < 0)
		return ERR_PTR(ret);

	return bpf_prog_inc(prog);
}

struct bpf_prog *bpf_prog_get_type_path(const char *name, enum bpf_prog_type type)
{
	struct bpf_prog *prog;
	struct path path;
	int ret = kern_path(name, LOOKUP_FOLLOW, &path);
	if (ret)
		return ERR_PTR(ret);
	prog = __get_prog_inode(d_backing_inode(path.dentry), type);
	if (!IS_ERR(prog))
		touch_atime(&path);
	path_put(&path);
	return prog;
}
EXPORT_SYMBOL(bpf_prog_get_type_path);

/*
 * Display the mount options in /proc/mounts.
 */
static int bpf_show_options(struct seq_file *m, struct dentry *root)
{
	umode_t mode = d_inode(root)->i_mode & S_IALLUGO & ~S_ISVTX;

	if (mode != S_IRWXUGO)
		seq_printf(m, ",mode=%o", mode);
	return 0;
}

static void bpf_destroy_inode_deferred(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	enum bpf_type type;

	if (S_ISLNK(inode->i_mode))
		kfree(inode->i_link);
	if (!bpf_inode_type(inode, &type))
		bpf_any_put(inode->i_private, type);
	free_inode_nonrcu(inode);
}

static void bpf_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, bpf_destroy_inode_deferred);
}

static const struct super_operations bpf_super_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= bpf_show_options,
	.destroy_inode	= bpf_destroy_inode,
};

enum {
	OPT_MODE,
	OPT_ERR,
};

static const match_table_t bpf_mount_tokens = {
	{ OPT_MODE, "mode=%o" },
	{ OPT_ERR, NULL },
};

struct bpf_mount_opts {
	umode_t mode;
};

static int bpf_parse_options(char *data, struct bpf_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option, token;
	char *ptr;

	opts->mode = S_IRWXUGO;

	while ((ptr = strsep(&data, ",")) != NULL) {
		if (!*ptr)
			continue;

		token = match_token(ptr, bpf_mount_tokens, args);
		switch (token) {
		case OPT_MODE:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->mode = option & S_IALLUGO;
			break;
		/* We might like to report bad mount options here, but
		 * traditionally we've ignored all mount options, so we'd
		 * better continue to ignore non-existing options for bpf.
		 */
		}
	}

	return 0;
}

static int bpf_fill_super(struct super_block *sb, void *data, int silent)
{
	static const struct tree_descr bpf_rfiles[] = { { "" } };
	struct bpf_mount_opts opts;
	struct inode *inode;
	int ret;

	ret = bpf_parse_options(data, &opts);
	if (ret)
		return ret;

	ret = simple_fill_super(sb, BPF_FS_MAGIC, bpf_rfiles);
	if (ret)
		return ret;

	sb->s_op = &bpf_super_ops;

	inode = sb->s_root->d_inode;
	inode->i_op = &bpf_dir_iops;
	inode->i_mode &= ~S_IALLUGO;
	inode->i_mode |= S_ISVTX | opts.mode;

	return 0;
}

static struct dentry *bpf_mount(struct file_system_type *type, int flags,
				const char *dev_name, void *data)
{
	return mount_nodev(type, flags, data, bpf_fill_super);
}

static struct file_system_type bpf_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "bpf",
	.mount		= bpf_mount,
	.kill_sb	= kill_litter_super,
};

static int __init bpf_init(void)
{
	int ret;

	ret = sysfs_create_mount_point(fs_kobj, "bpf");
	if (ret)
		return ret;

	ret = register_filesystem(&bpf_fs_type);
	if (ret)
		sysfs_remove_mount_point(fs_kobj, "bpf");

	return ret;
}
fs_initcall(bpf_init);
