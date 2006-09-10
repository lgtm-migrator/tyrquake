/*
Copyright (C) 2002 Kevin Shanahan

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

/*
 * This whole setup is butt-ugly. Proceed with caution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "rb_tree.h"
#include "shell.h"
#include "sys.h"
#include "zone.h"

const char **completions_list = NULL;
static int num_completions = 0;

static struct rb_root completions = RB_ROOT;

/*
 * FIXME: document assumptions about args; see caller below
 */
static struct completion *
rb_find_exact_r(struct rb_node *n, const char *str, unsigned long type)
{
    if (n) {
	struct completion *c, *ret = NULL; /* FIXME - null right here? */

	c = rb_entry(n, struct completion, rb_cmd_cache);
	if (!strcasecmp(str, c->string)) {
	    ret = rb_find_exact_r(n->rb_left, str, type);
	    if (!ret && (c->cmd_type & type))
		ret = c;
	    if (!ret)
		ret = rb_find_exact_r(n->rb_right, str, type);
	}
	return ret;
    }
    return NULL;
}


/*
 * Find exact string
 */
static int
rb_find_exact(const char *str, unsigned long type, struct rb_root *root)
{
    struct rb_node *n = root->rb_node;
    struct completion *c;
    int cmp;

    /* Do the first part iteratively */
    while (n) {
	c = rb_entry(n, struct completion, rb_cmd_cache);
	cmp = strcasecmp(str, c->string);
	if (cmp < 0)
	    n = n->rb_left;
	else if (cmp > 0)
	    n = n->rb_right;
	else
	    break;
    }
    /* now take care of the type matching */
    return (rb_find_exact_r(n, str, type) != NULL);
}


/*
 * str, type - the insertion key (only use one type 'flag' here)
 * root - root of the tree being inserted into
 * node - the node being inserted
 */
static struct completion *
rb_insert_completion__(const char *str, unsigned long type,
		       struct rb_root *root, struct rb_node *node)
{
    struct rb_node **p = &root->rb_node;
    struct rb_node *parent = NULL;
    struct completion *c;
    int cmp;

    while (*p) {
	parent = *p;
	c = rb_entry(parent, struct completion, rb_cmd_cache);

	cmp = strcasecmp(str, c->string);
	if (cmp < 0)
	    p = &(*p)->rb_left;
	else if (cmp > 0)
	    p = &(*p)->rb_right;
	else if (type < c->cmd_type)
	    p = &(*p)->rb_left;
	else if (type > c->cmd_type)
	    p = &(*p)->rb_right;
	else
	    return c; /* Already match in cache */
    }
    rb_link_node(node, parent, p);

    return NULL;
}

static void
rb_insert_completion(const char *str, unsigned long type, struct rb_root *root)
{
    struct completion *c, *ret;

    c = Z_Malloc(sizeof(struct completion));
    c->string = str;
    c->cmd_type = type;

    ret = rb_insert_completion__(str, type, root, &c->rb_cmd_cache);

    /* FIXME: insertion failure is important ; return error code and handle */
    if (ret == NULL)
	rb_insert_color(&c->rb_cmd_cache, root);
    else {
	Con_DPrintf("** Attempted to insert duplicate completion: %s\n", str);
	Z_Free(c);
    }
}

/*
 * FIXME: when I'm not so tired, I'll make this more efficient...
 */
static unsigned
rb_count_completions_r(struct rb_node *n, const char *str, unsigned long type)
{
    unsigned cnt = 0;

    if (n) {
	struct completion *c = rb_entry(n, struct completion, rb_cmd_cache);
	int cmp = strncasecmp(str, c->string, strlen(str));
	if (cmp <= 0)
	    cnt += rb_count_completions_r(n->rb_left, str, type);
	if (!cmp && (c->cmd_type & type))
	    cnt++;
	if (cmp >= 0)
	    cnt += rb_count_completions_r(n->rb_right, str, type);
    }
    return cnt;
}

static unsigned
rb_count_completions(struct rb_node *n, const char *str, unsigned long type)
{
    /* do the first part iteratively */
    while (n) {
	struct completion *c = rb_entry(n, struct completion, rb_cmd_cache);
	int cmp = strncasecmp(str, c->string, strlen(str));
	if (cmp < 0)
	    n = n->rb_left;
	else if (cmp > 0)
	    n = n->rb_right;
	else
	    break;
    }
    return rb_count_completions_r(n, str, type);
}

static void
rb_find_completions_r(struct rb_node *n, const char *str, unsigned long type)
{
    if (n) {
	struct completion *c = rb_entry(n, struct completion, rb_cmd_cache);
	int cmp = strncasecmp(str, c->string, strlen(str));
	if (cmp <= 0)
	    rb_find_completions_r(n->rb_left, str, type);
	if (!cmp && (c->cmd_type & type))
	    completions_list[num_completions++] = c->string;
	if (cmp >= 0)
	    rb_find_completions_r(n->rb_right, str, type);
    }
}

static unsigned
rb_find_completions(const char *str, unsigned long type, struct rb_root *root)
{
    struct rb_node *n;
    unsigned cnt;

    n = root->rb_node;
    cnt = rb_count_completions(n, str, type);

    if (cnt) {
	if (completions_list)
	    free(completions_list);

	completions_list = malloc(cnt * sizeof(char *));
	num_completions = 0;
	rb_find_completions_r(n, str, type);

	if (num_completions != cnt)
	    Con_DPrintf("**** WARNING: rb completions counts don't match!\n");
    }
    return cnt;
}

/*
 * Given the partial string 'str', find all strings in the tree that have
 * this prefix. Return the common prefix of all those strings, which may
 * be longer that the original str.
 *
 * Returned str is allocated on the zone, so caller to Z_Free it after use.
 * If no matches found, return null;
 */
static char *
rb_find_completion(const char *str, unsigned long type, struct rb_root *root)
{
    int n, i, max_match, len;
    char *ret;

    n = rb_find_completions(str, type, root);
    if (!n)
	return NULL;

    /*
     * What is the most that could possibly match?
     * i.e. shortest string in the list
     */
    max_match = strlen(completions_list[0]);
    for (i = 1; i < n; ++i) {
	len = strlen(completions_list[i]);
	if (len < max_match)
	    max_match = len;
    }

    /*
     * Check if we can match max_match chars. If not, reduce by one and
     * try again.
     */
    while (max_match > strlen(str)) {
	for (i = 1; i < n; ++i) {
	    if (strncasecmp(completions_list[0], completions_list[i],
			    max_match)) {
		max_match--;
		break;
	    }
	}
	if (i == n)
	    break;
    }

    ret = Z_Malloc(max_match + 1);
    strncpy(ret, completions_list[0], max_match);
    ret[max_match] = '\0';

    return ret;
}




/* Command completions */

void
insert_command_completion(const char *str)
{
    rb_insert_completion(str, CMD_COMMAND, &completions);
}

unsigned
find_command_completions(const char *str)
{
    return rb_find_completions(str, CMD_COMMAND, &completions);
}

char *
find_command_completion(const char *str)
{
    return rb_find_completion(str, CMD_COMMAND, &completions);
}

int
command_exists(const char *str)
{
    return rb_find_exact(str, CMD_COMMAND, &completions);
}

/* Alias completions */

void
insert_alias_completion(const char *str)
{
    rb_insert_completion(str, CMD_ALIAS, &completions);
}

unsigned
find_alias_completions(const char *str)
{
    return rb_find_completions(str, CMD_ALIAS, &completions);
}

char *
find_alias_completion(const char *str)
{
    return rb_find_completion(str, CMD_ALIAS, &completions);
}

int
alias_exists(const char *str)
{
    return rb_find_exact(str, CMD_ALIAS, &completions);
}


/* Cvar completions */

void
insert_cvar_completion(const char *str)
{
    rb_insert_completion(str, CMD_CVAR, &completions);
}

unsigned
find_cvar_completions(const char *str)
{
    return rb_find_completions(str, CMD_CVAR, &completions);
}

char *
find_cvar_completion(const char *str)
{
    return rb_find_completion(str, CMD_CVAR, &completions);
}

int
cvar_exists(const char *str)
{
    return rb_find_exact(str, CMD_CVAR, &completions);
}

/* ------------------------------------------------------------------------ */

/*
 * Find all the completions for a string, looking at commands, cvars and
 * aliases.
 */
unsigned
find_completions(const char *str)
{
    return rb_find_completions(str, CMD_COMMAND | CMD_CVAR | CMD_ALIAS,
			       &completions);
}


/*
 * For tab completion. Checks cmds, cvars and aliases
 */
char *find_completion(const char *str)
{
    return rb_find_completion(str, CMD_COMMAND | CMD_CVAR | CMD_ALIAS,
			      &completions);
}