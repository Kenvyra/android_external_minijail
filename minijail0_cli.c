/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <linux/filter.h>

#include "libminijail.h"
#include "libsyscalls.h"

#include "config_parser.h"
#include "elfparse.h"
#include "minijail0_cli.h"
#include "system.h"
#include "util.h"

#define IDMAP_LEN 32U
#define DEFAULT_TMP_SIZE (64 * 1024 * 1024)

/*
 * A malloc() that aborts on failure.  We only implement this in the CLI as
 * the library should return ENOMEM errors when allocations fail.
 */
static void *xmalloc(size_t size)
{
	void *ret = malloc(size);
	if (!ret)
		err(1, "malloc() failed");
	return ret;
}

static char *xstrdup(const char *s)
{
	char *ret = strdup(s);
	if (!ret)
		err(1, "strdup() failed");
	return ret;
}

static void set_user(struct minijail *j, const char *arg, uid_t *out_uid,
		     gid_t *out_gid)
{
	char *end = NULL;
	uid_t uid = strtoul(arg, &end, 10);
	if (!*end && *arg) {
		*out_uid = uid;
		minijail_change_uid(j, uid);
		return;
	}

	int ret = lookup_user(arg, out_uid, out_gid);
	if (ret) {
		errno = -ret;
		err(1, "Bad user '%s'", arg);
	}

	ret = minijail_change_user(j, arg);
	if (ret) {
		errno = -ret;
		err(1, "minijail_change_user('%s') failed", arg);
	}
}

static void set_group(struct minijail *j, const char *arg, gid_t *out_gid)
{
	char *end = NULL;
	gid_t gid = strtoul(arg, &end, 10);
	if (!*end && *arg) {
		*out_gid = gid;
		minijail_change_gid(j, gid);
		return;
	}

	int ret = lookup_group(arg, out_gid);
	if (ret) {
		errno = -ret;
		err(1, "Bad group '%s'", arg);
	}

	minijail_change_gid(j, *out_gid);
}

/*
 * Helper function used by --add-suppl-group (possibly more than once),
 * to build the supplementary gids array.
 */
static void suppl_group_add(size_t *suppl_gids_count, gid_t **suppl_gids,
			    char *arg)
{
	char *end = NULL;
	gid_t gid = strtoul(arg, &end, 10);
	int ret;
	if (!*end && *arg) {
		/* A gid number has been specified, proceed. */
	} else if ((ret = lookup_group(arg, &gid))) {
		/*
		 * A group name has been specified,
		 * but doesn't exist: we bail out.
		 */
		errno = -ret;
		err(1, "Bad group '%s'", arg);
	}

	/*
	 * From here, gid is guaranteed to be set and valid,
	 * we add it to our supplementary gids array.
	 */
	*suppl_gids =
	    realloc(*suppl_gids, sizeof(gid_t) * ++(*suppl_gids_count));
	if (!suppl_gids)
		err(1, "failed to allocate memory");

	(*suppl_gids)[*suppl_gids_count - 1] = gid;
}

static void skip_securebits(struct minijail *j, const char *arg)
{
	uint64_t securebits_skip_mask;
	char *end = NULL;
	securebits_skip_mask = strtoull(arg, &end, 16);
	if (*end)
		errx(1, "Invalid securebit mask: '%s'", arg);
	minijail_skip_setting_securebits(j, securebits_skip_mask);
}

static void use_caps(struct minijail *j, const char *arg)
{
	uint64_t caps = 0;
	cap_t parsed_caps = cap_from_text(arg);

	if (parsed_caps != NULL) {
		unsigned int i;
		const uint64_t one = 1;
		cap_flag_value_t cap_value;
		unsigned int last_valid_cap = get_last_valid_cap();

		for (i = 0; i <= last_valid_cap; ++i) {
			if (cap_get_flag(parsed_caps, i, CAP_EFFECTIVE,
					 &cap_value)) {
				if (errno == EINVAL) {
					/*
					 * Some versions of libcap reject any
					 * capabilities they were not compiled
					 * with by returning EINVAL.
					 */
					continue;
				}
				err(1,
				    "Could not get the value of the %d-th "
				    "capability",
				    i);
			}
			if (cap_value == CAP_SET)
				caps |= (one << i);
		}
		cap_free(parsed_caps);
	} else {
		char *end = NULL;
		caps = strtoull(arg, &end, 16);
		if (*end)
			errx(1, "Invalid cap set: '%s'", arg);
	}

	minijail_use_caps(j, caps);
}

static void add_binding(struct minijail *j, char *arg)
{
	char *src = tokenize(&arg, ",");
	char *dest = tokenize(&arg, ",");
	char *flags = tokenize(&arg, ",");
	if (!src || src[0] == '\0' || arg != NULL)
		errx(1, "Bad binding: %s %s", src, dest);
	if (dest == NULL || dest[0] == '\0')
		dest = src;
	int writable;
	if (flags == NULL || flags[0] == '\0' || !strcmp(flags, "0"))
		writable = 0;
	else if (!strcmp(flags, "1"))
		writable = 1;
	else
		errx(1, "Bad value for <writable>: %s", flags);
	if (minijail_bind(j, src, dest, writable))
		errx(1, "minijail_bind failed");
}

static void add_rlimit(struct minijail *j, char *arg)
{
	char *type = tokenize(&arg, ",");
	char *cur = tokenize(&arg, ",");
	char *max = tokenize(&arg, ",");
	char *end;
	if (!type || type[0] == '\0' || !cur || cur[0] == '\0' || !max ||
	    max[0] == '\0' || arg != NULL) {
		errx(1, "Bad rlimit '%s'", arg);
	}
	rlim_t cur_rlim;
	rlim_t max_rlim;
	if (!strcmp(cur, "unlimited")) {
		cur_rlim = RLIM_INFINITY;
	} else {
		end = NULL;
		cur_rlim = strtoul(cur, &end, 0);
		if (*end)
			errx(1, "Bad soft limit: '%s'", cur);
	}
	if (!strcmp(max, "unlimited")) {
		max_rlim = RLIM_INFINITY;
	} else {
		end = NULL;
		max_rlim = strtoul(max, &end, 0);
		if (*end)
			errx(1, "Bad hard limit: '%s'", max);
	}

	end = NULL;
	int resource = parse_single_constant(type, &end);
	if (type == end)
		errx(1, "Bad rlimit: '%s'", type);

	if (minijail_rlimit(j, resource, cur_rlim, max_rlim))
		errx(1, "minijail_rlimit '%s,%s,%s' failed", type, cur, max);
}

static void add_mount(struct minijail *j, char *arg)
{
	char *src = tokenize(&arg, ",");
	char *dest = tokenize(&arg, ",");
	char *type = tokenize(&arg, ",");
	char *flags = tokenize(&arg, ",");
	char *data = tokenize(&arg, ",");
	char *end;
	if (!src || src[0] == '\0' || !dest || dest[0] == '\0' || !type ||
	    type[0] == '\0') {
		errx(1, "Bad mount: %s %s %s", src, dest, type);
	}

	/*
	 * Fun edge case: the data option itself is comma delimited.  If there
	 * were no more options, then arg would be set to NULL.  But if we had
	 * more pending, it'll be pointing to the next token.  Back up and undo
	 * the null byte so it'll be merged back.
	 * An example:
	 *   none,/tmp,tmpfs,0xe,mode=0755,uid=10,gid=10
	 * The tokenize calls above will turn this memory into:
	 *   none\0/tmp\0tmpfs\00xe\0mode=0755\0uid=10,gid=10
	 * With data pointing at mode=0755 and arg pointing at uid=10,gid=10.
	 */
	if (arg != NULL)
		arg[-1] = ',';

	unsigned long mountflags;
	if (flags == NULL || flags[0] == '\0') {
		mountflags = 0;
	} else {
		end = NULL;
		mountflags = parse_constant(flags, &end);
		if (flags == end)
			errx(1, "Bad mount flags: %s", flags);
	}

	if (minijail_mount_with_data(j, src, dest, type, mountflags, data))
		errx(1, "minijail_mount failed");
}

static char *build_idmap(id_t id, id_t lowerid)
{
	int ret;
	char *idmap = xmalloc(IDMAP_LEN);
	ret = snprintf(idmap, IDMAP_LEN, "%d %d 1", id, lowerid);
	if (ret < 0 || (size_t)ret >= IDMAP_LEN) {
		free(idmap);
		errx(1, "Could not build id map");
	}
	return idmap;
}

static int has_cap_setgid(void)
{
	cap_t caps;
	cap_flag_value_t cap_value;

	if (!CAP_IS_SUPPORTED(CAP_SETGID))
		return 0;

	caps = cap_get_proc();
	if (!caps)
		err(1, "Could not get process' capabilities");

	if (cap_get_flag(caps, CAP_SETGID, CAP_EFFECTIVE, &cap_value))
		err(1, "Could not get the value of CAP_SETGID");

	if (cap_free(caps))
		err(1, "Could not free capabilities");

	return cap_value == CAP_SET;
}

static void set_ugid_mapping(struct minijail *j, int set_uidmap, uid_t uid,
			     char *uidmap, int set_gidmap, gid_t gid,
			     char *gidmap)
{
	if (set_uidmap) {
		minijail_namespace_user(j);
		minijail_namespace_pids(j);

		if (!uidmap) {
			/*
			 * If no map is passed, map the current uid to the
			 * chosen uid in the target namespace (or root, if none
			 * was chosen).
			 */
			uidmap = build_idmap(uid, getuid());
		}
		if (0 != minijail_uidmap(j, uidmap))
			errx(1, "Could not set uid map");
		free(uidmap);
	}
	if (set_gidmap) {
		minijail_namespace_user(j);
		minijail_namespace_pids(j);

		if (!gidmap) {
			/*
			 * If no map is passed, map the current gid to the
			 * chosen gid in the target namespace.
			 */
			gidmap = build_idmap(gid, getgid());
		}
		if (!has_cap_setgid()) {
			/*
			 * This means that we are not running as root,
			 * so we also have to disable setgroups(2) to
			 * be able to set the gid map.
			 * See
			 * http://man7.org/linux/man-pages/man7/user_namespaces.7.html
			 */
			minijail_namespace_user_disable_setgroups(j);
		}
		if (0 != minijail_gidmap(j, gidmap))
			errx(1, "Could not set gid map");
		free(gidmap);
	}
}

static void use_chroot(struct minijail *j, const char *path, int *chroot,
		       int pivot_root)
{
	if (pivot_root)
		errx(1, "Could not set chroot because -P was specified");
	if (minijail_enter_chroot(j, path))
		errx(1, "Could not set chroot");
	*chroot = 1;
}

static void use_pivot_root(struct minijail *j, const char *path,
			   int *pivot_root, int chroot)
{
	if (chroot)
		errx(1, "Could not set pivot_root because -C was specified");
	if (minijail_enter_pivot_root(j, path))
		errx(1, "Could not set pivot_root");
	minijail_namespace_vfs(j);
	*pivot_root = 1;
}

static void use_profile(struct minijail *j, const char *profile,
			int *pivot_root, int chroot, size_t *tmp_size)
{
	/* Note: New profiles should be added in minijail0_cli_unittest.cc. */

	if (!strcmp(profile, "minimalistic-mountns") ||
	    !strcmp(profile, "minimalistic-mountns-nodev")) {
		minijail_namespace_vfs(j);
		if (minijail_bind(j, "/", "/", 0))
			errx(1, "minijail_bind(/) failed");
		if (minijail_bind(j, "/proc", "/proc", 0))
			errx(1, "minijail_bind(/proc) failed");
		if (!strcmp(profile, "minimalistic-mountns")) {
			if (minijail_bind(j, "/dev/log", "/dev/log", 0))
				errx(1, "minijail_bind(/dev/log) failed");
			minijail_mount_dev(j);
		}
		if (!*tmp_size) {
			/* Avoid clobbering |tmp_size| if it was already set. */
			*tmp_size = DEFAULT_TMP_SIZE;
		}
		minijail_remount_proc_readonly(j);
		use_pivot_root(j, DEFAULT_PIVOT_ROOT, pivot_root, chroot);
	} else
		errx(1, "Unrecognized profile name '%s'", profile);
}

static void set_remount_mode(struct minijail *j, const char *mode)
{
	unsigned long msmode;
	if (!strcmp(mode, "shared"))
		msmode = MS_SHARED;
	else if (!strcmp(mode, "private"))
		msmode = MS_PRIVATE;
	else if (!strcmp(mode, "slave"))
		msmode = MS_SLAVE;
	else if (!strcmp(mode, "unbindable"))
		msmode = MS_UNBINDABLE;
	else
		errx(1, "Unknown remount mode: '%s'", mode);
	minijail_remount_mode(j, msmode);
}

static void read_seccomp_filter(const char *filter_path,
				struct sock_fprog *filter)
{
	attribute_cleanup_fp FILE *f = fopen(filter_path, "re");
	if (!f)
		err(1, "failed to open %s", filter_path);
	off_t filter_size = 0;
	if (fseeko(f, 0, SEEK_END) == -1 || (filter_size = ftello(f)) == -1)
		err(1, "failed to get file size of %s", filter_path);
	if (filter_size % sizeof(struct sock_filter) != 0) {
		errx(1,
		     "filter size (%" PRId64 ") of %s is not a multiple of"
		     " %zu",
		     filter_size, filter_path, sizeof(struct sock_filter));
	}
	rewind(f);

	filter->len = filter_size / sizeof(struct sock_filter);
	filter->filter = xmalloc(filter_size);
	if (fread(filter->filter, sizeof(struct sock_filter), filter->len, f) !=
	    filter->len) {
		err(1, "failed read %s", filter_path);
	}
}

/*
 * Long options use values starting at 0x100 so that they're out of range of
 * bytes which is how command line options are processed.  Practically speaking,
 * we could get by with the (7-bit) ASCII range, but UTF-8 codepoints would be a
 * bit confusing, and honestly there's no reason to "optimize" here.
 *
 * The long enum values are internal to this file and can freely change at any
 * time without breaking anything.  Don't worry about ordering.
 */
enum {
	/* Everything after this point only have long options. */
	LONG_OPTION_BASE = 0x100,
	OPT_ADD_SUPPL_GROUP,
	OPT_ALLOW_SPECULATIVE_EXECUTION,
	OPT_AMBIENT,
	OPT_CONFIG,
	OPT_LOGGING,
	OPT_PRELOAD_LIBRARY,
	OPT_PROFILE,
	OPT_SECCOMP_BPF_BINARY,
	OPT_UTS,
};

static void usage(const char *progn)
{
	size_t i;
	/* clang-format off */
	printf("Usage: %s [-dGhHiIKlLnNprRstUvyYz]\n"
	       "  [-a <table>]\n"
	       "  [-b <src>[,[dest][,<writeable>]]] [-k <src>,<dest>,<type>[,<flags>[,<data>]]]\n"
	       "  [-c <caps>] [-C <dir>] [-P <dir>] [-e[file]] [-f <file>] [-g <group>]\n"
	       "  [-m[<uid> <loweruid> <count>]*] [-M[<gid> <lowergid> <count>]*] [--profile <name>]\n"
	       "  [-R <type,cur,max>] [-S <file>] [-t[size]] [-T <type>] [-u <user>] [-V <file>]\n"
	       "  <program> [args...]\n"
	       "  -a <table>:   Use alternate syscall table <table>.\n"
	       "  --bind-mount <...>,  Bind <src> to <dest> in chroot.\n"
	       "            -b <...>:  Multiple instances allowed.\n"
	       "  -B <mask>:    Skip setting securebits in <mask> when restricting capabilities (-c).\n"
	       "                By default, SECURE_NOROOT, SECURE_NO_SETUID_FIXUP, and \n"
	       "                SECURE_KEEP_CAPS (together with their respective locks) are set.\n"
	       "                There are eight securebits in total.\n"
	       "  --mount <...>,Mount <src> at <dest> in chroot.\n"
	       "       -k <...>:<flags> and <data> can be specified as in mount(2).\n"
	       "                Multiple instances allowed.\n"
	       "  -c <caps>:    Restrict caps to <caps>.\n"
	       "  -C <dir>:     chroot(2) to <dir>.\n"
	       "                Not compatible with -P.\n"
	       "  -P <dir>:     pivot_root(2) to <dir> (implies -v).\n"
	       "                Not compatible with -C.\n"
	       "  --mount-dev,  Create a new /dev with a minimal set of device nodes (implies -v).\n"
	       "           -d:  See the minijail0(1) man page for the exact set.\n"
	       "  -e[file]:     Enter new network namespace, or existing one if |file| is provided.\n"
	       "  -f <file>:    Write the pid of the jailed process to <file>.\n"
	       "  -g <group>:   Change gid to <group>.\n"
	       "  -G:           Inherit supplementary groups from new uid.\n"
	       "                Not compatible with -y or --add-suppl-group.\n"
	       "  -y:           Keep original uid's supplementary groups.\n"
	       "                Not compatible with -G or --add-suppl-group.\n"
	       "  --add-suppl-group <g>:Add <g> to the proccess' supplementary groups,\n"
	       "                can be specified multiple times to add several groups.\n"
	       "                Not compatible with -y or -G.\n"
	       "  -h:           Help (this message).\n"
	       "  -H:           Seccomp filter help message.\n"
	       "  -i:           Exit immediately after fork(2). The jailed process will run\n"
	       "                in the background.\n"
	       "  -I:           Run <program> as init (pid 1) inside a new pid namespace (implies -p).\n"
	       "  -K:           Do not change share mode of any existing mounts.\n"
	       "  -K<mode>:     Mark all existing mounts as <mode> instead of MS_PRIVATE.\n"
	       "  -l:           Enter new IPC namespace.\n"
	       "  -L:           Report blocked syscalls when using seccomp filter.\n"
	       "                If the kernel does not support SECCOMP_RET_LOG,\n"
	       "                forces the following syscalls to be allowed:\n"
	       "                  ", progn);
	/* clang-format on */
	for (i = 0; i < log_syscalls_len; i++)
		printf("%s ", log_syscalls[i]);

	/* clang-format off */
	printf("\n"
	       "  -m[map]:      Set the uid map of a user namespace (implies -pU).\n"
	       "                Same arguments as newuidmap(1), multiple mappings should be separated by ',' (comma).\n"
	       "                With no mapping, map the current uid to root inside the user namespace.\n"
	       "                Not compatible with -b without the 'writable' option.\n"
	       "  -M[map]:      Set the gid map of a user namespace (implies -pU).\n"
	       "                Same arguments as newgidmap(1), multiple mappings should be separated by ',' (comma).\n"
	       "                With no mapping, map the current gid to root inside the user namespace.\n"
	       "                Not compatible with -b without the 'writable' option.\n"
	       "  -n:           Set no_new_privs.\n"
	       "  -N:           Enter a new cgroup namespace.\n"
	       "  -p:           Enter new pid namespace (implies -vr).\n"
	       "  -r:           Remount /proc read-only (implies -v).\n"
	       "  -R:           Set rlimits, can be specified multiple times.\n"
	       "  -s:           Use seccomp mode 1 (not the same as -S).\n"
	       "  -S <file>:    Set seccomp filter using <file>.\n"
	       "                E.g., '-S /usr/share/filters/<prog>.$(uname -m)'.\n"
	       "                Requires -n when not running as root.\n"
	       "  -t[size]:     Mount tmpfs at /tmp (implies -v).\n"
	       "                Optional argument specifies size (default \"64M\").\n"
	       "  -T <type>:    Assume <program> is a <type> ELF binary; <type> can be 'static' or 'dynamic'.\n"
	       "                This will avoid accessing <program> binary before execve(2).\n"
	       "                Type 'static' will avoid preload hooking.\n"
	       "  -u <user>:    Change uid to <user>.\n"
	       "  -U:           Enter new user namespace (implies -p).\n"
	       "  -v:           Enter new mount namespace.\n"
	       "  -V <file>:    Enter specified mount namespace.\n"
	       "  -w:           Create and join a new anonymous session keyring.\n"
	       "  -Y:           Synchronize seccomp filters across thread group.\n"
	       "  -z:           Don't forward signals to jailed process.\n"
	       "  --ambient:    Raise ambient capabilities. Requires -c.\n"
	       "  --uts[=name]: Enter a new UTS namespace (and set hostname).\n"
	       "  --logging=<s>:Use <s> as the logging system.\n"
	       "                <s> must be 'auto' (default), 'syslog', or 'stderr'.\n"
	       "  --profile <p>:Configure minijail0 to run with the <p> sandboxing profile,\n"
	       "                which is a convenient way to express multiple flags\n"
	       "                that are typically used together.\n"
	       "                See the minijail0(1) man page for the full list.\n"
	       "  --preload-library=<f>:Overrides the path to \"" PRELOADPATH "\".\n"
	       "                This is only really useful for local testing.\n"
	       "  --seccomp-bpf-binary=<f>:Set a pre-compiled seccomp filter using <f>.\n"
	       "                E.g., '-S /usr/share/filters/<prog>.$(uname -m).bpf'.\n"
	       "                Requires -n when not running as root.\n"
	       "                The user is responsible for ensuring that the binary\n"
	       "                was compiled for the correct architecture / kernel version.\n"
	       "  --allow-speculative-execution:Allow speculative execution and disable\n"
	       "                mitigations for speculative execution attacks.\n"
	       "  --config <file>:Load the Minijail configuration file <file>.\n"
	       "                If used, must be specified ahead of other options.\n");
	/* clang-format on */
}

static void seccomp_filter_usage(const char *progn)
{
	const struct syscall_entry *entry = syscall_table;
	printf("Usage: %s -S <policy.file> <program> [args...]\n\n"
	       "System call names supported:\n",
	       progn);
	for (; entry->name && entry->nr >= 0; ++entry)
		printf("  %s [%d]\n", entry->name, entry->nr);
	printf("\nSee minijail0(5) for example policies.\n");
}

/*
 * Return the next unconsumed option char/value parsed from
 * |*conf_entry_list|. |optarg| is updated to point to an argument from
 * the entry value. If all options have been consumed, |*conf_entry_list|
 * will be freed and -1 will be returned.
 */
static int getopt_from_conf(const struct option *longopts,
			    struct config_entry_list **conf_entry_list,
			    size_t *conf_index)
{
	int opt = -1;
	/* If we've consumed all the options in the this config, reset it. */
	if (*conf_index >= (*conf_entry_list)->num_entries) {
		free_config_entry_list(*conf_entry_list);
		*conf_entry_list = NULL;
		*conf_index = 0;
		return opt;
	}

	struct config_entry *entry = &(*conf_entry_list)->entries[*conf_index];
	/* Look up a matching long option. */
	size_t i = 0;
	const struct option *curr_opt;
	for (curr_opt = &longopts[0]; curr_opt->name != NULL;
	     curr_opt = &longopts[++i])
		if (strcmp(entry->key, curr_opt->name) == 0)
			break;
	if (curr_opt->name == NULL) {
		errx(1,
		     "Unable to recognize '%s' as Minijail conf entry key, "
		     "please refer to minijail0(5) for syntax and examples.",
		     entry->key);
	}
	opt = curr_opt->val;
	optarg = (char *)entry->value;
	(*conf_index)++;
	return opt;
}

/*
 * Similar to getopt(3), return the next option char/value as it
 * parses through the CLI argument list. Config entries in
 * |*conf_entry_list| will be parsed with precendences over cli options.
 * Same as getopt(3), |optarg| is pointing to the option argument.
 */
static int getopt_conf_or_cli(int argc, char *const argv[],
			      struct config_entry_list **conf_entry_list,
			      size_t *conf_index)
{
	int opt = -1;
	static const char optstring[] =
	    "+u:g:sS:c:C:P:b:B:V:f:m::M::k:a:e::R:T:vrGhHinNplLt::IUK::wyYzd";
	/* clang-format off */
	static const struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"mount-dev", no_argument, 0, 'd'},
		{"ambient", no_argument, 0, OPT_AMBIENT},
		{"uts", optional_argument, 0, OPT_UTS},
		{"logging", required_argument, 0, OPT_LOGGING},
		{"profile", required_argument, 0, OPT_PROFILE},
		{"preload-library", required_argument, 0, OPT_PRELOAD_LIBRARY},
		{"seccomp-bpf-binary", required_argument, 0, OPT_SECCOMP_BPF_BINARY},
		{"add-suppl-group", required_argument, 0, OPT_ADD_SUPPL_GROUP},
		{"allow-speculative-execution", no_argument, 0,
		 OPT_ALLOW_SPECULATIVE_EXECUTION},
		{"config", required_argument, 0, OPT_CONFIG},
		{"mount", required_argument, 0, 'k'},
		{"bind-mount", required_argument, 0, 'b'},
		{0, 0, 0, 0},
	};
	/* clang-format on */
	if (*conf_entry_list != NULL)
		opt =
		    getopt_from_conf(long_options, conf_entry_list, conf_index);
	if (opt == -1)
		opt = getopt_long(argc, argv, optstring, long_options, NULL);
	return opt;
}

int parse_args(struct minijail *j, int argc, char *const argv[],
	       int *exit_immediately, ElfType *elftype,
	       const char **preload_path)
{
	enum seccomp_type { None, Strict, Filter, BpfBinaryFilter };
	enum seccomp_type seccomp = None;
	int opt;
	int use_seccomp_filter = 0;
	int use_seccomp_filter_binary = 0;
	int use_seccomp_log = 0;
	int forward = 1;
	int binding = 0;
	int chroot = 0, pivot_root = 0;
	int mount_ns = 0, change_remount = 0;
	const char *remount_mode = NULL;
	int inherit_suppl_gids = 0, keep_suppl_gids = 0;
	int caps = 0, ambient_caps = 0;
	bool use_uid = false, use_gid = false;
	uid_t uid = 0;
	gid_t gid = 0;
	gid_t *suppl_gids = NULL;
	size_t suppl_gids_count = 0;
	char *uidmap = NULL, *gidmap = NULL;
	int set_uidmap = 0, set_gidmap = 0;
	size_t tmp_size = 0;
	const char *filter_path = NULL;
	int log_to_stderr = -1;
	struct config_entry_list *conf_entry_list = NULL;
	size_t conf_index = 0;

	while ((opt = getopt_conf_or_cli(argc, argv, &conf_entry_list,
					 &conf_index)) != -1) {
		switch (opt) {
		case 'u':
			if (use_uid)
				errx(1, "-u provided multiple times.");
			use_uid = true;
			set_user(j, optarg, &uid, &gid);
			break;
		case 'g':
			if (use_gid)
				errx(1, "-g provided multiple times.");
			use_gid = true;
			set_group(j, optarg, &gid);
			break;
		case 'n':
			minijail_no_new_privs(j);
			break;
		case 's':
			if (seccomp != None && seccomp != Strict) {
				errx(1, "Do not use -s, -S, or "
					"--seccomp-bpf-binary together");
			}
			seccomp = Strict;
			minijail_use_seccomp(j);
			break;
		case 'S':
			if (seccomp != None && seccomp != Filter) {
				errx(1, "Do not use -s, -S, or "
					"--seccomp-bpf-binary together");
			}
			seccomp = Filter;
			minijail_use_seccomp_filter(j);
			filter_path = optarg;
			use_seccomp_filter = 1;
			break;
		case 'l':
			minijail_namespace_ipc(j);
			break;
		case 'L':
			if (seccomp == BpfBinaryFilter) {
				errx(1, "-L does not work with "
					"--seccomp-bpf-binary");
			}
			use_seccomp_log = 1;
			minijail_log_seccomp_filter_failures(j);
			break;
		case 'b':
			add_binding(j, optarg);
			binding = 1;
			break;
		case 'B':
			skip_securebits(j, optarg);
			break;
		case 'c':
			caps = 1;
			use_caps(j, optarg);
			break;
		case 'C':
			use_chroot(j, optarg, &chroot, pivot_root);
			break;
		case 'k':
			add_mount(j, optarg);
			break;
		case 'K':
			remount_mode = optarg;
			change_remount = 1;
			break;
		case 'P':
			use_pivot_root(j, optarg, &pivot_root, chroot);
			break;
		case 'f':
			if (0 != minijail_write_pid_file(j, optarg))
				errx(1, "Could not prepare pid file path");
			break;
		case 't':
			minijail_namespace_vfs(j);
			if (!tmp_size) {
				/*
				 * Avoid clobbering |tmp_size| if it was already
				 * set.
				 */
				tmp_size = DEFAULT_TMP_SIZE;
			}
			if (optarg != NULL &&
			    0 != parse_size(&tmp_size, optarg)) {
				errx(1, "Invalid /tmp tmpfs size");
			}
			break;
		case 'v':
			minijail_namespace_vfs(j);
			/*
			 * Set the default mount propagation in the command-line
			 * tool to MS_SLAVE.
			 *
			 * When executing the sandboxed program in a new mount
			 * namespace the Minijail library will by default
			 * remount all mounts with the MS_PRIVATE flag. While
			 * this is an appropriate, safe default for the library,
			 * MS_PRIVATE can be problematic: unmount events will
			 * not propagate into mountpoints marked as MS_PRIVATE.
			 * This means that if a mount is unmounted in the root
			 * mount namespace, it will not be unmounted in the
			 * non-root mount namespace.
			 * This in turn can be problematic because activity in
			 * the non-root mount namespace can now directly
			 * influence the root mount namespace (e.g. preventing
			 * re-mounts of said mount), which would be a privilege
			 * inversion.
			 *
			 * Setting the default in the command-line to MS_SLAVE
			 * will still prevent mounts from leaking out of the
			 * non-root mount namespace but avoid these
			 * privilege-inversion issues.
			 * For cases where mounts should not flow *into* the
			 * namespace either, the user can pass -Kprivate.
			 * Note that mounts are marked as MS_PRIVATE by default
			 * by the kernel, so unless the init process (like
			 * systemd) or something else marks them as shared, this
			 * won't do anything.
			 */
			minijail_remount_mode(j, MS_SLAVE);
			mount_ns = 1;
			break;
		case 'V':
			minijail_namespace_enter_vfs(j, optarg);
			break;
		case 'r':
			minijail_remount_proc_readonly(j);
			break;
		case 'G':
			if (keep_suppl_gids)
				errx(1, "-y and -G are not compatible");
			minijail_inherit_usergroups(j);
			inherit_suppl_gids = 1;
			break;
		case 'y':
			if (inherit_suppl_gids)
				errx(1, "-y and -G are not compatible");
			minijail_keep_supplementary_gids(j);
			keep_suppl_gids = 1;
			break;
		case 'N':
			minijail_namespace_cgroups(j);
			break;
		case 'p':
			minijail_namespace_pids(j);
			break;
		case 'e':
			if (optarg)
				minijail_namespace_enter_net(j, optarg);
			else
				minijail_namespace_net(j);
			break;
		case 'i':
			*exit_immediately = 1;
			break;
		case 'H':
			seccomp_filter_usage(argv[0]);
			exit(0);
		case 'I':
			minijail_namespace_pids(j);
			minijail_run_as_init(j);
			break;
		case 'U':
			minijail_namespace_user(j);
			minijail_namespace_pids(j);
			break;
		case 'm':
			set_uidmap = 1;
			if (uidmap) {
				free(uidmap);
				uidmap = NULL;
			}
			if (optarg)
				uidmap = xstrdup(optarg);
			break;
		case 'M':
			set_gidmap = 1;
			if (gidmap) {
				free(gidmap);
				gidmap = NULL;
			}
			if (optarg)
				gidmap = xstrdup(optarg);
			break;
		case 'a':
			if (0 != minijail_use_alt_syscall(j, optarg))
				errx(1, "Could not set alt-syscall table");
			break;
		case 'R':
			add_rlimit(j, optarg);
			break;
		case 'T':
			if (!strcmp(optarg, "static"))
				*elftype = ELFSTATIC;
			else if (!strcmp(optarg, "dynamic"))
				*elftype = ELFDYNAMIC;
			else {
				errx(1, "ELF type must be 'static' or "
					"'dynamic'");
			}
			break;
		case 'w':
			minijail_new_session_keyring(j);
			break;
		case 'Y':
			minijail_set_seccomp_filter_tsync(j);
			break;
		case 'z':
			forward = 0;
			break;
		case 'd':
			minijail_namespace_vfs(j);
			minijail_mount_dev(j);
			break;
		/* Long options. */
		case OPT_AMBIENT:
			ambient_caps = 1;
			minijail_set_ambient_caps(j);
			break;
		case OPT_UTS:
			minijail_namespace_uts(j);
			if (optarg)
				minijail_namespace_set_hostname(j, optarg);
			break;
		case OPT_LOGGING:
			if (!strcmp(optarg, "auto"))
				log_to_stderr = -1;
			else if (!strcmp(optarg, "syslog"))
				log_to_stderr = 0;
			else if (!strcmp(optarg, "stderr"))
				log_to_stderr = 1;
			else
				errx(1,
				     "--logger must be 'syslog' or 'stderr'");
			break;
		case OPT_PROFILE:
			use_profile(j, optarg, &pivot_root, chroot, &tmp_size);
			break;
		case OPT_PRELOAD_LIBRARY:
			*preload_path = optarg;
			break;
		case OPT_SECCOMP_BPF_BINARY:
			if (seccomp != None && seccomp != BpfBinaryFilter) {
				errx(1, "Do not use -s, -S, or "
					"--seccomp-bpf-binary together");
			}
			if (use_seccomp_log == 1)
				errx(1, "-L does not work with "
					"--seccomp-bpf-binary");
			seccomp = BpfBinaryFilter;
			minijail_use_seccomp_filter(j);
			filter_path = optarg;
			use_seccomp_filter_binary = 1;
			break;
		case OPT_ADD_SUPPL_GROUP:
			suppl_group_add(&suppl_gids_count, &suppl_gids, optarg);
			break;
		case OPT_ALLOW_SPECULATIVE_EXECUTION:
			minijail_set_seccomp_filter_allow_speculation(j);
			break;
		case OPT_CONFIG: {
			if (conf_entry_list != NULL) {
				errx(1, "Nested config file specification is "
					"not allowed.");
			}
			conf_entry_list = new_config_entry_list();
			conf_index = 0;
#if defined(BLOCK_NOEXEC_CONF)
			/*
			 * Check the conf file is in a exec mount.
			 * With a W^X invariant, it excludes writable
			 * mounts.
			 */
			struct statfs conf_statfs;
			if (statfs(optarg, &conf_statfs) != 0)
				err(1, "statfs(%s) failed.", optarg);
			if ((conf_statfs.f_flags & MS_NOEXEC) != 0)
				errx(1,
				     "Conf file must be in a exec "
				     "mount: %s",
				     optarg);
#endif
#if defined(ENFORCE_ROOTFS_CONF)
			/* Make sure the conf file is in the same device as the
			 * rootfs. */
			struct stat root_stat;
			struct stat conf_stat;
			if (stat("/", &root_stat) != 0)
				err(1, "stat(/) failed.");
			if (stat(optarg, &conf_stat) != 0)
				err(1, "stat(%s) failed.", optarg);
			if (root_stat.st_dev != conf_stat.st_dev)
				errx(1, "Conf file must be in the rootfs.");
#endif
			attribute_cleanup_fp FILE *config_file =
			    fopen(optarg, "re");
			if (!config_file)
				err(1, "Failed to open %s", optarg);
			if (!parse_config_file(config_file, conf_entry_list)) {
				errx(
				    1,
				    "Unable to parse %s as Minijail conf file, "
				    "please refer to minijail0(5) for syntax "
				    "and examples.",
				    optarg);
			}
			break;
		}
		default:
			usage(argv[0]);
			exit(opt == 'h' ? 0 : 1);
		}
	}

	if (log_to_stderr == -1) {
		/* Autodetect default logging output. */
		log_to_stderr = isatty(STDIN_FILENO) ? 1 : 0;
	}
	if (log_to_stderr) {
		init_logging(LOG_TO_FD, STDERR_FILENO, LOG_INFO);
		/*
		 * When logging to stderr, ensure the FD survives the jailing.
		 */
		if (0 !=
		    minijail_preserve_fd(j, STDERR_FILENO, STDERR_FILENO)) {
			errx(1, "Could not preserve stderr");
		}
	}

	/* Set up uid/gid mapping. */
	if (set_uidmap || set_gidmap) {
		set_ugid_mapping(j, set_uidmap, uid, uidmap, set_gidmap, gid,
				 gidmap);
	}

	/* Can only set ambient caps when using regular caps. */
	if (ambient_caps && !caps) {
		errx(1, "Can't set ambient capabilities (--ambient) "
			"without actually using capabilities (-c)");
	}

	/* Set up signal handlers in minijail unless asked not to. */
	if (forward)
		minijail_forward_signals(j);

	/*
	 * Only allow bind mounts when entering a chroot, using pivot_root, or
	 * a new mount namespace.
	 */
	if (binding && !(chroot || pivot_root || mount_ns)) {
		errx(1, "Bind mounts require a chroot, pivot_root, or "
			" new mount namespace");
	}

	/*
	 * / is only remounted when entering a new mount namespace, so unless
	 * that's set there is no need for the -K/-K<mode> flags.
	 */
	if (change_remount && !mount_ns) {
		errx(1, "No need to use -K (skip remounting '/') or "
			"-K<mode> (remount '/' as <mode>) "
			"without -v (new mount namespace).\n"
			"Do you need to add '-v' explicitly?");
	}

	/* Configure the remount flag here to avoid having -v override it. */
	if (change_remount) {
		if (remount_mode != NULL) {
			set_remount_mode(j, remount_mode);
		} else {
			minijail_skip_remount_private(j);
		}
	}

	/*
	 * Proceed in setting the supplementary gids specified on the
	 * cmdline options.
	 */
	if (suppl_gids_count) {
		minijail_set_supplementary_gids(j, suppl_gids_count,
						suppl_gids);
		free(suppl_gids);
	}

	/*
	 * We parse seccomp filters here to make sure we've collected all
	 * cmdline options.
	 */
	if (use_seccomp_filter) {
		minijail_parse_seccomp_filters(j, filter_path);
	} else if (use_seccomp_filter_binary) {
		struct sock_fprog filter;
		read_seccomp_filter(filter_path, &filter);
		minijail_set_seccomp_filters(j, &filter);
		free((void *)filter.filter);
	}

	/* Mount a tmpfs under /tmp and set its size. */
	if (tmp_size)
		minijail_mount_tmp_size(j, tmp_size);

	/*
	 * There should be at least one additional unparsed argument: the
	 * executable name.
	 */
	if (argc == optind) {
		usage(argv[0]);
		exit(1);
	}

	if (*elftype == ELFERROR) {
		/*
		 * -T was not specified.
		 * Get the path to the program adjusted for changing root.
		 */
		char *program_path =
		    minijail_get_original_path(j, argv[optind]);

		/* Check that we can access the target program. */
		if (access(program_path, X_OK)) {
			errx(1, "Target program '%s' is not accessible",
			     argv[optind]);
		}

		/* Check if target is statically or dynamically linked. */
		*elftype = get_elf_linkage(program_path);
		free(program_path);
	}

	/*
	 * Setting capabilities need either a dynamically-linked binary, or the
	 * use of ambient capabilities for them to be able to survive an
	 * execve(2).
	 */
	if (caps && *elftype == ELFSTATIC && !ambient_caps) {
		errx(1, "Can't run statically-linked binaries with capabilities"
			" (-c) without also setting ambient capabilities. "
			"Try passing --ambient.");
	}

	return optind;
}
