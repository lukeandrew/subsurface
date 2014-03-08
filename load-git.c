#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <git2.h>

#include "dive.h"
#include "device.h"
#include "membuffer.h"

#define GIT_WALK_OK   0
#define GIT_WALK_SKIP 1

static struct dive *active_dive;
static dive_trip_t *active_trip;

static struct dive *create_new_dive(timestamp_t when)
{
	struct dive *dive = alloc_dive();

	/* We'll fill in more data from the dive file */
	dive->when = when;

	if (active_trip)
		add_dive_to_trip(dive, active_trip);
	record_dive(dive);

	return dive;
}

static dive_trip_t *create_new_trip(int yyyy, int mm, int dd)
{
	dive_trip_t *trip = calloc(1, sizeof(dive_trip_t));
	struct tm tm = { 0 };

	/* We'll fill in the real data from the trip descriptor file */
	tm.tm_year = yyyy;
	tm.tm_mon = mm-1;
	tm.tm_mday = dd;
	trip->when = utc_mktime(&tm);

	return trip;
}

static bool validate_date(int yyyy, int mm, int dd)
{
	return yyyy > 1970 && yyyy < 3000 &&
		mm > 0 && mm < 13 &&
		dd > 0 && dd < 32;
}

static bool validate_time(int h, int m, int s)
{
	return h >= 0 && h < 24 &&
		m >= 0 && m < 60 &&
		s >=0 && s <= 60;
}

/*
 * Dive trip directory, name is 'nn-alphabetic[~hex]'
 */
static int dive_trip_directory(const char *root, const char *name)
{
	int yyyy = -1, mm = -1, dd = -1;

	if (sscanf(root, "%d/%d", &yyyy, &mm) != 2)
		return GIT_WALK_SKIP;
	dd = atoi(name);
	if (!validate_date(yyyy, mm, dd))
		return GIT_WALK_SKIP;
	active_trip = create_new_trip(yyyy, mm, dd);
	return GIT_WALK_OK;
}

/*
 * Dive directory, name is [[yyyy-]mm-]nn-ddd-hh:mm:ss[~hex],
 * and 'timeoff' points to what should be the time part of
 * the name (the first digit of the hour).
 *
 * The root path will be of the form yyyy/mm[/tripdir],
 */
static int dive_directory(const char *root, const char *name, int timeoff)
{
	int yyyy = -1, mm = -1, dd = -1;
	int h, m, s;
	int mday_off = timeoff - 7;
	int month_off = mday_off - 3;
	int year_off = month_off - 5;
	struct tm tm;

	/* There has to be a mday */
	if (mday_off < 0)
		return GIT_WALK_SKIP;
	if (name[timeoff-1] != '-')
		return GIT_WALK_SKIP;

	/* Get the time of day */
	if (sscanf(name+timeoff, "%d:%d:%d", &h, &m, &s) != 3)
		return GIT_WALK_SKIP;
	if (!validate_time(h, m, s))
		return GIT_WALK_SKIP;

	/*
	 * Using the "git_tree_walk()" interface is simple, but
	 * it kind of sucks as an interface because there is
	 * no sane way to pass the hierarchy to the callbacks.
	 * The "payload" is a fixed one-time thing: we'd like
	 * the "current trip" to be passed down to the dives
	 * that get parsed under that trip, but we can't.
	 *
	 * So "active_trip" is not the trip that is in the hierarchy
	 * _above_ us, it's just the trip that was _before_ us. But
	 * if a dive is not in a trip at all, we can't tell.
	 *
	 * We could just do a better walker that passes the
	 * return value around, but we hack around this by
	 * instead looking at the one hierarchical piece of
	 * data we have: the pathname to the current entry.
	 *
	 * This is pretty hacky. The magic '8' is the length
	 * of a pathname of the form 'yyyy/mm/'.
	 */
	if (strlen(root) == 8)
		active_trip = NULL;

	/*
	 * Get the date. The day of the month is in the dive directory
	 * name, the year and month might be in the path leading up
	 * to it.
	 */
	dd = atoi(name + mday_off);
	if (year_off < 0) {
		if (sscanf(root, "%d/%d", &yyyy, &mm) != 2)
			return GIT_WALK_SKIP;
	} else
		yyyy = atoi(name + year_off);
	if (month_off >= 0)
		mm = atoi(name + month_off);

	if (!validate_date(yyyy, mm, dd))
		return GIT_WALK_SKIP;

	/* Ok, close enough. We've gotten sufficient information */
	memset(&tm, 0, sizeof(tm));
	tm.tm_hour = h;
	tm.tm_min = m;
	tm.tm_sec = s;
	tm.tm_year = yyyy - 1900;
	tm.tm_mon = mm-1;
	tm.tm_mday = dd;

	active_dive = create_new_dive(utc_mktime(&tm));
	return GIT_WALK_OK;
}

/*
 * Return the length of the string without the unique part.
 */
static int nonunique_length(const char *str)
{
	int len = 0;

	for (;;) {
		char c = *str++;
		if (!c || c == '~')
			return len;
		len++;
	}
}

/*
 * When hitting a directory node, we have a couple of cases:
 *
 *  - It's just a date entry - all numeric (either year or month):
 *
 *       [yyyy|mm]
 *
 *    We don't do anything with these, we just traverse into them.
 *    The numeric data will show up as part of the full path when
 *    we hit more interesting entries.
 *
 *  - It's a trip directory. The name will be of the form
 *
 *       nn-alphabetic[~hex]
 *
 *    where 'nn' is the day of the month (year and month will be
 *    encoded in the path leading up to this).
 *
 *  - It's a dive directory. The name will be of the form
 *
 *       [[yyyy-]mm-]nn-ddd-hh:mm:ss[~hex]
 *
 *    which describes the date and time of a dive (yyyy and mm
 *    are optional, and may be encoded in the path leading up to
 *    the dive).
 *
 *  - It's some random non-dive-data directory.
 *
 *    Subsurface doesn't create these yet, but maybe we'll encode
 *    pictures etc. If it doesn't match the above patterns, we'll
 *    ignore them for dive loading purposes, and not even recurse
 *    into them.
 */
static int walk_tree_directory(const char *root, const git_tree_entry *entry)
{
	const char *name = git_tree_entry_name(entry);
	int digits = 0, len;
	char c;

	while (isdigit(c = name[digits]))
		digits++;

	/* Doesn't start with two or four digits? Skip */
	if (digits != 4 && digits != 2)
		return GIT_WALK_SKIP;

	/* Only digits? Do nothing, but recurse into it */
	if (!c)
		return GIT_WALK_OK;

	/* All valid cases need to have a slash following */
	if (c != '-')
		return GIT_WALK_SKIP;

	/* Do a quick check for a common dive case */
	len = nonunique_length(name);

	/*
	 * We know the len is at least 3, because we had at least
	 * two digits and a dash
	 */
	if (name[len-3] == ':')
		return dive_directory(root, name, len-8);

	if (digits != 2)
		return GIT_WALK_SKIP;

	return dive_trip_directory(root, name);
}

git_blob *git_tree_entry_blob(git_repository *repo, const git_tree_entry *entry)
{
	const git_oid *id = git_tree_entry_id(entry);
	git_blob *blob;

	if (git_blob_lookup(&blob, repo, id))
		return NULL;
	return blob;
}

/*
 * We should *really* try to delay the dive computer data parsing
 * until necessary, in order to reduce load-time. The parsing is
 * cheap, but the loading of the git blob into memory can be pretty
 * costly.
 */
static int parse_divecomputer_entry(git_repository *repo, const git_tree_entry *entry, const char *suffix)
{
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read divecomputer file");
	git_blob_free(blob);
	return 0;
}

static int parse_dive_entry(git_repository *repo, const git_tree_entry *entry, const char *suffix)
{
	struct dive *dive = active_dive;
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read dive file");
	if (*suffix)
		dive->number = atoi(suffix+1);
	git_blob_free(blob);
	return 0;
}

static int parse_trip_entry(git_repository *repo, const git_tree_entry *entry)
{
	git_blob *blob = git_tree_entry_blob(repo, entry);
	if (!blob)
		return report_error("Unable to read trip file");
	git_blob_free(blob);
	return 0;
}

static int walk_tree_file(const char *root, const git_tree_entry *entry, git_repository *repo)
{
	struct dive *dive = active_dive;
	dive_trip_t *trip = active_trip;
	const char *name = git_tree_entry_name(entry);

	if (dive && !strncmp(name, "Divecomputer", 12))
		return parse_divecomputer_entry(repo, entry, name+12);
	if (dive && !strncmp(name, "Dive", 4))
		return parse_dive_entry(repo, entry, name+4);
	if (trip && !strcmp(name, "00-Trip"))
		return parse_trip_entry(repo, entry);
	report_error("Unknown file %s%s (%p %p)", root, name, dive, trip);
	return GIT_WALK_SKIP;
}

static int walk_tree_cb(const char *root, const git_tree_entry *entry, void *payload)
{
	git_repository *repo = payload;
	git_filemode_t mode = git_tree_entry_filemode(entry);

	if (mode == GIT_FILEMODE_TREE)
		return walk_tree_directory(root, entry);

	walk_tree_file(root, entry, repo);
	/* Ignore failed blob loads */
	return GIT_WALK_OK;
}

static int load_dives_from_tree(git_repository *repo, git_tree *tree)
{
	git_tree_walk(tree, GIT_TREEWALK_PRE, walk_tree_cb, repo);
	return 0;
}

static int do_git_load(git_repository *repo, const char *branch)
{
	int ret;
	git_reference *ref;
	git_object *tree;

	ret = git_branch_lookup(&ref, repo, branch, GIT_BRANCH_LOCAL);
	if (ret)
		return report_error("Unable to look up branch '%s'", branch);
	if (git_reference_peel(&tree, ref, GIT_OBJ_TREE))
		return report_error("Could not look up tree of branch '%s'", branch);
	ret = load_dives_from_tree(repo, (git_tree *) tree);
	git_object_free(tree);
	return ret;
}

int git_load_dives(char *where)
{
	int ret, len;
	git_repository *repo;
	char *loc, *branch;

	/* Jump over the "git" marker */
	loc = where + 3;
	while (isspace(*loc))
		loc++;

	/* Trim whitespace from the end */
	len = strlen(loc);
	while (len && isspace(loc[len-1]))
		loc[--len] = 0;

	/* Find a branch name if there is any */
	branch = strrchr(loc, ':');
	if (branch)
		*branch++ = 0;

	if (git_repository_open(&repo, loc))
		return report_error("Unable to open git repository at '%s' (branch '%s')", loc, branch);

	ret = do_git_load(repo, branch);
	git_repository_free(repo);
	return ret;
}