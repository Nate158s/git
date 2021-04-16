/*
 * This is a port of Scalar to C.
 */

#include "cache.h"
#include "gettext.h"
#include "parse-options.h"
#include "config.h"
#include "run-command.h"
#include "strbuf.h"
#include "refs.h"
#include "version.h"
#include "dir.h"

static const char scalar_usage[] =
	N_("scalar <command> [<options>]\n\n"
	   "Commands: clone, config, diagnose, list\n"
	   "\tregister, run, unregister");

static char *scalar_executable_path;

static int run_git(const char *dir, const char *arg, ...)
{
	struct strvec argv = STRVEC_INIT;
	va_list args;
	const char *p;
	int res;

	va_start(args, arg);
	strvec_push(&argv, arg);
	while ((p = va_arg(args, const char *)))
		strvec_push(&argv, p);
	va_end(args);

	res = run_command_v_opt_cd_env(argv.v, RUN_GIT_CMD, dir, NULL);

	strvec_clear(&argv);
	return res;
}

static int is_non_empty_dir(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *entry;

	if (!dir) {
		if (errno != ENOENT) {
			error_errno(_("could not open directory '%s'"), path);
		}
		return 0;
	}

	while ((entry = readdir(dir))) {
		const char *name = entry->d_name;

		if (strcmp(name, ".") && strcmp(name, "..")) {
			closedir(dir);
			return 1;
		}
	}

	closedir(dir);
	return 0;
}

static int set_recommended_config(const char *file)
{
	struct {
		const char *key;
		const char *value;
	} config[] = {
		{ "am.keepCR", "true" },
		{ "commitGraph.generationVersion", "1" },
		{ "core.autoCRLF", "false" },
		{ "core.FSCache", "true" },
		{ "core.logAllRefUpdates", "true" },
		{ "core.multiPackIndex", "true" },
		{ "core.preloadIndex", "true" },
		{ "core.safeCRLF", "false" },
		{ "credential.validate", "false" },
		{ "feature.manyFiles", "false" },
		{ "feature.experimental", "false" },
		{ "fetch.unpackLimit", "1" },
		{ "fetch.writeCommitGraph", "false" },
		{ "gc.auto", "0" },
		{ "gui.GCWarning", "false" },
		{ "index.threads", "true" },
		{ "index.version", "4" },
		{ "maintenance.auto", "false" },
		{ "merge.stat", "false" },
		{ "merge.renames", "false" },
		{ "pack.useBitmaps", "false" },
		{ "pack.useSparse", "true" },
		{ "receive.autoGC", "false" },
		{ "reset.quiet", "true" },
		{ "status.aheadBehind", "false" },
#ifdef WIN32
		/*
		 * Windows-specific settings.
		 */
		{ "core.untrackedCache", "true" },
		{ "core.filemode", "true" },
#endif
		{ NULL, NULL },
	};
	int i;

	for (i = 0; config[i].key; i++) {
		char *value;

		if (file || git_config_get_string(config[i].key, &value)) {
			trace2_data_string("scalar", the_repository, config[i].key, "created");
			git_config_set_in_file_gently(file, config[i].key,
						      config[i].value);
		} else {
			trace2_data_string("scalar", the_repository, config[i].key, "exists");
			free(value);
		}
	}
	return 0;
}

/* printf-style interface, expects `<key>=<value>` argument */
static int set_config(const char *file, const char *fmt, ...)
{
	struct strbuf buf = STRBUF_INIT;
	char *value;
	int res;
	va_list args;

	va_start(args, fmt);
	strbuf_vaddf(&buf, fmt, args);
	va_end(args);

	value = strchr(buf.buf, '=');
	if (value)
		*(value++) = '\0';
	res = git_config_set_in_file_gently(file, buf.buf, value);
	strbuf_release(&buf);

	return res;
}

static char *remote_default_branch(const char *dir)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;

	cp.git_cmd = 1;
	cp.dir = dir;
	strvec_pushl(&cp.args, "ls-remote", "--symref", "origin", "HEAD", NULL);
	strbuf_addstr(&out, "-\n");
	if (!pipe_command(&cp, NULL, 0, &out, 0, NULL, 0)) {
		char *ref = out.buf;

		while ((ref = strstr(ref + 1, "\nref: "))) {
			const char *p;
			char *head, *branch;

			ref += strlen("\nref: ");
			head = strstr(ref, "\tHEAD");

			if (!head || memchr(ref, '\n', head - ref))
				continue;

			if (skip_prefix(ref, "refs/heads/", &p)) {
				branch = xstrndup(p, head - p);
				strbuf_release(&out);
				return branch;
			}

			error(_("remote HEAD is not a branch: '%.*s'"),
			      (int)(head - ref), ref);
			strbuf_release(&out);
			return NULL;
		}
	}
	warning(_("failed to get default branch name from remote; "
		  "using local default"));
	strbuf_reset(&out);

	child_process_init(&cp);
	cp.git_cmd = 1;
	cp.dir = dir;
	strvec_pushl(&cp.args, "symbolic-ref", "--short", "HEAD", NULL);
	if (!pipe_command(&cp, NULL, 0, &out, 0, NULL, 0)) {
		strbuf_trim(&out);
		return strbuf_detach(&out, NULL);
	}

	strbuf_release(&out);
	error(_("failed to get default branch name"));
	return NULL;
}

static int cmd_clone(int argc, const char **argv)
{
	int is_unattended = git_env_bool("Scalar_UNATTENDED", 0);
	char *cache_server_url = NULL, *branch = NULL;
	int single_branch = 0, no_fetch_commits_and_trees = 0;
	char *local_cache_path = NULL;
	int full_clone = 0;
	struct option clone_options[] = {
		OPT_STRING(0, "cache-server-url", &cache_server_url,
			   N_("<url>"),
			   N_("the url or friendly name of the cache server")),
		OPT_STRING('b', "branch", &branch, N_("<branch>"),
			   N_("branch to checkout after clone")),
		OPT_BOOL(0, "single-branch", &single_branch,
			 N_("only download metadata for the branch that will be checked out")),
		OPT_BOOL(0, "no-fetch-commits-and-trees",
			 &no_fetch_commits_and_trees,
			 N_("skip fetching commits and trees after clone")),
		OPT_STRING(0, "local-cache-path", &local_cache_path,
			   N_("<path>"),
			   N_("override the path for the local Scalar cache")),
		OPT_BOOL(0, "full-clone", &full_clone,
			 N_("when cloning, create full working directory")),
		OPT_END(),
	};
	const char * const clone_usage[] = {
		N_("git clone [<options>] [--] <repo> [<dir>]"),
		NULL
	};
	const char *url;
	char *root = NULL, *dir = NULL, *config_path = NULL;
	struct strbuf buf = STRBUF_INIT;
	int res;

	argc = parse_options(argc, argv, NULL, clone_options, clone_usage,
			     PARSE_OPT_KEEP_DASHDASH |
			     PARSE_OPT_STOP_AT_NON_OPTION);

	if (argc == 2) {
		url = argv[0];
		root = xstrdup(argv[1]);
	} else if (argc == 1) {
		url = argv[0];

		strbuf_addstr(&buf, url);
		/* Strip trailing slashes, if any */
		while (buf.len > 0 && is_dir_sep(buf.buf[buf.len - 1]))
			strbuf_setlen(&buf, buf.len - 1);
		/* Strip suffix `.git`, if any */
		strbuf_strip_suffix(&buf, ".git");

		root = find_last_dir_sep(buf.buf);
		if (!root) {
			die(_("cannot deduce worktree name from '%s'"), url);
		}
		root = xstrdup(root + 1);
	} else {
		usage_msg_opt(N_("need a URL"), clone_usage, clone_options);
	}

	dir = xstrfmt("%s/src", root);

	/* TODO: verify that '--local-cache-path' isn't inside the src folder */
	/* TODO: CheckNotInsideExistingRepo */

	if (is_non_empty_dir(dir)) {
		die(_("'%s' exists and is not empty"), dir);
	}

	strbuf_reset(&buf);
	if (branch)
		strbuf_addf(&buf, "init.defaultBranch=%s", branch);
	else {
		char *b = repo_default_branch_name(the_repository, 1);
		strbuf_addf(&buf, "init.defaultBranch=%s", b);
		free(b);
	}

	if ((res = run_git(NULL, "-c", buf.buf, "init", "--", dir, NULL)))
		goto cleanup;

	/* TODO: trace command-line options, is_unattended, elevated, dir */
	trace2_data_intmax("scalar", the_repository, "unattended",
			   is_unattended);

	/* TODO: handle local cache root */

	/* TODO: check whether to use the GVFS protocol */

	config_path = xstrfmt("%s/.git/config", dir);

	/* TODO: this should be removed, right? */
	/* protocol.version=2 is broken right now. */
	if (set_config(config_path, "protocol.version=1") ||
	    set_config(config_path, "remote.origin.url=%s", url) ||
	    /* TODO: should we respect single_branch here? */
	    set_config(config_path, "remote.origin.fetch="
		       "+refs/heads/*:refs/remotes/origin/*") ||
	    set_config(config_path, "remote.origin.promisor=true") ||
	    set_config(config_path,
		       "remote.origin.partialCloneFilter=blob:none"))
		return error(_("could not configure '%s'"), dir);

	if (!full_clone &&
	    (res = run_git(dir, "-c", "core.useGVFSHelper=false",
			   "sparse-checkout", "init", "--cone", NULL)))
		goto cleanup;

	if (set_recommended_config(config_path))
		return error(_("could not configure '%s'"), dir);

	/*
	 * TODO: should we pipe the output and grep for "filtering not
	 * recognized by server", and suppress the error output in
	 * that case?
	 */
	if ((res = run_git(dir, "-c", "core.useGVFSHelper=false", "fetch",
			   "--quiet", "origin", NULL))) {
		warning(_("Partial clone failed; Trying full clone"));

		if (set_config(config_path, "remote.origin.promisor") ||
		    set_config(config_path,
			       "remote.origin.partialCloneFilter")) {
			res = error(_("could not configure for full clone"));
			goto cleanup;
		}

		if ((res = run_git(dir, "-c", "core.useGVFSHelper=false",
				   "fetch", "--quiet", "origin", NULL)))
			goto cleanup;
	}

	if (!branch &&
	    !(branch = remote_default_branch(dir))) {
		res = error(_("failed to get default branch for '%s'"), url);
		goto cleanup;
	}

	if ((res = set_config(config_path, "branch.%s.remote=origin", branch)))
		goto cleanup;
	if ((res = set_config(config_path, "branch.%s.merge=refs/heads/%s",
			      branch, branch)))
		goto cleanup;

	res = run_git(dir, "-c", "core.useGVFSHelper=false",
		      "checkout", "-f", branch, NULL);

cleanup:
	free(root);
	free(dir);
	free(config_path);
	strbuf_release(&buf);
	free(branch);
	free(cache_server_url);
	free(local_cache_path);
	return res;
}

static int stage(const char *git_dir, struct strbuf *buf, const char *path)
{
	struct strbuf cacheinfo = STRBUF_INIT;
	struct child_process cp = CHILD_PROCESS_INIT;
	int res;

	strbuf_addstr(&cacheinfo, "100644,");

	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "--git-dir", git_dir,
		     "hash-object", "-w", "--stdin", NULL);
	res = pipe_command(&cp, buf->buf, buf->len, &cacheinfo, 256, NULL, 0);
	if (!res) {
		strbuf_rtrim(&cacheinfo);
		strbuf_addch(&cacheinfo, ',');
		/* We cannot stage `.git`, use `_git` instead. */
		if (starts_with(path, ".git/"))
			strbuf_addf(&cacheinfo, "_%s", path + 1);
		else
			strbuf_addstr(&cacheinfo, path);

		child_process_init(&cp);
		cp.git_cmd = 1;
		strvec_pushl(&cp.args, "--git-dir", git_dir,
			     "update-index", "--add", "--cacheinfo",
			     cacheinfo.buf, NULL);
		res = run_command(&cp);
	}

	strbuf_release(&cacheinfo);
	return res;
}

static int stage_file(const char *git_dir, const char *path)
{
	struct strbuf buf = STRBUF_INIT;
	int res;

	if (strbuf_read_file(&buf, path, 0) < 0)
		return error(_("could not read '%s'"), path);

	res = stage(git_dir, &buf, path);

	strbuf_release(&buf);
	return res;
}

static int stage_directory(const char *git_dir, const char *path, int recurse)
{
	int at_root = !*path;
	DIR *dir = opendir(at_root ? "." : path);
	struct dirent *e;
	struct strbuf buf = STRBUF_INIT;
	size_t len;
	int res = 0;

	if (!dir)
		return error(_("could not open directory '%s'"), path);

	if (!at_root)
		strbuf_addf(&buf, "%s/", path);
	len = buf.len;

	while (!res && (e = readdir(dir))) {
		if (!strcmp(".", e->d_name) || !strcmp("..", e->d_name))
			continue;

		strbuf_setlen(&buf, len);
		strbuf_addstr(&buf, e->d_name);

		if ((e->d_type == DT_REG && stage_file(git_dir, buf.buf)) ||
		    (e->d_type == DT_DIR && recurse &&
		     stage_directory(git_dir, buf.buf, recurse)))
			res = -1;
	}

	closedir(dir);
	strbuf_release(&buf);
	return res;
}

static int index_to_zip(const char *git_dir)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf oid = STRBUF_INIT;

	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "--git-dir", git_dir, "write-tree", NULL);
	if (pipe_command(&cp, NULL, 0, &oid, the_hash_algo->hexsz + 1,
			 NULL, 0))
		return error(_("could not write temporary tree object"));

	strbuf_rtrim(&oid);
	child_process_init(&cp);
	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "--git-dir", git_dir, "archive", "-o", NULL);
	strvec_pushf(&cp.args, "%s.zip", git_dir);
	strvec_pushl(&cp.args, oid.buf, "--", NULL);
	strbuf_release(&oid);
	return run_command(&cp);
}

static int cmd_diagnose(int argc, const char **argv)
{
	struct strbuf tmp_dir = STRBUF_INIT;
	time_t now = time(NULL);
	struct tm tm;
	struct strbuf path = STRBUF_INIT, buf = STRBUF_INIT;
	int res = 0;

	if (argc != 1)
		die("'scalar diagnose' does not accept any arguments");

	strbuf_addstr(&tmp_dir, ".scalarDiagnostics/scalar_");
	strbuf_addftime(&tmp_dir, "%Y%m%d_%H%M%S",
			localtime_r(&now, &tm), 0, 0);
	if (run_git(NULL, "init", "-q", "-b", "dummy",
		    "--bare", tmp_dir.buf, NULL)) {
		res = error(_("could not initialize temporary repository"));
		goto diagnose_cleanup;
	}

	strbuf_reset(&buf);
	strbuf_addf(&buf, "Collecting diagnostic info into temp folder %s\n\n",
		    tmp_dir.buf);

	strbuf_addf(&buf, "git version %s\n", git_version_string);
	strbuf_addf(&buf, "built from commit: %s\n\n",
		    git_built_from_commit_string[0] ?
		    git_built_from_commit_string : "(n/a)");

	strbuf_addf(&buf, "Enlistment root: %s\n", the_repository->worktree);
	strbuf_addf(&buf,
		    "Cache Server: None\n"
		    "Local Cache:\n"
		    "\n"
		    "TODO: acquire disk space information\n");
	fwrite(buf.buf, buf.len, 1, stdout);

	if ((res = stage(tmp_dir.buf, &buf, "diagnostics.log")))
		goto diagnose_cleanup;

	if ((res = stage_directory(tmp_dir.buf, ".git", 0)) ||
	    (res = stage_directory(tmp_dir.buf, ".git/hooks", 0)) ||
	    (res = stage_directory(tmp_dir.buf, ".git/info", 0)) ||
	    (res = stage_directory(tmp_dir.buf, ".git/logs", 1)) ||
	    (res = stage_directory(tmp_dir.buf, ".git/objects/info", 0)))
		goto diagnose_cleanup;

	/*
	 * TODO: add more stuff:
	 * disk space info
	 * LogDirectoryEnumeration(...DotGit.Objects.Root), ScalarConstants.DotGit.Objects.Pack.Root, "packs-local.txt");
	 * LogLooseObjectCount(...DotGit.Objects.Root), ScalarConstants.DotGit.Objects.Root, "objects-local.txt");
	 *
	 * CopyLocalCacheData(archiveFolderPath, gitObjectsRoot);
	 */

	res = index_to_zip(tmp_dir.buf);

	if (!res)
		res = remove_dir_recursively(&tmp_dir, 0);

	if (!res)
		printf("\n"
		       "Diagnostics complete.\n"
		       "All of the gathered info is captured in '%s.zip'\n",
		       tmp_dir.buf);

diagnose_cleanup:
	strbuf_release(&tmp_dir);
	strbuf_release(&path);
	strbuf_release(&buf);

	return res;
}

static int cmd_list(int argc, const char **argv)
{
	return run_git(NULL, "config", "--get-all", "scalar.repo", NULL);
}

static int add_or_remove_enlistment(int add)
{
	int res;

	if (!the_repository->worktree)
		die(_("Scalar enlistments require a worktree"));

	res = run_git(NULL, "config", "--global", "--get",
		      "--fixed-value", "scalar.repo", the_repository->worktree, NULL);

	/*
	 * If we want to add and the setting is already there, then do nothing.
	 * If we want to remove and the setting is not there, then do nothing.
	 */
	if ((add && !res) || (!add && res))
		return 0;

	return run_git(NULL, "config", "--global",
		       add ? "--add" : "--unset",
		       "--fixed-value", "scalar.repo",
		       the_repository->worktree, NULL);
}

static int toggle_maintenance(int enable)
{
	return run_git(NULL, "maintenance", enable ? "start" : "unregister",
		       NULL);
}

static int run_config_task(void)
{
	int res = 0;

	res = res || add_or_remove_enlistment(1);
	res = res || set_recommended_config(NULL);
	res = res || toggle_maintenance(1);

	return res;
}

static int cmd_register(int argc, const char **argv)
{
	return run_config_task();
}

static const char scalar_run_usage[] =
	N_("scalar run <task>\n"
	   "\ttasks: all, config, commit-graph,\n"
	   "\t       fetch, loose-objects, pack-files");

static struct {
	const char *arg, *task;
} tasks[] = {
	{ "config", NULL },
	{ "commit-graph", "commit-graph" },
	{ "fetch", "prefetch" },
	{ "loose-objects", "loose-objects" },
	{ "pack-files", "incremental-repack" },
	{ NULL, NULL }
};

static int run_maintenance_task(const char *arg)
{
	int i;

	if (!strcmp("config", arg))
		return run_config_task();
	else if (!strcmp("all", arg)) {
		for (i = 0; tasks[i].arg; i++)
			if (run_maintenance_task(tasks[i].arg))
				return -1;
		return 0;
	}

	for (i = 0; tasks[i].arg; i++)
		if (!strcmp(tasks[i].arg, arg))
			return run_git(NULL, "maintenance", "run", "--task",
				       tasks[i].task, NULL);

	return error(_("no such task: '%s'"), arg);
}

static int cmd_run(int argc, const char **argv)
{
	if (argc != 2)
		usage(scalar_run_usage);

	return run_maintenance_task(argv[1]);
}

static int cmd_unregister(int argc, const char **argv)
{
	int res = 0;

	res = res || add_or_remove_enlistment(0);
	res = res || toggle_maintenance(0);
	return res;
}

static int cmd_test(int argc, const char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	printf("default for '%s': '%s'\n", dir, remote_default_branch(dir));
	return 0;
}

struct {
	const char *name;
	int (*fn)(int, const char **);
	int needs_git_repo;
} builtins[] = {
	{ "clone", cmd_clone, 0 },
	{ "diagnose", cmd_diagnose, 1 },
	{ "list", cmd_list, 0 },
	{ "register", cmd_register, 1 },
	{ "run", cmd_run, 1 },
	{ "unregister", cmd_unregister, 1 },
	{ "test", cmd_test, 0 },
	{ NULL, NULL},
};

int cmd_main(int argc, const char **argv)
{
	int i;

	if (argc < 2)
		usage(scalar_usage);

	scalar_executable_path = real_pathdup(argv[0], 0);
	if (!scalar_executable_path)
		die(_("could not determine full path of `scalar`"));

	argv++;
	argc--;

	for (i = 0; builtins[i].name; i++)
		if (!strcmp(builtins[i].name, argv[0])) {
			if (builtins[i].needs_git_repo)
				setup_git_directory();
			return builtins[i].fn(argc, argv);
		}

	usage(scalar_usage);
}
