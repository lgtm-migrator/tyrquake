/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

-----------------------------------------------------------------------

  To use rbtrees you'll have to implement your own insert and search cores.
  This will avoid us to use callbacks and to drop drammatically performances.
  I know it's not the cleaner way,  but in C (not in C++) to get
  performances and genericity...

  Some example of insert and search follows here. The search is a plain
  normal search over an ordered tree. The insert instead must be implemented
  int two steps: as first thing the code must insert the element in
  order as a red leaf in the tree, then the support library function
  rb_insert_color() must be called. Such function will do the
  not trivial work to rebalance the rbtree if necessary.

-----------------------------------------------------------------------

// FIXME - use own example here:

// TYR - comments:
//  offset - the search key
//  inode  - the (sub)tree to search
static inline struct page *
rb_search_page_cache(struct inode *inode, unsigned long offset)
{
    struct rb_node *n = inode->i_rb_page_cache.rb_node;
    struct page *page;

    while (n) {
        page = rb_entry(n, struct page, rb_page_cache);

	if (offset < page->offset)
	    n = n->rb_left;
	else if (offset > page->offset)
	    n = n->rb_right;
	else
	    return page;
    }
    return NULL;
}

// TYR - comments
//  node - pointer to rb_node structure being inserted
//
/ *
 * inode  - holds root of tree, tree is a page cache
 * offset - the insertion key (maybe could have got it from:
 *            rb_entry(node, struct page, rb_page_cache)->offset (?)
 * node   - rb_node being inserted
 * /
static inline struct page *
__rb_insert_page_cache(struct inode *inode, unsigned long offset,
		       struct rb_node * node)
{
    struct rb_node **p = &inode->i_rb_page_cache.rb_node;
    struct rb_node *parent = NULL;
    struct page *page;

    while (*p) {
	parent = *p;
	page = rb_entry(parent, struct page, rb_page_cache);

	if (offset < page->offset)
	    p = &(*p)->rb_left;
	else if (offset > page->offset)
	    p = &(*p)->rb_right;
	else
	    return page;
    }

    rb_link_node(node, parent, p);

    return NULL;
}

static inline struct page *
rb_insert_page_cache(struct inode *inode, unsigned long offset,
		     struct rb_node *node)
{
    struct page *ret;
    if ((ret = __rb_insert_page_cache(inode, offset, node)))
	goto out;
    rb_insert_color(node, &inode->i_rb_page_cache);
 out:
    return ret;
}

-----------------------------------------------------------------------
*/

#ifndef	RB_TREE_H
#define	RB_TREE_H

#include <stddef.h>

struct rb_node {
    struct rb_node *rb_parent;
    int rb_color;
#define	RB_RED		0
#define	RB_BLACK	1
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_ROOT	(struct rb_root) { NULL, }
#define	rb_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

/*
 *
 */
extern void rb_insert_color(struct rb_node *, struct rb_root *);

/*
 *
 */
extern void rb_erase(struct rb_node *, struct rb_root *);

static inline void
rb_link_node(struct rb_node *node, struct rb_node *parent,
	     struct rb_node **rb_link)
{
    node->rb_parent = parent;
    node->rb_color = RB_RED;
    node->rb_left = node->rb_right = NULL;

    *rb_link = node;
}

#endif /* RB_TREE_H */