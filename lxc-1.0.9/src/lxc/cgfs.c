/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <net/if.h>

#include "error.h"
#include "commands.h"
#include "list.h"
#include "conf.h"
#include "utils.h"
#include "bdev.h"
#include "log.h"
#include "cgroup.h"
#include "start.h"
#include "state.h"

#if IS_BIONIC
#include <../include/lxcmntent.h>
#else
#include <mntent.h>
#endif

struct cgroup_hierarchy;
struct cgroup_meta_data;
struct cgroup_mount_point;

/*
 * cgroup_meta_data: the metadata about the cgroup infrastructure on this
 *                   host
 */ //cgfs_data.meta成员
struct cgroup_meta_data { //初始化使用见cgfs_init  
	ptrdiff_t ref; /* simple refcount */
	struct cgroup_hierarchy **hierarchies; ////proc/self/cgroup中的信息  find_cgroup_hierarchies
	struct cgroup_mount_point **mount_points; //cgroup mount /proc/self/mountinf相关信息存入该结构中，见find_hierarchy_mountpts o
	int maximum_hierarchy; //赋值见find_cgroup_hierarchies
};

/*
 * cgroup_hierarchy: describes a single cgroup hierarchy
 *                   (may have multiple mount points)
 */
struct cgroup_hierarchy { //cgroup_meta_data.hierarchies成员
	int index; ///proc/self/cgroup中每行的子系统编号
	bool used; /* false if the hierarchy should be ignored by lxc */
	char **subsystems; //例如2:net_prio,net_cls:/中的net_prio,net_cls  子系统列表信息 net_prio,net_cls会被拆分成net_prio net_cls占用两个数组位


    //需要保证mount的挂载点必须在/proc/cgroups中存在
	//cgroup on /sys/fs/cgroup/devices type cgroup (rw,nosuid,nodev,noexec,relatime,devices)满足要求
	//cgroup on /xxaa/bb type cgroup (rw,nosuid,nodev,noexec,relatime,devices)不满足要求

	//只读的mount point加入ro_absolute_mount_point  其他的加入rw_absolute_mount_point  只存放从更目录mount的
	struct cgroup_mount_point *rw_absolute_mount_point;
	struct cgroup_mount_point *ro_absolute_mount_point;
	struct cgroup_mount_point **all_mount_points; //所有的mount，包括跟目录的和非根目录的
	size_t all_mount_point_capacity;
};

/*
 * cgroup_mount_point: a mount point to where a hierarchy
 *                     is mounted to
 */
 //cgroup_process_info.designated_mount_point
struct cgroup_mount_point {//参考find_hierarchy_mountpts
	struct cgroup_hierarchy *hierarchy;
	//cgroup on /sys/fs/cgroup/devices type cgroup (rw,nosuid,nodev,noexec,relatime,devices)中的/sys/fs/cgroup/devices
	char *mount_point;//挂载点
	char *mount_prefix; //lxcfs默认为/     赋值见find_hierarchy_mountpts
	bool read_only; //是否只读
	//如果是linux内核版本比较老，则需要我们代码自己初始化，生效见handle_cgroup_settings  init_cpuset_if_needed
	bool need_cpuset_init; //是否INIT
};

/*
 * cgroup_process_info: describes the membership of a
 *                      process to the different cgroup
 *                      hierarchies
 *
 * Note this is the per-process info tracked by the cgfs_ops.
 * This is not used with cgmanager.
 */ //cgfs_data.info
struct cgroup_process_info { 
	struct cgroup_process_info *next; //cgroup_process_info通过next链接到一起，可以参考cgroup_process_info
	struct cgroup_meta_data *meta_ref;
	struct cgroup_hierarchy *hierarchy; //cgourp文件每行的最前面的数字  如11:pids:/ 中的11
	char *cgroup_path; //cgourp中的例如11:pids:/ 中的/    创建/sys/fs/cgroup/lxc/xxx等相关目录后，会重新赋值为/lxc/yyz-test
	char *cgroup_path_sub;
	//lxc_cgroupfs_create 或者 lxc_cgroup_create_legacy 中赋值
	char **created_paths; //创建的文件路径，例如/sys/fs/cgroup/pids/lxc   /sys/fs/cgroup/pids/lxc/yyz-test 等都存入该数组
	size_t created_paths_capacity;
	size_t created_paths_count;
	//使用见lxc_cgroupfs_create   这里面一般是mount的cgroup文件系统挂载点  
	struct cgroup_mount_point *designated_mount_point; //例如/sys/fs/cgroup/blkio  /sys/fs/cgroup/cpu,cpuacct等
};

struct cgfs_data { //初始化见cgfs_init
	char *name; //容器名
	const char *cgroup_pattern; //默认//默认/lxc/%n  DEFAULT_CGROUP_PATTERN
	struct cgroup_meta_data *meta; //赋值见lxc_cgroup_load_meta2
	struct cgroup_process_info *info; //见cgfs_create
};

lxc_log_define(lxc_cgfs, lxc);

static struct cgroup_process_info *lxc_cgroup_process_info_getx(const char *proc_pid_cgroup_str, struct cgroup_meta_data *meta);
static char **subsystems_from_mount_options(const char *mount_options, char **kernel_list);
static void lxc_cgroup_mount_point_free(struct cgroup_mount_point *mp);
static void lxc_cgroup_hierarchy_free(struct cgroup_hierarchy *h);
static bool is_valid_cgroup(const char *name);
static int create_cgroup(struct cgroup_mount_point *mp, const char *path);
static int remove_cgroup(struct cgroup_mount_point *mp, const char *path, bool recurse);
static char *cgroup_to_absolute_path(struct cgroup_mount_point *mp, const char *path, const char *suffix);
static struct cgroup_process_info *find_info_for_subsystem(struct cgroup_process_info *info, const char *subsystem);
static int do_cgroup_get(const char *cgroup_path, const char *sub_filename, char *value, size_t len);
static int do_cgroup_set(const char *cgroup_path, const char *sub_filename, const char *value);
static bool cgroup_devices_has_allow_or_deny(struct cgfs_data *d, char *v, bool for_allow);
static int do_setup_cgroup_limits(struct cgfs_data *d, struct lxc_list *cgroup_settings, bool do_devices);
static int cgroup_recursive_task_count(const char *cgroup_path);
static int count_lines(const char *fn);
static int handle_cgroup_settings(struct cgroup_mount_point *mp, char *cgroup_path);
static bool init_cpuset_if_needed(struct cgroup_mount_point *mp, const char *path);

static struct cgroup_meta_data *lxc_cgroup_load_meta2(const char **subsystem_whitelist);
static struct cgroup_meta_data *lxc_cgroup_get_meta(struct cgroup_meta_data *meta_data);
static struct cgroup_meta_data *lxc_cgroup_put_meta(struct cgroup_meta_data *meta_data);

/* free process membership information */
static void lxc_cgroup_process_info_free(struct cgroup_process_info *info);
static void lxc_cgroup_process_info_free_and_remove(struct cgroup_process_info *info);

static struct cgroup_ops cgfs_ops;

static int cgroup_rmdir(char *dirname)
{
	struct dirent *direntp;
	int saved_errno = 0;
	DIR *dir;
	int ret, failed=0;
	char pathname[MAXPATHLEN];

	dir = opendir(dirname);
	if (!dir) {
		ERROR("%s: failed to open %s", __func__, dirname);
		return -1;
	}

	while ((direntp = readdir(dir))) {
		struct stat mystat;
		int rc;

		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;

		rc = snprintf(pathname, MAXPATHLEN, "%s/%s", dirname, direntp->d_name);
		if (rc < 0 || rc >= MAXPATHLEN) {
			ERROR("pathname too long");
			failed=1;
			if (!saved_errno)
				saved_errno = -ENOMEM;
			continue;
		}
		ret = lstat(pathname, &mystat);
		if (ret) {
			SYSERROR("%s: failed to stat %s", __func__, pathname);
			failed=1;
			if (!saved_errno)
				saved_errno = errno;
			continue;
		}
		if (S_ISDIR(mystat.st_mode)) {
			if (cgroup_rmdir(pathname) < 0) {
				if (!saved_errno)
					saved_errno = errno;
				failed=1;
			}
		}
	}

	if (rmdir(dirname) < 0) {
		SYSERROR("%s: failed to delete %s", __func__, dirname);
		if (!saved_errno)
			saved_errno = errno;
		failed=1;
	}

	ret = closedir(dir);
	if (ret) {
		SYSERROR("%s: failed to close directory %s", __func__, dirname);
		if (!saved_errno)
			saved_errno = errno;
		failed=1;
	}

	errno = saved_errno;
	return failed ? -1 : 0;
}

static struct cgroup_meta_data *lxc_cgroup_load_meta()
{
	const char *cgroup_use = NULL;
	char **cgroup_use_list = NULL;
	struct cgroup_meta_data *md = NULL;
	int saved_errno;

	errno = 0;
	cgroup_use = lxc_global_config_value("lxc.cgroup.use");
	if (!cgroup_use && errno != 0)
		return NULL;
	if (cgroup_use) {
		cgroup_use_list = lxc_string_split_and_trim(cgroup_use, ',');
		if (!cgroup_use_list)
			return NULL;
	}

	//printf("yang test ..... cgroup_use:%s\r\n", cgroup_use); //打印NULL
	md = lxc_cgroup_load_meta2((const char **)cgroup_use_list);
	saved_errno = errno;
	lxc_free_array((void **)cgroup_use_list, free);
	errno = saved_errno;
	return md;
}

//解析/proc/cgroups中的内容到kernel_subsystems数组
/* Step 1: determine all kernel subsystems */ 
//3个step分别为find_cgroup_subsystems(/proc/cgroups)   find_cgroup_hierarchies(/proc/self/cgroup)  find_hierarchy_mountpts(/proc/self/mountinfo)
static bool find_cgroup_subsystems(char ***kernel_subsystems)
{
	FILE *proc_cgroups;
	bool bret = false;
	char *line = NULL;
	size_t sz = 0;
	size_t kernel_subsystems_count = 0;
	size_t kernel_subsystems_capacity = 0;
	int r;

    /*
    [root@localhost lxc-lxc-1.0.9]# cat /proc/cgroups
    #subsys_name    hierarchy       num_cgroups     enabled
    cpuset  7       2       1
    cpu     5       2       1
    cpuacct 5       2       1
    memory  6       2       1
    devices 3       18      1
    freezer 10      2       1
    net_cls 2       2       1
    blkio   8       2       1
    perf_event      4       2       1
    hugetlb 9       2       1
    pids    11      2       1
    net_prio        2       2       1
    */
	proc_cgroups = fopen_cloexec("/proc/cgroups", "r");
	if (!proc_cgroups)
		return false;

	while (getline(&line, &sz, proc_cgroups) != -1) {
		char *tab1;
		char *tab2;
		int hierarchy_number; //hierarchy_number   为hierarchy列中对应的内容

		if (line[0] == '#') //跳过#开始的行，例如#subsys_name    hierarchy       num_cgroups     enabled
			continue;
		if (!line[0])
			continue;

		tab1 = strchr(line, '\t');
		if (!tab1)
			continue;
		*tab1++ = '\0';
		tab2 = strchr(tab1, '\t');
		if (!tab2)
			continue;
		*tab2 = '\0';

		tab2 = NULL;
		hierarchy_number = strtoul(tab1, &tab2, 10);
		if (!tab2 || *tab2)
			continue;
		(void)hierarchy_number;

		r = lxc_grow_array((void ***)kernel_subsystems, &kernel_subsystems_capacity, kernel_subsystems_count + 1, 12);
		if (r < 0)
			goto out;
		(*kernel_subsystems)[kernel_subsystems_count] = strdup(line); //cpuset  7       2       1行中的cpuset,代表subsys_name
		if (!(*kernel_subsystems)[kernel_subsystems_count])
			goto out;
		kernel_subsystems_count++;

        // ...........cpuset, 7  1
        //...........cpu, 5  2
        //
		//printf("...........%s, %d  %d\r\n",  line, hierarchy_number, (int)kernel_subsystems_count);
	}
	bret = true;

out:
	fclose(proc_cgroups);
	free(line);
	return bret;
}

/* Step 2: determine all hierarchies (by reading /proc/self/cgroup),
 *         since mount points don't specify hierarchy number and
 *         /proc/cgroups does not contain named hierarchies
 */ 

//3个step分别为find_cgroup_subsystems(/proc/cgroups)   find_cgroup_hierarchies(/proc/self/cgroup)  find_hierarchy_mountpts(/proc/self/mountinfo)
//注意和lxc_cgroupfs_create->lxc_cgroup_process_info_get_init的区别
static bool find_cgroup_hierarchies(struct cgroup_meta_data *meta_data,
	bool all_kernel_subsystems, bool all_named_subsystems,
	const char **subsystem_whitelist)
{
	FILE *proc_self_cgroup;
	char *line = NULL;
	size_t sz = 0;
	int r;
	bool bret = false;
	size_t hierarchy_capacity = 0;

	proc_self_cgroup = fopen_cloexec("/proc/self/cgroup", "r");
	/* if for some reason (because of setns() and pid namespace for example),
	 * /proc/self is not valid, we try /proc/1/cgroup... */
	if (!proc_self_cgroup)
		proc_self_cgroup = fopen_cloexec("/proc/1/cgroup", "r");
	if (!proc_self_cgroup)
		return false;

	while (getline(&line, &sz, proc_self_cgroup) != -1) {
		/* file format: hierarchy:subsystems:group,
		 * we only extract hierarchy and subsystems
		 * here */
		char *colon1;
		char *colon2;
		int hierarchy_number;
		struct cgroup_hierarchy *h = NULL;
		char **p;

		if (!line[0])
			continue;

		colon1 = strchr(line, ':');
		if (!colon1)
			continue;
		*colon1++ = '\0';
		colon2 = strchr(colon1, ':');
		if (!colon2)
			continue;
		*colon2 = '\0';

		colon2 = NULL;

		/* With cgroupv2 /proc/self/cgroup can contain entries of the
		 * form: 0::/
		 * These entries need to be skipped.
		 */
		if (!strcmp(colon1, ""))
			continue;

		hierarchy_number = strtoul(line, &colon2, 10);
		if (!colon2 || *colon2)
			continue;

		if (hierarchy_number > meta_data->maximum_hierarchy) {
			/* lxc_grow_array will never shrink, so even if we find a lower
			* hierarchy number here, the array will never be smaller
			*/
			r = lxc_grow_array((void ***)&meta_data->hierarchies, &hierarchy_capacity, hierarchy_number + 1, 12);
			if (r < 0)
				goto out;

			meta_data->maximum_hierarchy = hierarchy_number;
		}

		/* this shouldn't happen, we had this already */
		if (meta_data->hierarchies[hierarchy_number])
			goto out;

		h = calloc(1, sizeof(struct cgroup_hierarchy));
		if (!h)
			goto out;

		meta_data->hierarchies[hierarchy_number] = h;

		h->index = hierarchy_number;
		h->subsystems = lxc_string_split_and_trim(colon1, ',');
		//yang test ...dd......5, cpuacct,cpu
		//printf("yang test ...dd......%d, %s\r\n", hierarchy_number, colon1);
		if (!h->subsystems)
			goto out;
		/* see if this hierarchy should be considered */
		if (!all_kernel_subsystems || !all_named_subsystems) {
			for (p = h->subsystems; *p; p++) {
				if (!strncmp(*p, "name=", 5)) {
					if (all_named_subsystems || (subsystem_whitelist && lxc_string_in_array(*p, subsystem_whitelist))) {
						h->used = true;
						break;
					}
				} else {
					if (all_kernel_subsystems || (subsystem_whitelist && lxc_string_in_array(*p, subsystem_whitelist))) {
						h->used = true;
						break;
					}
				}
			}
		} else {
			/* we want all hierarchy anyway */
			h->used = true;
		}
	}
	bret = true;

out:
	fclose(proc_self_cgroup);
	free(line);
	return bret;
}

/*
[root@localhost lxc-lxc-1.0.9]# cat /proc/self/mountinfo
17 61 0:16 / /sys rw,nosuid,nodev,noexec,relatime shared:6 - sysfs sysfs rw,seclabel
18 61 0:3 / /proc rw,nosuid,nodev,noexec,relatime shared:5 - proc proc rw
19 61 0:5 / /dev rw,nosuid shared:2 - devtmpfs devtmpfs rw,seclabel,size=1438752k,nr_inodes=359688,mode=755
20 17 0:15 / /sys/kernel/security rw,nosuid,nodev,noexec,relatime shared:7 - securityfs securityfs rw
21 19 0:17 / /dev/shm rw,nosuid,nodev shared:3 - tmpfs tmpfs rw,seclabel
22 19 0:11 / /dev/pts rw,nosuid,noexec,relatime shared:4 - devpts devpts rw,seclabel,gid=5,mode=620,ptmxmode=000
*/

/* Step 3: determine all mount points of each hierarchy */

//3个step分别为find_cgroup_subsystems(/proc/cgroups)   find_cgroup_hierarchies(/proc/self/cgroup)  find_hierarchy_mountpts(/proc/self/mountinfo)
//kernel_subsystems是从proc/cgroups获取的内容  //需要保证mount的挂载点必须在/proc/cgroups中存在
static bool find_hierarchy_mountpts( struct cgroup_meta_data *meta_data, char **kernel_subsystems) 
{
	bool bret = false;
	FILE *proc_self_mountinfo;
	char *line = NULL;
	size_t sz = 0;
	char **tokens = NULL;
	size_t mount_point_count = 0;
	size_t mount_point_capacity = 0;
	size_t token_capacity = 0;
	int r;

	proc_self_mountinfo = fopen_cloexec("/proc/self/mountinfo", "r");
	/* if for some reason (because of setns() and pid namespace for example),
	 * /proc/self is not valid, we try /proc/1/cgroup... */
	if (!proc_self_mountinfo)
		proc_self_mountinfo = fopen_cloexec("/proc/1/mountinfo", "r");
	if (!proc_self_mountinfo)
		return false;

	while (getline(&line, &sz, proc_self_mountinfo) != -1) {
		char *token, *line_tok, *saveptr = NULL;
		size_t i, j, k;
		struct cgroup_mount_point *mount_point;
		struct cgroup_hierarchy *h;
		char **subsystems;
		bool is_lxcfs = false;

		if (line[0] && line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';

		for (i = 0, line_tok = line; (token = strtok_r(line_tok, " ", &saveptr)); line_tok = NULL) {
			r = lxc_grow_array((void ***)&tokens, &token_capacity, i + 1, 64);
			if (r < 0)
				goto out;
			tokens[i++] = token;
		}

		/* layout of /proc/self/mountinfo:
		 *      0: id
		 *      1: parent id
		 *      2: device major:minor
		 *      3: mount prefix
		 *      4: mount point
		 *      5: per-mount options
		 *    [optional X]: additional data
		 *    X+7: "-"
		 *    X+8: type
		 *    X+9: source
		 *    X+10: per-superblock options
		 */
		for (j = 6; j < i && tokens[j]; j++)
			if (!strcmp(tokens[j], "-"))
				break;

		/* could not find separator */
		if (j >= i || !tokens[j])
			continue;
		/* there should be exactly three fields after
		 * the separator
		 */
		if (i != j + 4)
			continue;

		/* not a cgroup filesystem */  //只获取cgroup文件系统
		if (strcmp(tokens[j + 1], "cgroup") != 0) {
			if (strcmp(tokens[j + 1], "fuse.lxcfs") != 0)
				continue;
			if (strncmp(tokens[4], "/sys/fs/cgroup/", 15) != 0)
				continue;
			is_lxcfs = true;
			char *curtok = tokens[4] + 15;
			subsystems = subsystems_from_mount_options(curtok,
							 kernel_subsystems);
		} else
			subsystems = subsystems_from_mount_options(tokens[j + 3],
							 kernel_subsystems);
		if (!subsystems)
			goto out;

		h = NULL;
		for (k = 0; k <= meta_data->maximum_hierarchy; k++) { 
		//需要保证mount的挂载点必须在/proc/cgroups中存在
		//cgroup on /sys/fs/cgroup/devices type cgroup (rw,nosuid,nodev,noexec,relatime,devices)满足要求
		//cgroup on /xxaa/bb type cgroup (rw,nosuid,nodev,noexec,relatime,devices)不满足要求
			if (meta_data->hierarchies[k] &&
			    meta_data->hierarchies[k]->subsystems[0] &&
			    lxc_string_in_array(meta_data->hierarchies[k]->subsystems[0], (const char **)subsystems)) {
				/* TODO: we could also check if the lists really match completely,
				 *       just to have an additional sanity check */
				h = meta_data->hierarchies[k];
				break;
			}
		}
		lxc_free_array((void **)subsystems, free);

		r = lxc_grow_array((void ***)&meta_data->mount_points, &mount_point_capacity, mount_point_count + 1, 12);
		if (r < 0)
			goto out;

		/* create mount point object */
		mount_point = calloc(1, sizeof(*mount_point));
		if (!mount_point)
			goto out;

		meta_data->mount_points[mount_point_count++] = mount_point;

		mount_point->hierarchy = h;
		if (is_lxcfs)
			mount_point->mount_prefix = strdup("/");
		else
			mount_point->mount_prefix = strdup(tokens[3]);
		mount_point->mount_point = strdup(tokens[4]);
		if (!mount_point->mount_point || !mount_point->mount_prefix)
			goto out;
		mount_point->read_only = !lxc_string_in_list("rw", tokens[5], ',');

		if (!strcmp(mount_point->mount_prefix, "/")) {
			if (mount_point->read_only) {
				if (!h->ro_absolute_mount_point) //只读的mount point加入ro_absolute_mount_point
					h->ro_absolute_mount_point = mount_point;
			} else {
				if (!h->rw_absolute_mount_point) //其他的加入rw_absolute_mount_point
					h->rw_absolute_mount_point = mount_point;
			}
		}

        /*
         ....mount_prefix:/, mount_point:/sys/fs/cgroup/systemd, read_only:0
         ....mount_prefix:/, mount_point:/sys/fs/cgroup/net_cls,net_prio, read_only:0
         ....mount_prefix:/, mount_point:/sys/fs/cgroup/devices, read_only:0
		printf(" ....mount_prefix:%s, mount_point:%s, read_only:%d\r\n",
		    mount_point->mount_prefix, mount_point->mount_point, mount_point->read_only);
        */
        
		k = lxc_array_len((void **)h->all_mount_points);
		r = lxc_grow_array((void ***)&h->all_mount_points, &h->all_mount_point_capacity, k + 1, 4);
		if (r < 0)
			goto out;
		h->all_mount_points[k] = mount_point;
	}
	bret = true;

out:
	fclose(proc_self_mountinfo);
	free(tokens);
	free(line);
	return bret;
}

static struct cgroup_meta_data *lxc_cgroup_load_meta2(const char **subsystem_whitelist)
{
	bool all_kernel_subsystems = true;
	bool all_named_subsystems = false;
	struct cgroup_meta_data *meta_data = NULL;
	char **kernel_subsystems = NULL;
	int saved_errno = 0;

	/* if the subsystem whitelist is not specified, include all
	 * hierarchies that contain kernel subsystems by default but
	 * no hierarchies that only contain named subsystems
	 *
	 * if it is specified, the specifier @all will select all
	 * hierarchies, @kernel will select all hierarchies with
	 * kernel subsystems and @named will select all named
	 * hierarchies
	 */
	all_kernel_subsystems = subsystem_whitelist ?
		(lxc_string_in_array("@kernel", subsystem_whitelist) || lxc_string_in_array("@all", subsystem_whitelist)) :
		true;
	all_named_subsystems = subsystem_whitelist ?
		(lxc_string_in_array("@named", subsystem_whitelist) || lxc_string_in_array("@all", subsystem_whitelist)) :
		false;

	meta_data = calloc(1, sizeof(struct cgroup_meta_data));
	if (!meta_data)
		return NULL;
	meta_data->ref = 1;

    //解析/proc/cgroups中的内容到kernel_subsystems数组
	if (!find_cgroup_subsystems(&kernel_subsystems)) 
		goto out_error;

	if (!find_cgroup_hierarchies(meta_data, all_kernel_subsystems,
				all_named_subsystems, subsystem_whitelist))
		goto out_error;

	if (!find_hierarchy_mountpts(meta_data, kernel_subsystems))
		goto out_error;

	/* oops, we couldn't find anything */
	if (!meta_data->hierarchies || !meta_data->mount_points) {
		errno = EINVAL;
		goto out_error;
	}

	lxc_free_array((void **)kernel_subsystems, free);
	return meta_data;

out_error:
	saved_errno = errno;
	lxc_free_array((void **)kernel_subsystems, free);
	lxc_cgroup_put_meta(meta_data);
	errno = saved_errno;
	return NULL;
}

static struct cgroup_meta_data *lxc_cgroup_get_meta(struct cgroup_meta_data *meta_data)
{
	meta_data->ref++;
	return meta_data;
}

static struct cgroup_meta_data *lxc_cgroup_put_meta(struct cgroup_meta_data *meta_data)
{
	size_t i;
	if (!meta_data)
		return NULL;
	if (--meta_data->ref > 0)
		return meta_data;
	lxc_free_array((void **)meta_data->mount_points, (lxc_free_fn)lxc_cgroup_mount_point_free);
	if (meta_data->hierarchies)
		for (i = 0; i <= meta_data->maximum_hierarchy; i++)
			if (meta_data->hierarchies[i])
				lxc_cgroup_hierarchy_free(meta_data->hierarchies[i]);
	free(meta_data->hierarchies);
	free(meta_data);
	return NULL;
}

static struct cgroup_hierarchy *lxc_cgroup_find_hierarchy(struct cgroup_meta_data *meta_data, const char *subsystem)
{
	size_t i;
	for (i = 0; i <= meta_data->maximum_hierarchy; i++) {
		struct cgroup_hierarchy *h = meta_data->hierarchies[i];
		if (!h)
			continue;
		if (h && lxc_string_in_array(subsystem, (const char **)h->subsystems))
			return h;
	}
	return NULL;
}

static bool mountpoint_is_accessible(struct cgroup_mount_point *mp)
{
	return mp && access(mp->mount_point, F_OK) == 0;
}

static struct cgroup_mount_point *lxc_cgroup_find_mount_point(struct cgroup_hierarchy *hierarchy, const char *group, bool should_be_writable)
{
	struct cgroup_mount_point **mps;
	struct cgroup_mount_point *current_result = NULL;
	ssize_t quality = -1;

	/* trivial case */  //mount点不是/的直接返回
	if (mountpoint_is_accessible(hierarchy->rw_absolute_mount_point))
		return hierarchy->rw_absolute_mount_point;
	if (!should_be_writable && mountpoint_is_accessible(hierarchy->ro_absolute_mount_point))
		return hierarchy->ro_absolute_mount_point;

	for (mps = hierarchy->all_mount_points; mps && *mps; mps++) {
		struct cgroup_mount_point *mp = *mps;
		size_t prefix_len = mp->mount_prefix ? strlen(mp->mount_prefix) : 0;

		if (prefix_len == 1 && mp->mount_prefix[0] == '/') 
			prefix_len = 0;

		if (!mountpoint_is_accessible(mp))
			continue;

		if (should_be_writable && mp->read_only)
			continue;

		if (!prefix_len ||
		    (strncmp(group, mp->mount_prefix, prefix_len) == 0 &&
		     (group[prefix_len] == '\0' || group[prefix_len] == '/'))) {
			/* search for the best quality match, i.e. the match with the
			 * shortest prefix where this group is still contained
			 */
			if (quality == -1 || prefix_len < quality) {
				current_result = mp;
				quality = prefix_len;
			}
		}
	}

	if (!current_result)
		errno = ENOENT;
	return current_result;
}

static char *lxc_cgroup_find_abs_path(const char *subsystem, const char *group, bool should_be_writable, const char *suffix)
{
	struct cgroup_meta_data *meta_data;
	struct cgroup_hierarchy *h;
	struct cgroup_mount_point *mp;
	char *result;
	int saved_errno;

	meta_data = lxc_cgroup_load_meta();
	if (!meta_data)
		return NULL;

	h = lxc_cgroup_find_hierarchy(meta_data, subsystem);
	if (!h)
		goto out_error;

	mp = lxc_cgroup_find_mount_point(h, group, should_be_writable);
	if (!mp)
		goto out_error;

	result = cgroup_to_absolute_path(mp, group, suffix);
	if (!result)
		goto out_error;

	lxc_cgroup_put_meta(meta_data);
	return result;

out_error:
	saved_errno = errno;
	lxc_cgroup_put_meta(meta_data);
	errno = saved_errno;
	return NULL;
}

static struct cgroup_process_info *lxc_cgroup_process_info_get(pid_t pid, struct cgroup_meta_data *meta)
{
	char pid_buf[32];
	snprintf(pid_buf, 32, "/proc/%lu/cgroup", (unsigned long)pid);
	return lxc_cgroup_process_info_getx(pid_buf, meta);
}

//3个step分别为find_cgroup_subsystems(/proc/cgroups)   find_cgroup_hierarchies(/proc/self/cgroup)  find_hierarchy_mountpts(/proc/self/mountinfo)
//注意和lxc_cgroupfs_create->lxc_cgroup_process_info_get_init的区别
static struct cgroup_process_info *lxc_cgroup_process_info_get_init(struct cgroup_meta_data *meta)
{
	return lxc_cgroup_process_info_get(1, meta);
}

/*
    子目录/proc/self本身就是当前运行进程ID的符号链接.

　　用ls -ld查看/proc/self目录的符号链接,发现每次都不一样,说明我们每次用ls命令时的进程ID都是不同的.

　　ls -ld /proc/self

　　lrwxrwxrwx 1 root root 64 2010-10-10 06:25 /proc/self -> 30525

*/ //获取当前运行的lxc进程的self信息
static struct cgroup_process_info *lxc_cgroup_process_info_get_self(struct cgroup_meta_data *meta)
{
	struct cgroup_process_info *i;
	i = lxc_cgroup_process_info_getx("/proc/self/cgroup", meta);
	if (!i)
		i = lxc_cgroup_process_info_get(getpid(), meta);
	return i;
}

/*
 * If a controller has ns cgroup mounted, then in that cgroup the handler->pid
 * is already in a new cgroup named after the pid.  'mnt' is passed in as
 * the full current cgroup.  Say that is /sys/fs/cgroup/lxc/2975 and the container
 * name is c1. .  We want to rename the cgroup directory to /sys/fs/cgroup/lxc/c1,
 * and return the string /sys/fs/cgroup/lxc/c1.
 */
static char *cgroup_rename_nsgroup(const char *mountpath, const char *oldname, pid_t pid, const char *name)
{
	char *dir, *fulloldpath;
	char *newname, *fullnewpath;
	int len, newlen, ret;

	/*
	 * if cgroup is mounted at /cgroup and task is in cgroup /ab/, pid 2375 and
	 * name is c1,
	 * dir: /ab
	 * fulloldpath = /cgroup/ab/2375
	 * fullnewpath = /cgroup/ab/c1
	 * newname = /ab/c1
	 */
	dir = alloca(strlen(oldname) + 1);
	strcpy(dir, oldname);

	len = strlen(oldname) + strlen(mountpath) + 22;
	fulloldpath = alloca(len);
	ret = snprintf(fulloldpath, len, "%s/%s/%ld", mountpath, oldname, (unsigned long)pid);
	if (ret < 0 || ret >= len)
		return NULL;

	len = strlen(dir) + strlen(name) + 2;
	newname = malloc(len);
	if (!newname) {
		SYSERROR("Out of memory");
		return NULL;
	}
	ret = snprintf(newname, len, "%s/%s", dir, name);
	if (ret < 0 || ret >= len) {
		free(newname);
		return NULL;
	}

	newlen = strlen(mountpath) + len + 2;
	fullnewpath = alloca(newlen);
	ret = snprintf(fullnewpath, newlen, "%s/%s", mountpath, newname);
	if (ret < 0 || ret >= newlen) {
		free(newname);
		return NULL;
	}

	if (access(fullnewpath, F_OK) == 0) {
		if (rmdir(fullnewpath) != 0) {
			SYSERROR("container cgroup %s already exists.", fullnewpath);
			free(newname);
			return NULL;
		}
	}
	if (rename(fulloldpath, fullnewpath)) {
		SYSERROR("failed to rename cgroup %s->%s", fulloldpath, fullnewpath);
		free(newname);
		return NULL;
	}

	DEBUG("'%s' renamed to '%s'", oldname, newname);
    
	return newname;
}

/* create a new cgroup */

//创建/sys/fs/cgroup/cpuacct/lxc/  /sys/fs/cgroup/cpuacct/lxc/yyz-test 及其他cgroup mount挂载点下面的lxc  lxc/yyz-test目录
static struct cgroup_process_info *lxc_cgroupfs_create(const char *name, const char *path_pattern, struct cgroup_meta_data *meta_data, const char *sub_pattern)
{
	char **cgroup_path_components = NULL;
	char **p = NULL;
	char *path_so_far = NULL; 
	char **new_cgroup_paths = NULL;
	char **new_cgroup_paths_sub = NULL;
	struct cgroup_mount_point *mp;
	struct cgroup_hierarchy *h;
	struct cgroup_process_info *base_info = NULL;
	struct cgroup_process_info *info_ptr;
	int saved_errno;
	int r;
	unsigned suffix = 0;
	bool had_sub_pattern = false;
	size_t i;

	if (!is_valid_cgroup(name)) {
		ERROR("Invalid cgroup name: '%s'", name);
		errno = EINVAL;
		return NULL;
	}

	if (!strstr(path_pattern, "%n")) { //默认/lxc/%n
		ERROR("Invalid cgroup path pattern: '%s'; contains no %%n for specifying container name", path_pattern);
		errno = EINVAL;
		return NULL;
	}

	/* we will modify the result of this operation directly,
	 * so we don't have to copy the data structure
	 */ //获取cgourp信息
	base_info = (path_pattern[0] == '/') ?
		lxc_cgroup_process_info_get_init(meta_data) :
		lxc_cgroup_process_info_get_self(meta_data);
	if (!base_info)
		return NULL;

	new_cgroup_paths = calloc(meta_data->maximum_hierarchy + 1, sizeof(char *));
	if (!new_cgroup_paths)
		goto out_initial_error;

	new_cgroup_paths_sub = calloc(meta_data->maximum_hierarchy + 1, sizeof(char *));
	if (!new_cgroup_paths_sub)
		goto out_initial_error;

	/* find mount points we can use */
	for (info_ptr = base_info; info_ptr; info_ptr = info_ptr->next) {
		h = info_ptr->hierarchy;
		if (!h)
			continue;

	    //printf("  ...........%s\r\n", info_ptr->cgroup_path);   打印/
		mp = lxc_cgroup_find_mount_point(h, info_ptr->cgroup_path, true);
		if (!mp) {
			ERROR("Could not find writable mount point for cgroup hierarchy %d while trying to create cgroup.", h->index);
			goto out_initial_error;
		}
		info_ptr->designated_mount_point = mp;

        /*
        .....haystatc:cpuacct    cgroup文件中的字系统名
        const char **haystack = (const char **)h->subsystems;
        for (; haystack && *haystack; haystack++)
            printf(".....haystatc:%s\r\n", (char*)*haystack);
        */
        
		if (lxc_string_in_array("ns", (const char **)h->subsystems)) //如ns子系统直接返回
			continue;
			
		if (handle_cgroup_settings(mp, info_ptr->cgroup_path) < 0) {
			ERROR("Could not set clone_children to 1 for cpuset hierarchy in parent cgroup.");
			goto out_initial_error;
		}
	}

	/* normalize the path */ //例如把/lxc/%n按照/拆分成lxc 和%n 存入cgroup_path_components数组，数组中就有了两个成员，一个lxc 一个%n
	cgroup_path_components = lxc_normalize_path(path_pattern);
	if (!cgroup_path_components)
		goto out_initial_error;

    //printf(" .......... %s.... %s\r\n", *cgroup_path_components, path_pattern); //lxc.... /lxc/%n
	/* go through the path components to see if we can create them */
	//该循环就是在cgroup挂载点上面创建/lxc  lxc/yyz_test目录
	for (p = cgroup_path_components; *p || (sub_pattern && !had_sub_pattern); p++) { 
	
		/* we only want to create the same component with -1, -2, etc.
		 * if the component contains the container name itself, otherwise
		 * it's not an error if it already exists
		 */
		char *p_eff = *p ? *p : (char *)sub_pattern;
		//p_eff是否%n，例如path_pattern为 /lxc/%n，则cgroup_path_components数组的第二个成员就是%n，和这里匹配
		bool contains_name = strstr(p_eff, "%n"); 
		char *current_component = NULL;
		char *current_subpath = NULL;
		char *current_entire_path = NULL;
		char *parts[3];
		size_t j = 0;
		i = 0;

		/* if we are processing the subpattern, we want to make sure
		 * loop is ended the next time around
		 */
		if (!*p) {
			had_sub_pattern = true;
			p--;
		}

		goto find_name_on_this_level;

	cleanup_name_on_this_level:
		/* This is reached if we found a name clash.
		 * In that case, remove the cgroup from all previous hierarchies
		 */
		for (j = 0, info_ptr = base_info; j < i && info_ptr; info_ptr = info_ptr->next, j++) {
			r = remove_cgroup(info_ptr->designated_mount_point, info_ptr->created_paths[info_ptr->created_paths_count - 1], false);
			if (r < 0)
				WARN("could not clean up cgroup we created when trying to create container");
			free(info_ptr->created_paths[info_ptr->created_paths_count - 1]);
			info_ptr->created_paths[--info_ptr->created_paths_count] = NULL;
		}
		if (current_component != current_subpath)
			free(current_subpath);
		if (current_component != p_eff)
			free(current_component);
		current_component = current_subpath = NULL;
		/* try again with another suffix */
		++suffix;

	find_name_on_this_level:
		/* determine name of the path component we should create */
		if (contains_name && suffix > 0) {
			char *buf = calloc(strlen(name) + 32, 1);
			if (!buf)
				goto out_initial_error;
			snprintf(buf, strlen(name) + 32, "%s-%u", name, suffix);
			current_component = lxc_string_replace("%n", buf, p_eff);
			free(buf);
		} else {
		    //如果是%n则用容器名替代
			current_component = contains_name ? lxc_string_replace("%n", name, p_eff) : p_eff;
			/*
			..................lxc  lxc
            ..................%n  yyz-test
			//printf("..................%s  %s\r\n", p_eff, current_component);
			*/
		}
		parts[0] = path_so_far; //记录上一次的current_subpath
		parts[1] = current_component; //本次的current_component
		parts[2] = NULL;
		current_subpath = path_so_far ? lxc_string_join("/", (const char **)parts, false) : current_component;

        /* /lxc/%n  被拆分成lxc  %n两个成员，其中%n前面被替换为容器名
		.........(null)  lxc  (null)  lxc
        ..lxc  yyz-test  (null)  lxc/yyz-test
		printf(".........%s  %s  %s  %s\r\n", parts[0], parts[1], parts[2], current_subpath);
		*/
		
		
		/* Now go through each hierarchy and try to create the
		 * corresponding cgroup
		 */
		for (i = 0, info_ptr = base_info; info_ptr; info_ptr = info_ptr->next, i++) {
			char *parts2[3];

			if (!info_ptr->hierarchy)
				continue;

            /*.....haystatc:pids
            .....haystatc:freezer
            .....
            const char **haystack = (const char **)info_ptr->hierarchy->subsystems;
            for (; haystack && *haystack; haystack++)
                printf(".....haystatc:%s\r\n", (char*)*haystack);
            */
			if (lxc_string_in_array("ns", (const char **)info_ptr->hierarchy->subsystems))
				continue;
			current_entire_path = NULL;

			parts2[0] = !strcmp(info_ptr->cgroup_path, "/") ? "" : info_ptr->cgroup_path;
			parts2[1] = current_subpath;
			parts2[2] = NULL;
			current_entire_path = lxc_string_join("/", (const char **)parts2, false);
			
			/*
			.........1:  2:lxc  3:(null)  4:/lxc
			..........
            .........1:  2:lxc/yyz-test  3:(null)  4:/lxc/yyz-test
            ..........
            printf(".........1:%s  2:%s  3:%s  4:%s\r\n", parts2[0], parts2[1], parts2[2], current_entire_path);
            */
            
			if (!*p) {
				/* we are processing the subpath, so only update that one */
				free(new_cgroup_paths_sub[i]);
				new_cgroup_paths_sub[i] = strdup(current_entire_path);
				if (!new_cgroup_paths_sub[i])
					goto cleanup_from_error;
			} else {
				/* remember which path was used on this controller */
				free(new_cgroup_paths[i]);
				new_cgroup_paths[i] = strdup(current_entire_path); //  /lxc   /lxc/yyz-test
				if (!new_cgroup_paths[i])
					goto cleanup_from_error;
			}

			r = create_cgroup(info_ptr->designated_mount_point, current_entire_path);
			// /sys/fs/cgroup/pids/lxc 返回值  -1  17  0        pids还可能是cpu,cpuacct等
			// /sys/fs/cgroup/pids/lxc/yyz-test 返回值  0  17  1
			//printf(" .........%s  %d  %d  %d\r\n", current_entire_path, r, errno, contains_name);
			if (r < 0 && errno == EEXIST && contains_name) { //一般不会进入这里面
				/* name clash => try new name with new suffix */
				free(current_entire_path);
				current_entire_path = NULL;
				goto cleanup_name_on_this_level;
			} else if (r < 0 && errno != EEXIST) { //创建失败
				SYSERROR("Could not create cgroup '%s' in '%s'.", current_entire_path, info_ptr->designated_mount_point->mount_point);
				goto cleanup_from_error;
			} else if (r == 0) { //如果是第一次创建/sys/fs/cgroup/pids/lxc  或者多次创建/sys/fs/cgroup/pids/lxc/yyz-test则会进入这里
				/* successfully created */
				r = lxc_grow_array((void ***)&info_ptr->created_paths, &info_ptr->created_paths_capacity, info_ptr->created_paths_count + 1, 8);
				if (r < 0)
					goto cleanup_from_error;
				if (!init_cpuset_if_needed(info_ptr->designated_mount_point, current_entire_path)) {
					ERROR("Failed to initialize cpuset for '%s' in '%s'.", current_entire_path, info_ptr->designated_mount_point->mount_point);
					goto cleanup_from_error;
				}
				info_ptr->created_paths[info_ptr->created_paths_count++] = current_entire_path;
			} else { //例如创建 /sys/fs/cgroup/pids/lxc  了能会走到这里面  比如第一次启动docker后，第二次在起来，这时候lxc目录以及有了
				/* if we didn't create the cgroup, then we have to make sure that
				 * further cgroups will be created properly
				 */

				if (handle_cgroup_settings(info_ptr->designated_mount_point, info_ptr->cgroup_path) < 0) {
					ERROR("Could not set clone_children to 1 for cpuset hierarchy in pre-existing cgroup.");
					goto cleanup_from_error;
				}

				//如果内核版本比较老，则需要手动init cpuset
				if (!init_cpuset_if_needed(info_ptr->designated_mount_point, info_ptr->cgroup_path)) {
					ERROR("Failed to initialize cpuset in pre-existing '%s'.", info_ptr->cgroup_path);
					goto cleanup_from_error;
				}

				/* already existed but path component of pattern didn't contain '%n',
				 * so this is not an error; but then we don't need current_entire_path
				 * anymore...
				 */
				free(current_entire_path);
				current_entire_path = NULL;
			}
		}

		/* save path so far */
		free(path_so_far);
		path_so_far = strdup(current_subpath);
		if (!path_so_far)
			goto cleanup_from_error;

		/* cleanup */
		if (current_component != current_subpath)
			free(current_subpath);
		if (current_component != p_eff)
			free(current_component);
		current_component = current_subpath = NULL;
		continue;

	cleanup_from_error:
		/* called if an error occurred in the loop, so we
		 * do some additional cleanup here
		 */
		saved_errno = errno;
		if (current_component != current_subpath)
			free(current_subpath);
		if (current_component != p_eff)
			free(current_component);
		free(current_entire_path);
		errno = saved_errno;
		goto out_initial_error;
	}

	/* we're done, now update the paths */
	for (i = 0, info_ptr = base_info; info_ptr; info_ptr = info_ptr->next, i++) {
		if (!info_ptr->hierarchy)
			continue;
		/* ignore legacy 'ns' subsystem here, lxc_cgroup_create_legacy
		 * will take care of it
		 * Since we do a continue in above loop, new_cgroup_paths[i] is
		 * unset anyway, as is new_cgroup_paths_sub[i]
		 */
		if (lxc_string_in_array("ns", (const char **)info_ptr->hierarchy->subsystems))
			continue;
		free(info_ptr->cgroup_path);
		
		info_ptr->cgroup_path = new_cgroup_paths[i];
		info_ptr->cgroup_path_sub = new_cgroup_paths_sub[i];
		//y .......... /lxc/yyz-test  (null)
		//printf(" .......... %s  %s\r\n", info_ptr->cgroup_path, info_ptr->cgroup_path_sub);
	}
	/* don't use lxc_free_array since we used the array members
	 * to store them in our result...
	 */
	free(new_cgroup_paths);
	free(new_cgroup_paths_sub);
	free(path_so_far);
	lxc_free_array((void **)cgroup_path_components, free);
	return base_info;

out_initial_error:
    printf("------------------------out_initial_error\r\n");
	saved_errno = errno;
	free(path_so_far);
	lxc_cgroup_process_info_free_and_remove(base_info);
	lxc_free_array((void **)new_cgroup_paths, free);
	lxc_free_array((void **)new_cgroup_paths_sub, free);
	lxc_free_array((void **)cgroup_path_components, free);
	errno = saved_errno;
	return NULL;
}

//mount点为ns的需要更改路径
static int lxc_cgroup_create_legacy(struct cgroup_process_info *base_info, const char *name, pid_t pid)
{
	struct cgroup_process_info *info_ptr;
	int r;

	for (info_ptr = base_info; info_ptr; info_ptr = info_ptr->next) {
		if (!info_ptr->hierarchy)
			continue;

		if (!lxc_string_in_array("ns", (const char **)info_ptr->hierarchy->subsystems))
			continue;
		/*
		 * For any path which has ns cgroup mounted, handler->pid is already
		 * moved into a container called '%d % (handler->pid)'.  Rename it to
		 * the cgroup name and record that.
		 */
		char *tmp = cgroup_rename_nsgroup((const char *)info_ptr->designated_mount_point->mount_point,
				info_ptr->cgroup_path, pid, name);
		if (!tmp)
			return -1;
	    //printf(".....oldname:%s, newname:%s\r\n", info_ptr->cgroup_path, tmp);
		free(info_ptr->cgroup_path);
		info_ptr->cgroup_path = tmp;
		r = lxc_grow_array((void ***)&info_ptr->created_paths, &info_ptr->created_paths_capacity, info_ptr->created_paths_count + 1, 8);
		if (r < 0)
			return -1;
		tmp = strdup(tmp);
		if (!tmp)
			return -1;
		info_ptr->created_paths[info_ptr->created_paths_count++] = tmp;
	}
	return 0;
}

/* get the cgroup membership of a given container */
static struct cgroup_process_info *lxc_cgroup_get_container_info(const char *name, const char *lxcpath, struct cgroup_meta_data *meta_data)
{
	struct cgroup_process_info *result = NULL;
	int saved_errno = 0;
	size_t i;
	struct cgroup_process_info **cptr = &result;
	struct cgroup_process_info *entry = NULL;
	char *path = NULL;

	for (i = 0; i <= meta_data->maximum_hierarchy; i++) {
		struct cgroup_hierarchy *h = meta_data->hierarchies[i];
		if (!h || !h->used)
			continue;

		/* use the command interface to look for the cgroup */
		path = lxc_cmd_get_cgroup_path(name, lxcpath, h->subsystems[0]);
		if (!path) {
			h->used = false;
			continue;
		}

		entry = calloc(1, sizeof(struct cgroup_process_info));
		if (!entry)
			goto out_error;
		entry->meta_ref = lxc_cgroup_get_meta(meta_data);
		entry->hierarchy = h;
		entry->cgroup_path = path;
		path = NULL;

		/* it is not an error if we don't find anything here,
		 * it is up to the caller to decide what to do in that
		 * case */
		entry->designated_mount_point = lxc_cgroup_find_mount_point(h, entry->cgroup_path, true);

		*cptr = entry;
		cptr = &entry->next;
		entry = NULL;
	}

	return result;
out_error:
	saved_errno = errno;
	free(path);
	lxc_cgroup_process_info_free(result);
	lxc_cgroup_process_info_free(entry);
	errno = saved_errno;
	return NULL;
}

/* move a processs to the cgroups specified by the membership */
//把pid进程加入到cgruop,就是把进程pid写入/sys/fs/cgroup/freezer/lxc/yyz-test/tasks中
static int lxc_cgroupfs_enter(struct cgroup_process_info *info, pid_t pid, bool enter_sub)
{
	char pid_buf[32];
	char *cgroup_tasks_fn;
	int r;
	struct cgroup_process_info *info_ptr;

	snprintf(pid_buf, 32, "%lu", (unsigned long)pid);
	for (info_ptr = info; info_ptr; info_ptr = info_ptr->next) {
		if (!info_ptr->hierarchy)
			continue;

		char *cgroup_path = (enter_sub && info_ptr->cgroup_path_sub) ?
			info_ptr->cgroup_path_sub :
			info_ptr->cgroup_path;

		if (!info_ptr->designated_mount_point) {
			info_ptr->designated_mount_point = lxc_cgroup_find_mount_point(info_ptr->hierarchy, cgroup_path, true);
			if (!info_ptr->designated_mount_point) {
				SYSERROR("Could not add pid %lu to cgroup %s: internal error (couldn't find any writable mountpoint to cgroup filesystem)", (unsigned long)pid, cgroup_path);
				return -1;
			}
		}

		cgroup_tasks_fn = cgroup_to_absolute_path(info_ptr->designated_mount_point, cgroup_path, "/tasks");
		if (!cgroup_tasks_fn) {
			SYSERROR("Could not add pid %lu to cgroup %s: internal error", (unsigned long)pid, cgroup_path);
			return -1;
		}

        /*
        ..   /lxc/yyz-test, /sys/fs/cgroup/pids/lxc/yyz-test/tasks
        ..   /lxc/yyz-test, /sys/fs/cgroup/freezer/lxc/yyz-test/tasks
        ..   /lxc/yyz-test, /sys/fs/cgroup/hugetlb/lxc/yyz-test/tasks
        printf("yang test ..   %s, %s\r\n", cgroup_path, cgroup_tasks_fn);
        
        */  
		r = lxc_write_to_file(cgroup_tasks_fn, pid_buf, strlen(pid_buf), false);
		free(cgroup_tasks_fn);
		if (r < 0) {
			SYSERROR("Could not add pid %lu to cgroup %s: internal error", (unsigned long)pid, cgroup_path);
			return -1;
		}
	}

	return 0;
}

/* free process membership information */
void lxc_cgroup_process_info_free(struct cgroup_process_info *info)
{
	struct cgroup_process_info *next;
	if (!info)
		return;
	next = info->next;
	lxc_cgroup_put_meta(info->meta_ref);
	free(info->cgroup_path);
	free(info->cgroup_path_sub);
	lxc_free_array((void **)info->created_paths, free);
	free(info);
	lxc_cgroup_process_info_free(next);
}

/* free process membership information and remove cgroups that were created */
void lxc_cgroup_process_info_free_and_remove(struct cgroup_process_info *info)
{
	struct cgroup_process_info *next;
	char **pp;
	if (!info)
		return;
	next = info->next;
	{
		struct cgroup_mount_point *mp = info->designated_mount_point;
		if (!mp)
			mp = lxc_cgroup_find_mount_point(info->hierarchy, info->cgroup_path, true);
		if (mp)
			/* ignore return value here, perhaps we created the
			 * '/lxc' cgroup in this container but another container
			 * is still running (for example)
			 */
			(void)remove_cgroup(mp, info->cgroup_path, true);
	}
	for (pp = info->created_paths; pp && *pp; pp++);
	for ((void)(pp && --pp); info->created_paths && pp >= info->created_paths; --pp) {
		free(*pp);
	}
	free(info->created_paths);
	lxc_cgroup_put_meta(info->meta_ref);
	free(info->cgroup_path);
	free(info->cgroup_path_sub);
	free(info);
	lxc_cgroup_process_info_free_and_remove(next);
}

static char *lxc_cgroup_get_hierarchy_path_data(const char *subsystem, struct cgfs_data *d)
{
	struct cgroup_process_info *info = d->info;
	info = find_info_for_subsystem(info, subsystem);
	if (!info)
		return NULL;
	prune_init_scope(info->cgroup_path);
	return info->cgroup_path;
}

static char *lxc_cgroup_get_hierarchy_abs_path_data(const char *subsystem, struct cgfs_data *d)
{
	struct cgroup_process_info *info = d->info;
	struct cgroup_mount_point *mp = NULL;

	info = find_info_for_subsystem(info, subsystem);
	if (!info)
		return NULL;
	if (info->designated_mount_point) {
		mp = info->designated_mount_point;
	} else {
		mp = lxc_cgroup_find_mount_point(info->hierarchy, info->cgroup_path, true);
		if (!mp)
			return NULL;
	}
	return cgroup_to_absolute_path(mp, info->cgroup_path, NULL);
}

static char *lxc_cgroup_get_hierarchy_abs_path(const char *subsystem, const char *name, const char *lxcpath)
{
	struct cgroup_meta_data *meta;
	struct cgroup_process_info *base_info, *info;
	struct cgroup_mount_point *mp;
	char *result = NULL;

	meta = lxc_cgroup_load_meta();
	if (!meta)
		return NULL;
	base_info = lxc_cgroup_get_container_info(name, lxcpath, meta);
	if (!base_info)
		goto out1;
	info = find_info_for_subsystem(base_info, subsystem);
	if (!info)
		goto out2;
	if (info->designated_mount_point) {
		mp = info->designated_mount_point;
	} else {
		mp = lxc_cgroup_find_mount_point(info->hierarchy, info->cgroup_path, true);
		if (!mp)
			goto out3;
	}
	result = cgroup_to_absolute_path(mp, info->cgroup_path, NULL);
out3:
out2:
	lxc_cgroup_process_info_free(base_info);
out1:
	lxc_cgroup_put_meta(meta);
	return result;
}

//把value写入filename,也就是把把配置文件cgroup解析内容写入cgroup相应文件
static int lxc_cgroup_set_data(const char *filename, const char *value, struct cgfs_data *d)
{
	char *subsystem = NULL, *p, *path;
	int ret = -1;

	subsystem = alloca(strlen(filename) + 1);
	strcpy(subsystem, filename);
	if ((p = strchr(subsystem, '.')) != NULL)
		*p = '\0';

	errno = ENOENT;
	path = lxc_cgroup_get_hierarchy_abs_path_data(subsystem, d);
	/*
	........     memory.limit_in_bytes, 512M, /sys/fs/cgroup/memory/lxc/yyz-test
    ........     cpuset.cpus, 0, /sys/fs/cgroup/cpuset/lxc/yyz-test
	printf("........     %s, %s, %s\r\n", filename, value, path);
	*/
	if (path) {
		ret = do_cgroup_set(path, filename, value);
		int saved_errno = errno;
		free(path);
		errno = saved_errno;
	}
	return ret;
}

static int lxc_cgroupfs_set(const char *filename, const char *value, const char *name, const char *lxcpath)
{
	char *subsystem = NULL, *p, *path;
	int ret = -1;

	subsystem = alloca(strlen(filename) + 1);
	strcpy(subsystem, filename);
	if ((p = strchr(subsystem, '.')) != NULL)
		*p = '\0';

	path = lxc_cgroup_get_hierarchy_abs_path(subsystem, name, lxcpath);
	if (path) {
		ret = do_cgroup_set(path, filename, value);
		free(path);
	}
	return ret;
}

static int lxc_cgroupfs_get(const char *filename, char *value, size_t len, const char *name, const char *lxcpath)
{
	char *subsystem = NULL, *p, *path;
	int ret = -1;

	subsystem = alloca(strlen(filename) + 1);
	strcpy(subsystem, filename);
	if ((p = strchr(subsystem, '.')) != NULL)
		*p = '\0';

	path = lxc_cgroup_get_hierarchy_abs_path(subsystem, name, lxcpath);
	if (path) {
		ret = do_cgroup_get(path, filename, value, len);
		free(path);
	}
	return ret;
}

static bool cgroupfs_mount_cgroup(void *hdata, const char *root, int type)
{
	size_t bufsz = strlen(root) + sizeof("/sys/fs/cgroup");
	char *path = NULL;
	char **parts = NULL;
	char *dirname = NULL;
	char *abs_path = NULL;
	char *abs_path2 = NULL;
	struct cgfs_data *cgfs_d;
	struct cgroup_process_info *info, *base_info;
	int r, saved_errno = 0;

	cgfs_d = hdata;
	if (!cgfs_d)
		return false;
	base_info = cgfs_d->info;

	/* If we get passed the _NOSPEC types, we default to _MIXED, since we don't
	 * have access to the lxc_conf object at this point. It really should be up
	 * to the caller to fix this, but this doesn't really hurt.
	 */
	if (type == LXC_AUTO_CGROUP_FULL_NOSPEC)
		type = LXC_AUTO_CGROUP_FULL_MIXED;
	else if (type == LXC_AUTO_CGROUP_NOSPEC)
		type = LXC_AUTO_CGROUP_MIXED;

	if (type < LXC_AUTO_CGROUP_RO || type > LXC_AUTO_CGROUP_FULL_MIXED) {
		ERROR("could not mount cgroups into container: invalid type specified internally");
		errno = EINVAL;
		return false;
	}

	path = calloc(1, bufsz);
	if (!path)
		return false;
	snprintf(path, bufsz, "%s/sys/fs/cgroup", root);

	
	r = safe_mount("cgroup_root", path, "tmpfs",
			MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_RELATIME,
			"size=10240k,mode=755",
			root);
			
    
	if (r < 0) {
		SYSERROR("could not mount tmpfs to /sys/fs/cgroup in the container");
		return false;
	}
    
    //mount所有cgroup子系统
	/* now mount all the hierarchies we care about */
	for (info = base_info; info; info = info->next) {
		size_t subsystem_count, i;
		struct cgroup_mount_point *mp = info->designated_mount_point;

		if (!info->hierarchy)
			continue;

		if (!mountpoint_is_accessible(mp))
			mp = lxc_cgroup_find_mount_point(info->hierarchy, info->cgroup_path, true);

		if (!mp) {
			SYSERROR("could not find original mount point for cgroup hierarchy while trying to mount cgroup filesystem");
			goto out_error;
		}

		subsystem_count = lxc_array_len((void **)info->hierarchy->subsystems);
		parts = calloc(subsystem_count + 1, sizeof(char *));
		if (!parts)
			goto out_error;

		for (i = 0; i < subsystem_count; i++) {
			if (!strncmp(info->hierarchy->subsystems[i], "name=", 5))
				parts[i] = info->hierarchy->subsystems[i] + 5;
			else
				parts[i] = info->hierarchy->subsystems[i];
		}
		dirname = lxc_string_join(",", (const char **)parts, false);
		if (!dirname)
			goto out_error;

		/* create subsystem directory */
		abs_path = lxc_append_paths(path, dirname);
		if (!abs_path)
			goto out_error;
		r = mkdir_p(abs_path, 0755);
		if (r < 0 && errno != EEXIST) {
			SYSERROR("could not create cgroup subsystem directory /sys/fs/cgroup/%s", dirname);
			goto out_error;
		}

		abs_path2 = lxc_append_paths(abs_path, info->cgroup_path);
		if (!abs_path2)
			goto out_error;

		if (type == LXC_AUTO_CGROUP_FULL_RO || type == LXC_AUTO_CGROUP_FULL_RW || type == LXC_AUTO_CGROUP_FULL_MIXED) {
			/* bind-mount the cgroup entire filesystem there */
			if (strcmp(mp->mount_prefix, "/") != 0) {
				/* FIXME: maybe we should just try to remount the entire hierarchy
				 *        with a regular mount command? may that works? */
				ERROR("could not automatically mount cgroup-full to /sys/fs/cgroup/%s: host has no mount point for this cgroup filesystem that has access to the root cgroup", dirname);
				goto out_error;
			}
			r = mount(mp->mount_point, abs_path, "none", MS_BIND, 0);
			if (r < 0) {
				SYSERROR("error bind-mounting %s to %s", mp->mount_point, abs_path);
				goto out_error;
			}
			/* main cgroup path should be read-only */
			if (type == LXC_AUTO_CGROUP_FULL_RO || type == LXC_AUTO_CGROUP_FULL_MIXED) {
				r = mount(NULL, abs_path, NULL, MS_REMOUNT|MS_BIND|MS_RDONLY, NULL);
				if (r < 0) {
					SYSERROR("error re-mounting %s readonly", abs_path);
					goto out_error;
				}
			}
			/* own cgroup should be read-write */
			if (type == LXC_AUTO_CGROUP_FULL_MIXED) {
				r = mount(abs_path2, abs_path2, NULL, MS_BIND, NULL);
				if (r < 0) {
					SYSERROR("error bind-mounting %s onto itself", abs_path2);
					goto out_error;
				}
				r = mount(NULL, abs_path2, NULL, MS_REMOUNT|MS_BIND, NULL);
				if (r < 0) {
					SYSERROR("error re-mounting %s readwrite", abs_path2);
					goto out_error;
				}
			}
		} else {
			/* create path for container's cgroup */
			r = mkdir_p(abs_path2, 0755);
			if (r < 0 && errno != EEXIST) {
				SYSERROR("could not create cgroup directory /sys/fs/cgroup/%s%s", dirname, info->cgroup_path);
				goto out_error;
			}

			/* for read-only and mixed cases, we have to bind-mount the tmpfs directory
			 * that points to the hierarchy itself (i.e. /sys/fs/cgroup/cpu etc.) onto
			 * itself and then bind-mount it read-only, since we keep the tmpfs itself
			 * read-write (see comment below)
			 */
			if (type == LXC_AUTO_CGROUP_MIXED || type == LXC_AUTO_CGROUP_RO) {
				r = mount(abs_path, abs_path, NULL, MS_BIND, NULL);
				if (r < 0) {
					SYSERROR("error bind-mounting %s onto itself", abs_path);
					goto out_error;
				}
				r = mount(NULL, abs_path, NULL, MS_REMOUNT|MS_BIND|MS_RDONLY, NULL);
				if (r < 0) {
					SYSERROR("error re-mounting %s readonly", abs_path);
					goto out_error;
				}
			}

			free(abs_path);
			abs_path = NULL;

			/* bind-mount container's cgroup to that directory */
			abs_path = cgroup_to_absolute_path(mp, info->cgroup_path, NULL);
			if (!abs_path)
				goto out_error;
			r = mount(abs_path, abs_path2, "none", MS_BIND, 0);
			if (r < 0) {
				SYSERROR("error bind-mounting %s to %s", abs_path, abs_path2);
				goto out_error;
			}
			if (type == LXC_AUTO_CGROUP_RO) {
				r = mount(NULL, abs_path2, NULL, MS_REMOUNT|MS_BIND|MS_RDONLY, NULL);
				if (r < 0) {
					SYSERROR("error re-mounting %s readonly", abs_path2);
					goto out_error;
				}
			}
		}

		free(abs_path);
		free(abs_path2);
		abs_path = NULL;
		abs_path2 = NULL;

		/* add symlinks for every single subsystem */
		if (subsystem_count > 1) {
			for (i = 0; i < subsystem_count; i++) {
				abs_path = lxc_append_paths(path, parts[i]);
				if (!abs_path)
					goto out_error;
				r = symlink(dirname, abs_path);
				if (r < 0)
					WARN("could not create symlink %s -> %s in /sys/fs/cgroup of container", parts[i], dirname);
				free(abs_path);
				abs_path = NULL;
			}
		}
		free(dirname);
		free(parts);
		dirname = NULL;
		parts = NULL;
	}

	/* We used to remount the entire tmpfs readonly if any :ro or
	 * :mixed mode was specified. However, Ubuntu's mountall has the
	 * unfortunate behavior to block bootup if /sys/fs/cgroup is
	 * mounted read-only and cannot be remounted read-write.
	 * (mountall reads /lib/init/fstab and tries to (re-)mount all of
	 * these if they are not already mounted with the right options;
	 * it contains an entry for /sys/fs/cgroup. In case it can't do
	 * that, it prompts for the user to either manually fix it or
	 * boot anyway. But without user input, booting of the container
	 * hangs.)
	 *
	 * Instead of remounting the entire tmpfs readonly, we only
	 * remount the paths readonly that are part of the cgroup
	 * hierarchy.
	 */

	free(path);

	return true;

out_error:
	saved_errno = errno;
	free(path);
	free(dirname);
	free(parts);
	free(abs_path);
	free(abs_path2);
	errno = saved_errno;
	return false;
}

static int cgfs_nrtasks(void *hdata)
{
	struct cgfs_data *d = hdata;
	struct cgroup_process_info *info;
	struct cgroup_mount_point *mp = NULL;
	char *abs_path = NULL;
	int ret;

	if (!d) {
		errno = ENOENT;
		return -1;
	}

	info = d->info;
	if (!info) {
		errno = ENOENT;
		return -1;
	}

	if (info->designated_mount_point) {
		mp = info->designated_mount_point;
	} else {
		mp = lxc_cgroup_find_mount_point(info->hierarchy, info->cgroup_path, false);
		if (!mp)
			return -1;
	}

	abs_path = cgroup_to_absolute_path(mp, info->cgroup_path, NULL);
	if (!abs_path)
		return -1;

	ret = cgroup_recursive_task_count(abs_path);
	free(abs_path);
	return ret;
}


/*
[root@localhost 1]# cat /proc/1/cgroup 
11:pids:/
10:freezer:/
9:hugetlb:/
8:blkio:/
7:cpuset:/
6:memory:/
5:cpuacct,cpu:/
4:perf_event:/
3:devices:/
2:net_prio,net_cls:/
1:name=systemd:/
*/
//proc_pid_cgroup_str代表proc目录下面的cgroup文件
static struct cgroup_process_info *
lxc_cgroup_process_info_getx(const char *proc_pid_cgroup_str,
			     struct cgroup_meta_data *meta)
{
	struct cgroup_process_info *result = NULL;
	FILE *proc_pid_cgroup = NULL;
	char *line = NULL;
	size_t sz = 0;
	int saved_errno = 0;
	struct cgroup_process_info **cptr = &result; //实现entry瓜姐到result上面
	struct cgroup_process_info *entry = NULL;

	proc_pid_cgroup = fopen_cloexec(proc_pid_cgroup_str, "r");
	if (!proc_pid_cgroup)
		return NULL;

	while (getline(&line, &sz, proc_pid_cgroup) != -1) {
		/* file format: hierarchy:subsystems:group */
		char *colon1;
		char *colon2;
		char *endptr;
		int hierarchy_number;//获取每行开始的数字
		struct cgroup_hierarchy *h = NULL;

		if (!line[0])
			continue;

		if (line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';

		colon1 = strchr(line, ':');
		if (!colon1)
			continue;
		*colon1++ = '\0';
		colon2 = strchr(colon1, ':');
		if (!colon2)
			continue;
		*colon2++ = '\0';

		endptr = NULL;

		/* With cgroupv2 /proc/self/cgroup can contain entries of the
		 * form: 0::/
		 * These entries need to be skipped.
		 */
		if (!strcmp(colon1, ""))
			continue;

		hierarchy_number = strtoul(line, &endptr, 10); //获取每行开始的数字
		if (!endptr || *endptr)
			continue;

        /*
         ..... 11,  11, pids, /
         ..... 10,  11, freezer, /
         ..... 9,  11, hugetlb, /
         ..... 8,  11, blkio, /
         ..... 7,  11, cpuset, /
         ..... 6,  11, memory, /
        ............
        printf(" ..... %d,  %d, %s, %s\r\n", hierarchy_number, meta->maximum_hierarchy, colon1,colon2);  //0-11
		*/
		if (hierarchy_number > meta->maximum_hierarchy) { //0 - 11
			/* we encountered a hierarchy we didn't have before,
			 * so probably somebody remounted some stuff in the
			 * mean time...
			 */
			errno = EAGAIN;
			goto out_error;
		}

		h = meta->hierarchies[hierarchy_number];
		if (!h) {
			/* we encountered a hierarchy that was thought to be
			 * dead before, so probably somebody remounted some
			 * stuff in the mean time...
			 */
			errno = EAGAIN;
			goto out_error;
		}

		/* we are told that we should ignore this hierarchy */
		if (!h->used)
			continue;

		entry = calloc(1, sizeof(struct cgroup_process_info));
		if (!entry)
			goto out_error;

		entry->meta_ref = lxc_cgroup_get_meta(meta);
		entry->hierarchy = h;
		entry->cgroup_path = strdup(colon2);
		if (!entry->cgroup_path)
			goto out_error;
		prune_init_scope(entry->cgroup_path);


        //这里把entry挂接到result上面
		*cptr = entry; 
		cptr = &entry->next;
		entry = NULL;
	}

	fclose(proc_pid_cgroup);
	free(line);
	return result;

out_error:
	saved_errno = errno;
	if (proc_pid_cgroup)
		fclose(proc_pid_cgroup);
	lxc_cgroup_process_info_free(result);
	lxc_cgroup_process_info_free(entry);
	free(line);
	errno = saved_errno;
	return NULL;
}

static char **subsystems_from_mount_options(const char *mount_options,
					    char **kernel_list)
{
	char *token, *str, *saveptr = NULL;
	char **result = NULL;
	size_t result_capacity = 0;
	size_t result_count = 0;
	int saved_errno;
	int r;

	str = alloca(strlen(mount_options)+1);
	strcpy(str, mount_options);
	for (; (token = strtok_r(str, ",", &saveptr)); str = NULL) {
		/* we have a subsystem if it's either in the list of
		 * subsystems provided by the kernel OR if it starts
		 * with name= for named hierarchies
		 */
		r = lxc_grow_array((void ***)&result, &result_capacity, result_count + 1, 12);
		if (r < 0)
			goto out_free;
		result[result_count + 1] = NULL;
		if (strncmp(token, "name=", 5) && !lxc_string_in_array(token, (const char **)kernel_list)) {
			// this is eg 'systemd' but the mount will be 'name=systemd'
			result[result_count] = malloc(strlen(token) + 6);
			if (result[result_count])
				sprintf(result[result_count], "name=%s", token);
		} else
			result[result_count] = strdup(token);
		if (!result[result_count])
			goto out_free;
		result_count++;
	}

	return result;

out_free:
	saved_errno = errno;
	lxc_free_array((void**)result, free);
	errno = saved_errno;
	return NULL;
}

static void lxc_cgroup_mount_point_free(struct cgroup_mount_point *mp)
{
	if (!mp)
		return;
	free(mp->mount_point);
	free(mp->mount_prefix);
	free(mp);
}

static void lxc_cgroup_hierarchy_free(struct cgroup_hierarchy *h)
{
	if (!h)
		return;
	if (h->subsystems) {
		lxc_free_array((void **)h->subsystems, free);
		h->subsystems = NULL;
	}
	if (h->all_mount_points) {
		free(h->all_mount_points);
		h->all_mount_points = NULL;
	}
	free(h);
	h = NULL;
}

static bool is_valid_cgroup(const char *name)
{
	const char *p;
	for (p = name; *p; p++) {
		/* Use the ASCII printable characters range(32 - 127)
		 * is reasonable, we kick out 32(SPACE) because it'll
		 * break legacy lxc-ls
		 */
		if (*p <= 32 || *p >= 127 || *p == '/')
			return false;
	}
	return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static int create_or_remove_cgroup(bool do_remove,
		struct cgroup_mount_point *mp, const char *path, int recurse)
{
	int r, saved_errno = 0;
	char *buf = cgroup_to_absolute_path(mp, path, NULL);
	if (!buf)
		return -1;

    /*  在cgroup挂载点上创建lxc  lxc/yyz-test等目录
    ............... buf:/sys/fs/cgroup/pids/lxc
    .....
    ............... buf:/sys/fs/cgroup/cpu,cpuacct/lxc
    ............... buf:/sys/fs/cgroup/perf_event/lxc
    ............... buf:/sys/fs/cgroup/devices/lxc
    ............... buf:/sys/fs/cgroup/net_cls,net_prio/lxc
    ............... buf:/sys/fs/cgroup/pids/lxc/yyz-test
    .....
    ............... buf:/sys/fs/cgroup/cpu,cpuacct/lxc/yyz-test
    ............... buf:/sys/fs/cgroup/perf_event/lxc/yyz-test
    ............... buf:/sys/fs/cgroup/devices/lxc/yyz-test
    ............... buf:/sys/fs/cgroup/net_cls,net_prio/lxc/yyz-test
    //printf("............... buf:%s\r\n", buf);
    */
	/* create or remove directory */
	if (do_remove) {
		if (recurse)
			r = cgroup_rmdir(buf);
		else
			r = rmdir(buf);
	} else 
		r = mkdir(buf, 0777);  //如果文件存在，会返回-1，错误码为EEXIST
	saved_errno = errno;
	free(buf);
	errno = saved_errno;
	return r;
}

//来创建cgroup的目录层级。 读取/proc/1/cgroup下去创建相应的cgroup层级，最后创建cgroup的目录。
static int create_cgroup(struct cgroup_mount_point *mp, const char *path)
{
	return create_or_remove_cgroup(false, mp, path, false);
}

static int remove_cgroup(struct cgroup_mount_point *mp,
			 const char *path, bool recurse)
{
	return create_or_remove_cgroup(true, mp, path, recurse);
}

static char *cgroup_to_absolute_path(struct cgroup_mount_point *mp,
				     const char *path, const char *suffix)
{
	/* first we have to make sure we subtract the mount point's prefix */
	char *prefix = mp->mount_prefix;
	char *buf;
	ssize_t len, rv;

	/* we want to make sure only absolute paths to cgroups are passed to us */
	if (path[0] != '/') {
		errno = EINVAL;
		return NULL;
	}

	if (prefix && !strcmp(prefix, "/"))
		prefix = NULL;

	/* prefix doesn't match */
	if (prefix && strncmp(prefix, path, strlen(prefix)) != 0) {
		errno = EINVAL;
		return NULL;
	}
	/* if prefix is /foo and path is /foobar */
	if (prefix && path[strlen(prefix)] != '/' && path[strlen(prefix)] != '\0') {
		errno = EINVAL;
		return NULL;
	}

	/* remove prefix from path */
	path += prefix ? strlen(prefix) : 0;

	len = strlen(mp->mount_point) + strlen(path) + (suffix ? strlen(suffix) : 0);
	buf = calloc(len + 1, 1);
	if (!buf)
		return NULL;
	rv = snprintf(buf, len + 1, "%s%s%s", mp->mount_point, path, suffix ? suffix : "");
	if (rv > len) {
		free(buf);
		errno = ENOMEM;
		return NULL;
	}

	return buf;
}

static struct cgroup_process_info *
find_info_for_subsystem(struct cgroup_process_info *info, const char *subsystem)
{
	struct cgroup_process_info *info_ptr;
	for (info_ptr = info; info_ptr; info_ptr = info_ptr->next) {
		struct cgroup_hierarchy *h = info_ptr->hierarchy;
		if (!h)
			continue;
		if (lxc_string_in_array(subsystem, (const char **)h->subsystems))
			return info_ptr;
	}
	errno = ENOENT;
	return NULL;
}

static int do_cgroup_get(const char *cgroup_path, const char *sub_filename,
			 char *value, size_t len)
{
	const char *parts[3] = {
		cgroup_path,
		sub_filename,
		NULL
	};
	char *filename;
	int ret, saved_errno;

	filename = lxc_string_join("/", parts, false);
	if (!filename)
		return -1;

	ret = lxc_read_from_file(filename, value, len);
	saved_errno = errno;
	free(filename);
	errno = saved_errno;
	return ret;
}

static int do_cgroup_set(const char *cgroup_path, const char *sub_filename,
			 const char *value)
{
	const char *parts[3] = {
		cgroup_path,
		sub_filename,
		NULL
	};
	char *filename;
	int ret, saved_errno;

	filename = lxc_string_join("/", parts, false);
	if (!filename)
		return -1;
    /*
    ................../sys/fs/cgroup/memory/lxc/yyz-test/memory.limit_in_bytes
    ................../sys/fs/cgroup/cpuset/lxc/yyz-test/cpuset.cpus
    //printf("..................%s\r\n", filename);
	*/
	ret = lxc_write_to_file(filename, value, strlen(value), false);
	saved_errno = errno;
	free(filename);
	errno = saved_errno;
	return ret;
}

/*
按照配置修改参数，例如lxc.cgourp.memory.limit_in_bytes=5
则往/sys/fs/cgroup/memory/lxc/yyz-test/memory.limit_in_bytes写5
*/
static int do_setup_cgroup_limits(struct cgfs_data *d,
			   struct lxc_list *cgroup_settings, bool do_devices)
{
	struct lxc_list *iterator, *sorted_cgroup_settings, *next;
	struct lxc_cgroup *cg;
	int ret = -1;

	if (lxc_list_empty(cgroup_settings))
		return 0;

	sorted_cgroup_settings = sort_cgroup_settings(cgroup_settings);
	if (!sorted_cgroup_settings) {
		return -1;
	}

	lxc_list_for_each(iterator, sorted_cgroup_settings) {
		cg = iterator->elem;

        /*
        LXC读写宿主机块设备，主要有两种方法：
        
        间接挂载 
        先将块设备格式化后挂载在宿主机某个目录下， 
        然后利用 lxc.mount.entry ，将该目录mount到LXC的rootfs某个目录， 
        注意，必须先确定LXC中该挂载目录是存在的，然后再配置挂载，否则无法启动LXC
        
        直接读写 
        利用lxc.cgroup.devices.allow，赋予LXC直接读写相应块设备的权限， 
        这样就可以直接在LXC里面访问相应的块设备，进行格式化，挂载等操作
        参考http://blog.sina.com.cn/s/blog_4da051a60102vfwx.html
        */
		if (do_devices == !strncmp("devices", cg->subsystem, 7)) {  //前n个字符相同，则返回0
		//do_devices为false的时候lxc.cgroup中devices以外的配置走这里
		//do_devices为ture的时候lxc.cgroup中devices的配置走这里
			if (strcmp(cg->subsystem, "devices.deny") == 0 &&
					cgroup_devices_has_allow_or_deny(d, cg->value, false))
				continue;
			if (strcmp(cg->subsystem, "devices.allow") == 0 &&
					cgroup_devices_has_allow_or_deny(d, cg->value, true))
				continue;

				
			if (lxc_cgroup_set_data(cg->subsystem, cg->value, d)) {
				if (do_devices && (errno == EACCES || errno == EPERM)) {
					WARN("Error setting %s to %s for %s",
					      cg->subsystem, cg->value, d->name);
					continue;
				}
				SYSERROR("Error setting %s to %s for %s",
				      cg->subsystem, cg->value, d->name);
				goto out; //直接out
			}
		}

		DEBUG("cgroup '%s' set to '%s'", cg->subsystem, cg->value);
		/*
        lxc.cgroup.devices.deny = a 
        lxc.cgroup.devices.allow = c *:* m

		
	    ... cgroup 'devices.deny' set to 'a'
        ... cgroup 'devices.allow' set to 'c *:* m'
		printf(" ... cgroup '%s' set to '%s'\r\n", cg->subsystem, cg->value);
		*/
	}

	ret = 0;
	INFO("cgroup has been setup");
out:
	lxc_list_for_each_safe(iterator, sorted_cgroup_settings, next) {
		lxc_list_del(iterator);
		free(iterator);
	}
	free(sorted_cgroup_settings);
	return ret;
}

static bool cgroup_devices_has_allow_or_deny(struct cgfs_data *d,
					     char *v, bool for_allow)
{
	char *path;
	FILE *devices_list;
	char *line = NULL;
	size_t sz = 0;
	bool ret = !for_allow;
	const char *parts[3] = {
		NULL,
		"devices.list",
		NULL
	};

	// XXX FIXME if users could use something other than 'lxc.devices.deny = a'.
	// not sure they ever do, but they *could*
	// right now, I'm assuming they do NOT
	if (!for_allow && strcmp(v, "a") != 0 && strcmp(v, "a *:* rwm") != 0)
		return false;

	parts[0] = (const char *)lxc_cgroup_get_hierarchy_abs_path_data("devices", d);
	if (!parts[0])
		return false;
	path = lxc_string_join("/", parts, false);
	if (!path) {
		free((void *)parts[0]);
		return false;
	}

	devices_list = fopen_cloexec(path, "r");
	if (!devices_list) {
		free(path);
		return false;
	}

	while (getline(&line, &sz, devices_list) != -1) {
		size_t len = strlen(line);
		if (len > 0 && line[len-1] == '\n')
			line[len-1] = '\0';
		if (strcmp(line, "a *:* rwm") == 0) {
			ret = for_allow;
			goto out;
		} else if (for_allow && strcmp(line, v) == 0) {
			ret = true;
			goto out;
		}
	}

out:
	fclose(devices_list);
	free(line);
	free(path);
	return ret;
}

static int cgroup_recursive_task_count(const char *cgroup_path)
{
	DIR *d;
	struct dirent *dent;
	int n = 0, r;

	d = opendir(cgroup_path);
	if (!d)
		return 0;

	while ((dent = readdir(d))) {
		const char *parts[3] = {
			cgroup_path,
			dent->d_name,
			NULL
		};
		char *sub_path;
		struct stat st;

		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
			continue;
		sub_path = lxc_string_join("/", parts, false);
		if (!sub_path) {
			closedir(d);
			return -1;
		}
		r = stat(sub_path, &st);
		if (r < 0) {
			closedir(d);
			free(sub_path);
			return -1;
		}
		if (S_ISDIR(st.st_mode)) {
			r = cgroup_recursive_task_count(sub_path);
			if (r >= 0)
				n += r;
		} else if (!strcmp(dent->d_name, "tasks")) {
			r = count_lines(sub_path);
			if (r >= 0)
				n += r;
		}
		free(sub_path);
	}
	closedir(d);

	return n;
}

static int count_lines(const char *fn)
{
	FILE *f;
	char *line = NULL;
	size_t sz = 0;
	int n = 0;

	f = fopen_cloexec(fn, "r");
	if (!f)
		return -1;

	while (getline(&line, &sz, f) != -1) {
		n++;
	}
	free(line);
	fclose(f);
	return n;
}

/*
把xxx/memory//memory.use_hierarchy值1，XXX一般为挂载点/sys/fs/cgroup
把xxx/cpuset//cgroup.clone_children值1，XXX一般为挂载点/sys/fs/cgroup
*/
static int handle_cgroup_settings(struct cgroup_mount_point *mp,
				  char *cgroup_path)
{
	int r, saved_errno = 0;
	char buf[2];

	mp->need_cpuset_init = false; //这里false，如果后面检测到stat失败，则说明内核版本较低，需要我们自己初始化

	/* If this is the memory cgroup, we want to enforce hierarchy.
	 * But don't fail if for some reason we can't.
	 */
	if (lxc_string_in_array("memory", (const char **)mp->hierarchy->subsystems)) {
	    //如果mp指向的是cgroup的memory目录，则把xxx/memory//memory.use_hierarchy值1，XXX一般为挂载点/sys/fs/cgroup
		char *cc_path = cgroup_to_absolute_path(mp, cgroup_path, "/memory.use_hierarchy");
		//printf("yang test ..... .. cc_path:%s\r\n", cc_path);  cc_path:/sys/fs/cgroup/memory//memory.use_hierarchy
		if (cc_path) {
			r = lxc_read_from_file(cc_path, buf, 1);
			if (r < 1 || buf[0] != '1') {
				r = lxc_write_to_file(cc_path, "1", 1, false);
				if (r < 0)
					SYSERROR("failed to set memory.use_hierarchy to 1; continuing");
			}
			free(cc_path);
		}
	}

	/* if this is a cpuset hierarchy, we have to set cgroup.clone_children in
	 * the base cgroup, otherwise containers will start with an empty cpuset.mems
	 * and cpuset.cpus and then
	 */
	 ///sys/fs/cgroup/cpuset//cgroup.clone_children 值1
	if (lxc_string_in_array("cpuset", (const char **)mp->hierarchy->subsystems)) {
	//cgroup.clone_children cpuset的subsystem会读取这个配置文件，如果这个的值是1(默认是0)，子cgroup才会继承父cgroup的cpuset的配置。
		char *cc_path = cgroup_to_absolute_path(mp, cgroup_path, "/cgroup.clone_children");//cgroup.clone_children cpuset的subsystem会读取这个配置文件，如果这个的值是1(默认是0)，子cgroup才会继承父cgroup的cpuset的配置。
		struct stat sb;

		if (!cc_path)
			return -1;
		/* cgroup.clone_children is not available when running under
		 * older kernel versions; in this case, we'll initialize
		 * cpuset.cpus and cpuset.mems later, after the new cgroup
		 * was created
		 */
		if (stat(cc_path, &sb) != 0 && errno == ENOENT) { //如果是linux内核版本比较老，则需要我们代码自己初始化，生效见init_cpuset_if_needed
			mp->need_cpuset_init = true;
			free(cc_path);
			return 0;
		}
		r = lxc_read_from_file(cc_path, buf, 1);
		if (r == 1 && buf[0] == '1') {
			free(cc_path);
			return 0;
		}
		r = lxc_write_to_file(cc_path, "1", 1, false);
		saved_errno = errno;
		free(cc_path);
		errno = saved_errno;
		return r < 0 ? -1 : 0;
	}
	return 0;
}

static int cgroup_read_from_file(const char *fn, char buf[], size_t bufsize)
{
	int ret = lxc_read_from_file(fn, buf, bufsize);
	if (ret < 0) {
		SYSERROR("failed to read %s", fn);
		return ret;
	}
	if (ret == bufsize) {
		if (bufsize > 0) {
			/* obviously this wasn't empty */
			buf[bufsize-1] = '\0';
			return ret;
		}
		/* Callers don't do this, but regression/sanity check */
		ERROR("%s: was not expecting 0 bufsize", __func__);
		return -1;
	}
	buf[ret] = '\0';
	return ret;
}

static bool do_init_cpuset_file(struct cgroup_mount_point *mp,
				const char *path, const char *name)
{
	char value[1024];
	char *childfile, *parentfile = NULL, *tmp;
	int ret;
	bool ok = false;

	childfile = cgroup_to_absolute_path(mp, path, name);
	if (!childfile)
		return false;

	/* don't overwrite a non-empty value in the file */
	ret = cgroup_read_from_file(childfile, value, sizeof(value));
	if (ret < 0)
		goto out;
	if (value[0] != '\0' && value[0] != '\n') {
		ok = true;
		goto out;
	}

	/* path to the same name in the parent cgroup */
	parentfile = strdup(path);
	if (!parentfile)
		goto out;

	tmp = strrchr(parentfile, '/');
	if (!tmp)
		goto out;
	if (tmp == parentfile)
		tmp++; /* keep the '/' at the start */
	*tmp = '\0';
	tmp = parentfile;
	parentfile = cgroup_to_absolute_path(mp, tmp, name);
	free(tmp);
	if (!parentfile)
		goto out;

	/* copy from parent to child cgroup */
	ret = cgroup_read_from_file(parentfile, value, sizeof(value));
	if (ret < 0)
		goto out;
	if (ret == sizeof(value)) {
		/* If anyone actually sees this error, we can address it */
		ERROR("parent cpuset value too long");
		goto out;
	}
	ok = (lxc_write_to_file(childfile, value, strlen(value), false) >= 0);
	if (!ok)
		SYSERROR("failed writing %s", childfile);

out:
	free(parentfile);
	free(childfile);
	return ok;
}

static bool init_cpuset_if_needed(struct cgroup_mount_point *mp,
				  const char *path)
{
	/* the files we have to handle here are only in cpuset hierarchies */
	if (!lxc_string_in_array("cpuset",
				 (const char **)mp->hierarchy->subsystems))
		return true;

	if (!mp->need_cpuset_init) //需要自己初始化，和handle_cgroup_settings配合阅读
		return true;

	return (do_init_cpuset_file(mp, path, "/cpuset.cpus") &&
		do_init_cpuset_file(mp, path, "/cpuset.mems") );
}

static void print_cgfs_init_debuginfo(struct cgfs_data *d)
{
	int i;

	if (!getenv("LXC_DEBUG_CGFS"))
		return;

	DEBUG("Cgroup information:");
	DEBUG("  container name: %s", d->name);
	if (!d->meta || !d->meta->hierarchies) {
		DEBUG("  No hierarchies found.");
		return;
	}
	DEBUG("  Controllers:");
	for (i = 0; i <= d->meta->maximum_hierarchy; i++) {
		char **p;
		struct cgroup_hierarchy *h = d->meta->hierarchies[i];
		if (!h) {
			DEBUG("     Empty hierarchy number %d.", i);
			continue;
		}
		for (p = h->subsystems; p && *p; p++) {
			DEBUG("     %2d: %s", i, *p);
		}
	}
}

struct cgroup_ops *cgfs_ops_init(void)
{
	return &cgfs_ops;
}

//读取/proc/self/mountinfo和/proc/self/cgroup相关信息存入相应结构， //需要保证mount的挂载点必须在/proc/cgroups中存在
static void *cgfs_init(const char *name)
{
	struct cgfs_data *d;

	d = malloc(sizeof(*d));
	if (!d)
		return NULL;

	memset(d, 0, sizeof(*d));
	d->name = strdup(name);
	if (!d->name)
		goto err1;

	d->cgroup_pattern = lxc_global_config_value("lxc.cgroup.pattern");
    
	d->meta = lxc_cgroup_load_meta();
	if (!d->meta) {
		ERROR("cgroupfs failed to detect cgroup metadata");
		goto err2;
	}

	print_cgfs_init_debuginfo(d);

	return d;

err2:
	free(d->name);
err1:
	free(d);
	return NULL;
}

static void cgfs_destroy(void *hdata)
{
	struct cgfs_data *d = hdata;

	if (!d)
		return;
	free(d->name);
	lxc_cgroup_process_info_free_and_remove(d->info);
	lxc_cgroup_put_meta(d->meta);
	free(d);
}

//cgroup_create中执行 
//创建/sys/fs/cgroup/cpuacct/lxc/  /sys/fs/cgroup/cpuacct/lxc/yyz-test 及其他cgroup mount挂载点下面的lxc  lxc/yyz-test目录
static inline bool cgfs_create(void *hdata)
{
	struct cgfs_data *d = hdata;
	struct cgroup_process_info *i;
	struct cgroup_meta_data *md;

	if (!d)
		return false;
	md = d->meta;
	//printf("  .......... pattern:%s\r\n", d->cgroup_pattern);  pattern:/lxc/%n
	i = lxc_cgroupfs_create(d->name, d->cgroup_pattern, md, NULL);
	if (!i)
		return false;
	d->info = i;
	return true;
}

//cgroup_enter中调用 //把pid进程加入到cgruop,就是把进程pid写入/sys/fs/cgroup/freezer/lxc/yyz-test/tasks中
static inline bool cgfs_enter(void *hdata, pid_t pid) //pid是子进程Pid
{
	struct cgfs_data *d = hdata;
	struct cgroup_process_info *i;
	int ret;

	if (!d)
		return false;
	i = d->info;
	ret = lxc_cgroupfs_enter(i, pid, false);

	return ret == 0;
}

////mount点为ns的需要更改路径
static inline bool cgfs_create_legacy(void *hdata, pid_t pid)
{
	struct cgfs_data *d = hdata;
	struct cgroup_process_info *i;

	if (!d)
		return false;
	i = d->info;
	if (lxc_cgroup_create_legacy(i, d->name, pid) < 0) {
		ERROR("failed to create legacy ns cgroups for '%s'", d->name);
		return false;
	}
	return true;
}

static const char *cgfs_get_cgroup(void *hdata, const char *subsystem)
{
	struct cgfs_data *d = hdata;

	if (!d)
		return NULL;
	return lxc_cgroup_get_hierarchy_path_data(subsystem, d);
}

static bool cgfs_unfreeze(void *hdata)
{
	struct cgfs_data *d = hdata;
	char *cgabspath, *cgrelpath;
	int ret;

	if (!d)
		return false;

	cgrelpath = lxc_cgroup_get_hierarchy_path_data("freezer", d);
	cgabspath = lxc_cgroup_find_abs_path("freezer", cgrelpath, true, NULL);
	if (!cgabspath)
		return false;

	ret = do_cgroup_set(cgabspath, "freezer.state", "THAWED");
	free(cgabspath);
	return ret == 0;
}

//cgroup_setup_limits中调用
static bool cgroupfs_setup_limits(void *hdata, struct lxc_list *cgroup_conf,
				  bool with_devices)
{
	struct cgfs_data *d = hdata;

	if (!d)
		return false;
	return do_setup_cgroup_limits(d, cgroup_conf, with_devices) == 0;
}

//cgroup_attach中执行
static bool lxc_cgroupfs_attach(const char *name, const char *lxcpath, pid_t pid)
{
	struct cgroup_meta_data *meta_data;
	struct cgroup_process_info *container_info;
	int ret;

	meta_data = lxc_cgroup_load_meta();
	if (!meta_data) {
		ERROR("could not move attached process %d to cgroup of container", pid);
		return false;
	}

	container_info = lxc_cgroup_get_container_info(name, lxcpath, meta_data);
	lxc_cgroup_put_meta(meta_data);
	if (!container_info) {
		ERROR("could not move attached process %d to cgroup of container", pid);
		return false;
	}

	ret = lxc_cgroupfs_enter(container_info, pid, false);
	lxc_cgroup_process_info_free(container_info);
	if (ret < 0) {
		ERROR("could not move attached process %d to cgroup of container", pid);
		return false;
	}
	return true;
}

static struct cgroup_ops cgfs_ops = {
	.init = cgfs_init,
	.destroy = cgfs_destroy,
	.create = cgfs_create,
	.enter = cgfs_enter,
	.create_legacy = cgfs_create_legacy,
	.get_cgroup = cgfs_get_cgroup,
	.get = lxc_cgroupfs_get,
	.set = lxc_cgroupfs_set,
	.unfreeze = cgfs_unfreeze,
	.setup_limits = cgroupfs_setup_limits,
	.name = "cgroupfs",
	.attach = lxc_cgroupfs_attach,
	.chown = NULL,
	.mount_cgroup = cgroupfs_mount_cgroup,
	.nrtasks = cgfs_nrtasks,
};
