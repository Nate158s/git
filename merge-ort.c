/*
 * "Ostensibly Recursive's Twin" merge strategy, or "ort" for short.  Meant
 * as a drop-in replacement for the "recursive" merge strategy, allowing one
 * to replace
 *
 *   git merge [-s recursive]
 *
 * with
 *
 *   git merge -s ort
 *
 * Note: git's parser allows the space between '-s' and its argument to be
 * missing.  (Should I have backronymed "ham", "alsa", "kip", "nap, "alvo",
 * "cale", "peedy", or "ins" instead of "ort"?)
 */

#include "cache.h"
#include "merge-ort.h"

#include "diff.h"
#include "diffcore.h"
#include "strmap.h"
#include "tree.h"
#include "xdiff-interface.h"

struct merge_options_internal {
	struct strmap paths;    /* maps path -> (merged|conflict)_info */
	struct strmap unmerged; /* maps path -> conflict_info */
	const char *current_dir_name;
	int call_depth;
};

struct version_info {
	struct object_id oid;
	unsigned short mode;
};

struct merged_info {
	struct version_info result;
	unsigned is_null:1;
	unsigned clean:1;
	size_t basename_offset;
	 /*
	  * Containing directory name.  Note that we assume directory_name is
	  * constructed such that
	  *    strcmp(dir1_name, dir2_name) == 0 iff dir1_name == dir2_name,
	  * i.e. string equality is equivalent to pointer equality.  For this
	  * to hold, we have to be careful setting directory_name.
	  */
	const char *directory_name;
};

struct conflict_info {
	struct merged_info merged;
	struct version_info stages[3];
	const char *pathnames[3];
	unsigned df_conflict:1;
	unsigned path_conflict:1;
	unsigned filemask:3;
	unsigned dirmask:3;
	unsigned match_mask:3;
};

static int err(struct merge_options *opt, const char *err, ...)
{
	va_list params;
	struct strbuf sb = STRBUF_INIT;

	strbuf_addstr(&sb, "error: ");
	va_start(params, err);
	strbuf_vaddf(&sb, err, params);
	va_end(params);

	error("%s", sb.buf);
	strbuf_release(&sb);

	return -1;
}

static void setup_path_info(struct merge_options *opt,
			    struct string_list_item *result,
			    const char *current_dir_name,
			    int current_dir_name_len,
			    char *fullpath, /* we'll take over ownership */
			    struct name_entry *names,
			    struct name_entry *merged_version,
			    unsigned is_null,     /* boolean */
			    unsigned df_conflict, /* boolean */
			    unsigned filemask,
			    unsigned dirmask,
			    int resolved          /* boolean */)
{
	struct conflict_info *path_info;

	assert(!is_null || resolved);
	assert(!df_conflict || !resolved); /* df_conflict implies !resolved */
	assert(resolved == (merged_version != NULL));

	path_info = xcalloc(1, resolved ? sizeof(struct merged_info) :
					  sizeof(struct conflict_info));
	path_info->merged.directory_name = current_dir_name;
	path_info->merged.basename_offset = current_dir_name_len;
	path_info->merged.clean = !!resolved;
	if (resolved) {
		path_info->merged.result.mode = merged_version->mode;
		oidcpy(&path_info->merged.result.oid, &merged_version->oid);
		path_info->merged.is_null = !!is_null;
	} else {
		int i;

		for (i = 0; i < 3; i++) {
			path_info->pathnames[i] = fullpath;
			path_info->stages[i].mode = names[i].mode;
			oidcpy(&path_info->stages[i].oid, &names[i].oid);
		}
		path_info->filemask = filemask;
		path_info->dirmask = dirmask;
		path_info->df_conflict = !!df_conflict;
	}
	strmap_put(&opt->priv->paths, fullpath, path_info);
	result->string = fullpath;
	result->util = path_info;
}

static int collect_merge_info_callback(int n,
				       unsigned long mask,
				       unsigned long dirmask,
				       struct name_entry *names,
				       struct traverse_info *info)
{
	/*
	 * n is 3.  Always.
	 * common ancestor (mbase) has mask 1, and stored in index 0 of names
	 * head of side 1  (side1) has mask 2, and stored in index 1 of names
	 * head of side 2  (side2) has mask 4, and stored in index 2 of names
	 */
	struct merge_options *opt = info->data;
	struct merge_options_internal *opti = opt->priv;
	struct string_list_item pi;  /* Path Info */
	struct conflict_info *ci; /* pi.util when there's a conflict */
	struct name_entry *p;
	size_t len;
	char *fullpath;
	const char *dirname = opti->current_dir_name;
	unsigned filemask = mask & ~dirmask;
	unsigned match_mask = 0; /* will be updated below */
	unsigned mbase_null = !(mask & 1);
	unsigned side1_null = !(mask & 2);
	unsigned side2_null = !(mask & 4);
	unsigned side1_matches_mbase = (!side1_null && !mbase_null &&
					names[0].mode == names[1].mode &&
					oideq(&names[0].oid, &names[1].oid));
	unsigned side2_matches_mbase = (!side2_null && !mbase_null &&
					names[0].mode == names[2].mode &&
					oideq(&names[0].oid, &names[2].oid));
	unsigned sides_match = (!side1_null && !side2_null &&
				names[1].mode == names[2].mode &&
				oideq(&names[1].oid, &names[2].oid));
	/*
	 * Note: We only label files with df_conflict, not directories.
	 * Since directories stay where they are, and files move out of the
	 * way to make room for a directory, we don't care if there was a
	 * directory/file conflict for a parent directory of the current path.
	 */
	unsigned df_conflict = (filemask != 0) && (dirmask != 0);

	/* n = 3 is a fundamental assumption. */
	if (n != 3)
		BUG("Called collect_merge_info_callback wrong");

	/*
	 * A bunch of sanity checks verifying that traverse_trees() calls
	 * us the way I expect.  Could just remove these at some point,
	 * though maybe they are helpful to future code readers.
	 */
	assert(mbase_null == is_null_oid(&names[0].oid));
	assert(side1_null == is_null_oid(&names[1].oid));
	assert(side2_null == is_null_oid(&names[2].oid));
	assert(!mbase_null || !side1_null || !side2_null);
	assert(mask > 0 && mask < 8);

	/* Other invariant checks, mostly for documentation purposes. */
	assert(mask == (dirmask | filemask));

	/* Determine match_mask */
	if (side1_matches_mbase)
		match_mask = (side2_matches_mbase ? 7 : 3);
	else if (side2_matches_mbase)
		match_mask = 5;
	else if (sides_match)
		match_mask = 6;

	/*
	 * Get the name of the relevant filepath, which we'll pass to
	 * setup_path_info() for tracking.
	 */
	p = names;
	while (!p->mode)
		p++;
	len = traverse_path_len(info, p->pathlen);

	/* +1 in both of the following lines to include the NUL byte */
	fullpath = xmalloc(len+1);
	make_traverse_path(fullpath, len+1, info, p->path, p->pathlen);

	/*
	 * If mbase, side1, and side2 all match, we can resolve early.  Even
	 * if these are trees, there will be no renames or anything
	 * underneath.
	 */
	if (side1_matches_mbase && side2_matches_mbase) {
		/* mbase, side1, & side2 all match; use mbase as resolution */
		setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
				names, names+0, mbase_null, 0,
				filemask, dirmask, 1);
		return mask;
	}

	/*
	 * Record information about the path so we can resolve later in
	 * process_entries.
	 */
	setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
			names, NULL, 0, df_conflict, filemask, dirmask, 0);
	ci = pi.util;
	ci->match_mask = match_mask;

	/* If dirmask, recurse into subdirectories */
	if (dirmask) {
		struct traverse_info newinfo;
		struct tree_desc t[3];
		void *buf[3] = {NULL,};
		const char *original_dir_name;
		int i, ret;

		ci->match_mask &= filemask;
		newinfo = *info;
		newinfo.prev = info;
		newinfo.name = p->path;
		newinfo.namelen = p->pathlen;
		newinfo.pathlen = st_add3(newinfo.pathlen, p->pathlen, 1);
		/*
		 * If we did care about parent directories having a D/F
		 * conflict, then we'd include
		 *    newinfo.df_conflicts |= (mask & ~dirmask);
		 * here.  But we don't.  (See comment near setting of local
		 * df_conflict variable near the beginning of this function).
		 */

		for (i = 0; i < 3; i++, dirmask >>= 1) {
			if (i == 1 && side1_matches_mbase)
				t[1] = t[0];
			else if (i == 2 && side2_matches_mbase)
				t[2] = t[0];
			else if (i == 2 && sides_match)
				t[2] = t[1];
			else {
				const struct object_id *oid = NULL;
				if (dirmask & 1)
					oid = &names[i].oid;
				buf[i] = fill_tree_descriptor(opt->repo,
							      t + i, oid);
			}
		}

		original_dir_name = opti->current_dir_name;
		opti->current_dir_name = pi.string;
		ret = traverse_trees(NULL, 3, t, &newinfo);
		opti->current_dir_name = original_dir_name;

		for (i = 0; i < 3; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}

	return mask;
}

static int collect_merge_info(struct merge_options *opt,
			      struct tree *merge_base,
			      struct tree *side1,
			      struct tree *side2)
{
	int ret;
	struct tree_desc t[3];
	struct traverse_info info;
	char *toplevel_dir_placeholder = "";

	opt->priv->current_dir_name = toplevel_dir_placeholder;
	setup_traverse_info(&info, toplevel_dir_placeholder);
	info.fn = collect_merge_info_callback;
	info.data = opt;
	info.show_all_errors = 1;

	parse_tree(merge_base);
	parse_tree(side1);
	parse_tree(side2);
	init_tree_desc(t+0, merge_base->buffer, merge_base->size);
	init_tree_desc(t+1, side1->buffer, side1->size);
	init_tree_desc(t+2, side2->buffer, side2->size);

	ret = traverse_trees(NULL, 3, t, &info);

	return ret;
}

static int detect_and_process_renames(struct merge_options *opt,
				      struct tree *merge_base,
				      struct tree *side1,
				      struct tree *side2)
{
	int clean = 1;

	/*
	 * Rename detection works by detecting file similarity.  Here we use
	 * a really easy-to-implement scheme: files are similar IFF they have
	 * the same filename.  Therefore, by this scheme, there are no renames.
	 *
	 * TODO: Actually implement a real rename detection scheme.
	 */
	return clean;
}

static int string_list_df_name_compare(const char *one, const char *two)
{
	int onelen = strlen(one);
	int twolen = strlen(two);
	/*
	 * Here we only care that entries for D/F conflicts are
	 * adjacent, in particular with the file of the D/F conflict
	 * appearing before files below the corresponding directory.
	 * The order of the rest of the list is irrelevant for us.
	 *
	 * To achieve this, we sort with df_name_compare and provide
	 * the mode S_IFDIR so that D/F conflicts will sort correctly.
	 * We use the mode S_IFDIR for everything else for simplicity,
	 * since in other cases any changes in their order due to
	 * sorting cause no problems for us.
	 */
	int cmp = df_name_compare(one, onelen, S_IFDIR,
				  two, twolen, S_IFDIR);
	/*
	 * Now that 'foo' and 'foo/bar' compare equal, we have to make sure
	 * that 'foo' comes before 'foo/bar'.
	 */
	if (cmp)
		return cmp;
	return onelen - twolen;
}

struct directory_versions {
	struct string_list versions;
};

static void record_entry_for_tree(struct directory_versions *dir_metadata,
				  const char *path,
				  struct conflict_info *ci)
{
	const char *basename;

	if (ci->merged.is_null)
		/* nothing to record */
		return;

	basename = path + ci->merged.basename_offset;
	assert(strchr(basename, '/') == NULL);
	string_list_append(&dir_metadata->versions,
			   basename)->util = &ci->merged.result;
}

/* Per entry merge function */
static void process_entry(struct merge_options *opt,
			  const char *path,
			  struct conflict_info *ci,
			  struct directory_versions *dir_metadata)
{
	assert(!ci->merged.clean);
	assert(ci->filemask >= 0 && ci->filemask <= 7);

	if (ci->filemask == 0) {
		/*
		 * This is a placeholder for directories that were recursed
		 * into; nothing to do in this case.
		 */
		return;
	}

	if (ci->df_conflict) {
		die("Not yet implemented.");
	}

	/*
	 * NOTE: Below there is a long switch-like if-elseif-elseif... block
	 *       which the code goes through even for the df_conflict cases
	 *       above.  Well, it will once we don't die-not-implemented above.
	 */
	if (ci->match_mask) {
		ci->merged.clean = 1;
		if (ci->match_mask == 6) {
			/* stages[1] == stages[2] */
			ci->merged.result.mode = ci->stages[1].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
		} else {
			/* determine the mask of the side that didn't match */
			unsigned int othermask = 7 & ~ci->match_mask;
			int side = (othermask == 4) ? 2 : 1;

			ci->merged.is_null = (ci->filemask == ci->match_mask);
			ci->merged.result.mode = ci->stages[side].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);

			assert(othermask == 2 || othermask == 4);
			assert(ci->merged.is_null == !ci->merged.result.mode);
		}
	} else if (ci->filemask >= 6 &&
		   (S_IFMT & ci->stages[1].mode) !=
		   (S_IFMT & ci->stages[2].mode)) {
		/*
		 * Two different items from (file/submodule/symlink)
		 */
		die("Not yet implemented.");
	} else if (ci->filemask >= 6) {
		/*
		 * TODO: Needs a two-way or three-way content merge, but we're
		 * just being lazy and copying the version from HEAD and
		 * leaving it as conflicted.
		 */
		ci->merged.clean = 0;
		ci->merged.result.mode = ci->stages[1].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
	} else if (ci->filemask == 3 || ci->filemask == 5) {
		/* Modify/delete */
		die("Not yet implemented.");
	} else if (ci->filemask == 2 || ci->filemask == 4) {
		/* Added on one side */
		int side = (ci->filemask == 4) ? 2 : 1;
		ci->merged.result.mode = ci->stages[side].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
		ci->merged.clean = !ci->df_conflict && !ci->path_conflict;
	} else if (ci->filemask == 1) {
		/* Deleted on both sides */
		ci->merged.is_null = 1;
		ci->merged.result.mode = 0;
		oidcpy(&ci->merged.result.oid, &null_oid);
		ci->merged.clean = !ci->path_conflict;
	}

	/*
	 * If still unmerged, record it separately.  This allows us to later
	 * iterate over just unmerged entries when updating the index instead
	 * of iterating over all entries.
	 */
	if (!ci->merged.clean)
		strmap_put(&opt->priv->unmerged, path, ci);
	record_entry_for_tree(dir_metadata, path, ci);
}

static void process_entries(struct merge_options *opt,
			    struct object_id *result_oid)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;
	struct string_list plist = STRING_LIST_INIT_NODUP;
	struct string_list_item *entry;
	struct directory_versions dir_metadata;

	if (strmap_empty(&opt->priv->paths)) {
		oidcpy(result_oid, opt->repo->hash_algo->empty_tree);
		return;
	}

	/* Hack to pre-allocate plist to the desired size */
	ALLOC_GROW(plist.items, strmap_get_size(&opt->priv->paths), plist.alloc);

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(&opt->priv->paths, &iter, e) {
		string_list_append(&plist, e->key)->util = e->value;
	}
	plist.cmp = string_list_df_name_compare;
	string_list_sort(&plist);

	/* other setup */
	string_list_init(&dir_metadata.versions, 0);

	/*
	 * Iterate over the items in reverse order, so we can handle paths
	 * below a directory before needing to handle the directory itself.
	 */
	for (entry = &plist.items[plist.nr-1]; entry >= plist.items; --entry) {
		char *path = entry->string;
		/*
		 * WARNING: If ci->merged.clean is true, then ci does not
		 * actually point to a conflict_info but a struct merge_info.
		 */
		struct conflict_info *ci = entry->util;

		if (ci->merged.clean)
			record_entry_for_tree(&dir_metadata, path, ci);
		else
			process_entry(opt, path, ci, &dir_metadata);
	}

	string_list_clear(&plist, 0);
	string_list_clear(&dir_metadata.versions, 0);
	die("Tree creation not yet implemented");
}

void merge_switch_to_result(struct merge_options *opt,
			    struct tree *head,
			    struct merge_result *result,
			    int update_worktree_and_index,
			    int display_update_msgs)
{
	die("Not yet implemented");
	merge_finalize(opt, result);
}

void merge_finalize(struct merge_options *opt,
		    struct merge_result *result)
{
	die("Not yet implemented");
}

static void merge_start(struct merge_options *opt, struct merge_result *result)
{
	/* Sanity checks on opt */
	assert(opt->repo);

	assert(opt->branch1 && opt->branch2);

	assert(opt->detect_directory_renames >= MERGE_DIRECTORY_RENAMES_NONE &&
	       opt->detect_directory_renames <= MERGE_DIRECTORY_RENAMES_TRUE);
	assert(opt->rename_limit >= -1);
	assert(opt->rename_score >= 0 && opt->rename_score <= MAX_SCORE);
	assert(opt->show_rename_progress >= 0 && opt->show_rename_progress <= 1);

	assert(opt->xdl_opts >= 0);
	assert(opt->recursive_variant >= MERGE_VARIANT_NORMAL &&
	       opt->recursive_variant <= MERGE_VARIANT_THEIRS);

	/*
	 * detect_renames, verbosity, buffer_output, and obuf are ignored
	 * fields that were used by "recursive" rather than "ort" -- but
	 * sanity check them anyway.
	 */
	assert(opt->detect_renames >= -1 &&
	       opt->detect_renames <= DIFF_DETECT_COPY);
	assert(opt->verbosity >= 0 && opt->verbosity <= 5);
	assert(opt->buffer_output <= 2);
	assert(opt->obuf.len == 0);

	assert(opt->priv == NULL);

	/* Default to histogram diff.  Actually, just hardcode it...for now. */
	opt->xdl_opts = DIFF_WITH_ALG(opt, HISTOGRAM_DIFF);

	/* Initialization of opt->priv, our internal merge data */
	opt->priv = xcalloc(1, sizeof(*opt->priv));
	/*
	 * Although we initialize opt->priv->paths with strdup_strings=0,
	 * that's just to avoid making yet another copy of an allocated
	 * string.  Putting the entry into paths means we are taking
	 * ownership, so we will later free it.
	 *
	 * In contrast, unmerged just has a subset of keys from paths, so
	 * we don't want to free those (it'd be a duplicate free).
	 */
	strmap_init_with_options(&opt->priv->paths, NULL, 0);
	strmap_init_with_options(&opt->priv->unmerged, NULL, 0);
}

/*
 * Originally from merge_trees_internal(); heavily adapted, though.
 */
static void merge_ort_nonrecursive_internal(struct merge_options *opt,
					    struct tree *merge_base,
					    struct tree *side1,
					    struct tree *side2,
					    struct merge_result *result)
{
	struct object_id working_tree_oid;

	if (collect_merge_info(opt, merge_base, side1, side2) != 0) {
		err(opt, _("collecting merge info failed for trees %s, %s, %s"),
		    oid_to_hex(&merge_base->object.oid),
		    oid_to_hex(&side1->object.oid),
		    oid_to_hex(&side2->object.oid));
		result->clean = -1;
		return;
	}

	result->clean = detect_and_process_renames(opt, merge_base,
						   side1, side2);
	process_entries(opt, &working_tree_oid);

	/* Set return values */
	result->tree = parse_tree_indirect(&working_tree_oid);
	/* existence of unmerged entries implies unclean */
	result->clean &= strmap_empty(&opt->priv->unmerged);
	if (!opt->priv->call_depth) {
		result->priv = opt->priv;
		opt->priv = NULL;
	}
}

void merge_incore_nonrecursive(struct merge_options *opt,
			       struct tree *merge_base,
			       struct tree *side1,
			       struct tree *side2,
			       struct merge_result *result)
{
	assert(opt->ancestor != NULL);
	merge_start(opt, result);
	merge_ort_nonrecursive_internal(opt, merge_base, side1, side2, result);
}

void merge_incore_recursive(struct merge_options *opt,
			    struct commit_list *merge_bases,
			    struct commit *side1,
			    struct commit *side2,
			    struct merge_result *result)
{
	die("Not yet implemented");
}
