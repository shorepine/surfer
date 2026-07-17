#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

surf_ctx surf_g;

/* ---- pool ---- */

static surf_node *node_alloc(uint8_t type)
{
    surf_node *n = surf_g.free_list;
    if (!n)
        return NULL;
    surf_g.free_list = n->next;
    memset(n, 0, sizeof *n);
    n->type = type;
    return n;
}

static void node_free(surf_node *n)
{
    n->type = SURF_NODE_FREE;
    n->next = surf_g.free_list;
    surf_g.free_list = n;
}

/* ---- lifecycle ---- */

bool surf_init(const surf_hal *hal, int16_t w, int16_t h, const surf_config *cfg)
{
    if (!hal || w <= 0 || h <= 0)
        return false;

    memset(&surf_g, 0, sizeof surf_g);
    surf_g.hal = hal;
    surf_g.w = w;
    surf_g.h = h;
    surf_g.bg = cfg ? cfg->bg : 0;
    surf_g.pool_cap = (cfg && cfg->max_nodes > 0) ? cfg->max_nodes : 256;

    /* Pools are sized once here; the frame path never allocates. */
    surf_g.pool = calloc((size_t)surf_g.pool_cap, sizeof(surf_node));
    surf_g.plist = calloc((size_t)surf_g.pool_cap, sizeof(surf_paint_ent));
    if (!surf_g.pool || !surf_g.plist) {
        free(surf_g.pool);
        free(surf_g.plist);
        memset(&surf_g, 0, sizeof surf_g);
        return false;
    }
    for (int i = surf_g.pool_cap - 1; i >= 0; i--)
        node_free(&surf_g.pool[i]);

    surf_g.root = node_alloc(SURF_NODE_GROUP);
    surf_dirty_reset(&surf_g.dirty, (surf_rect){0, 0, w, h});
    surf_dirty_add(&surf_g.dirty, (surf_rect){0, 0, w, h});
    return true;
}

void surf_deinit(void)
{
    free(surf_g.pool);
    free(surf_g.plist);
    memset(&surf_g, 0, sizeof surf_g);
}

surf_node *surf_screen(void)
{
    return surf_g.root;
}

void surf_tick(void)
{
    /* Animation/momentum ticks land here in later milestones. */
    surf_compose();
}

/* ---- damage ---- */

bool surf_node_attached(const surf_node *n)
{
    while (n->parent)
        n = n->parent;
    return n == surf_g.root;
}

surf_rect surf_node_subtree_bounds(const surf_node *n, int16_t px, int16_t py)
{
    if (n->flags & SURF_NF_HIDDEN)
        return (surf_rect){0, 0, 0, 0};

    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);
    if (n->type != SURF_NODE_GROUP)
        return (surf_rect){ax, ay, n->w, n->h};

    surf_rect b = {0, 0, 0, 0};
    for (const surf_node *c = n->first; c; c = c->next)
        b = surf_rect_union(b, surf_node_subtree_bounds(c, ax, ay));
    if (n->flags & SURF_NF_CLIP)
        b = surf_rect_intersect(b, (surf_rect){ax, ay, n->w, n->h});
    return b;
}

void surf_damage_subtree(const surf_node *n)
{
    if (!surf_g.root || !surf_node_attached(n))
        return;
    int16_t ax = 0, ay = 0;
    for (const surf_node *p = n->parent; p; p = p->parent) {
        ax = (int16_t)(ax + p->x);
        ay = (int16_t)(ay + p->y);
    }
    surf_dirty_add(&surf_g.dirty, surf_node_subtree_bounds(n, ax, ay));
}

/* ---- constructors ---- */

surf_node *surf_group_new(int16_t x, int16_t y)
{
    surf_node *n = node_alloc(SURF_NODE_GROUP);
    if (n) { n->x = x; n->y = y; }
    return n;
}

surf_node *surf_rect_new(int16_t x, int16_t y, int16_t w, int16_t h, surf_color c)
{
    surf_node *n = node_alloc(SURF_NODE_RECT);
    if (n) {
        n->x = x; n->y = y; n->w = w; n->h = h;
        n->u.rect.color = c;
    }
    return n;
}

surf_node *surf_sprite_new(const surf_image *img, int16_t x, int16_t y)
{
    if (!img)
        return NULL;
    surf_node *n = node_alloc(SURF_NODE_SPRITE);
    if (n) {
        n->x = x; n->y = y; n->w = img->w; n->h = img->h;
        n->u.sprite.img = img;
        n->u.sprite.src = (surf_rect){0, 0, img->w, img->h};
    }
    return n;
}

/* ---- tree ---- */

void surf_node_add(surf_node *parent, surf_node *child)
{
    if (!parent || !child || parent->type != SURF_NODE_GROUP || child->parent)
        return;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    child->parent = parent;
    surf_damage_subtree(child);
}

void surf_node_detach(surf_node *child)
{
    if (!child || !child->parent)
        return;
    surf_damage_subtree(child);
    surf_node *p = child->parent;
    if (child->prev) child->prev->next = child->next; else p->first = child->next;
    if (child->next) child->next->prev = child->prev; else p->last = child->prev;
    child->parent = NULL;
    child->prev = child->next = NULL;
}

static void destroy_children(surf_node *n)
{
    surf_node *c = n->first;
    while (c) {
        surf_node *next = c->next;
        destroy_children(c);
        node_free(c);
        c = next;
    }
    n->first = n->last = NULL;
}

void surf_node_destroy(surf_node *n)
{
    if (!n || n == surf_g.root)
        return;
    surf_node_detach(n);
    destroy_children(n);
    node_free(n);
}

/* ---- properties: damage old rect, mutate, damage new rect ---- */

void surf_node_set_pos(surf_node *n, int16_t x, int16_t y)
{
    if (!n || (n->x == x && n->y == y))
        return;
    surf_damage_subtree(n);
    n->x = x;
    n->y = y;
    surf_damage_subtree(n);
}

void surf_node_set_hidden(surf_node *n, bool hidden)
{
    if (!n || hidden == !!(n->flags & SURF_NF_HIDDEN))
        return;
    surf_damage_subtree(n);
    if (hidden) n->flags |= SURF_NF_HIDDEN; else n->flags &= (uint8_t)~SURF_NF_HIDDEN;
    surf_damage_subtree(n);
}

void surf_rect_set_color(surf_node *n, surf_color c)
{
    if (!n || n->type != SURF_NODE_RECT || n->u.rect.color == c)
        return;
    n->u.rect.color = c;
    surf_damage_subtree(n);
}

void surf_rect_set_size(surf_node *n, int16_t w, int16_t h)
{
    if (!n || n->type != SURF_NODE_RECT || (n->w == w && n->h == h))
        return;
    surf_damage_subtree(n);
    n->w = w;
    n->h = h;
    surf_damage_subtree(n);
}

void surf_sprite_set_src(surf_node *n, surf_rect src)
{
    if (!n || n->type != SURF_NODE_SPRITE)
        return;
    surf_damage_subtree(n);
    n->u.sprite.src = src;
    n->w = src.w;
    n->h = src.h;
    surf_damage_subtree(n);
}

void surf_group_set_clip(surf_node *g, int16_t w, int16_t h)
{
    if (!g || g->type != SURF_NODE_GROUP)
        return;
    surf_damage_subtree(g);
    g->w = w;
    g->h = h;
    if (w > 0 && h > 0) g->flags |= SURF_NF_CLIP; else g->flags &= (uint8_t)~SURF_NF_CLIP;
    surf_damage_subtree(g);
}
