/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set sw=4 sts=4 ts=4 expandtab: */
/*
   rsvg-text.c: Text handling routines for RSVG

   Copyright (C) 2000 Eazel, Inc.
   Copyright (C) 2002 Dom Lachowicz <cinamod@hotmail.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Raph Levien <raph@artofcode.com>
*/

#include <string.h>

#include "rsvg-private.h"
#include "rsvg-styles.h"
#include "rsvg-text.h"
#include "rsvg-css.h"
#include "rsvg-cairo-render.h"

#include "rsvg-shapes.h"

/* what we use for text rendering depends on what cairo has to offer */
#include <pango/pangocairo.h>

typedef struct _RsvgNodeText RsvgNodeText;

struct _RsvgNodeText {
    RsvgNode super;
    RsvgLength x, y, dx, dy;
};

typedef struct _RsvgNodeTref RsvgNodeTref;

struct _RsvgNodeTref {
    RsvgNode super;
    RsvgNode *link;
};

typedef struct _RsvgNodeTextPath RsvgNodeTextPath;

struct _RsvgNodeTextPath {
    RsvgNode super;
    RsvgNode *link;
    RsvgLength startOffset;
};

char *
rsvg_make_valid_utf8 (const char *str, int len)
{
    GString *string;
    const char *remainder, *invalid;
    int remaining_bytes, valid_bytes;

    string = NULL;
    remainder = str;

    if (len < 0)
        remaining_bytes = strlen (str);
    else
        remaining_bytes = len;

    while (remaining_bytes != 0) {
        if (g_utf8_validate (remainder, remaining_bytes, &invalid))
            break;
        valid_bytes = invalid - remainder;

        if (string == NULL)
            string = g_string_sized_new (remaining_bytes);

        g_string_append_len (string, remainder, valid_bytes);
        g_string_append_c (string, '?');

        remaining_bytes -= valid_bytes + 1;
        remainder = invalid + 1;
    }

    if (string == NULL)
        return len < 0 ? g_strndup (str, len) : g_strdup (str);

    g_string_append (string, remainder);

    return g_string_free (string, FALSE);
}

static GString *
_rsvg_text_chomp (RsvgState *state, GString * in, gboolean * lastwasspace)
{
    GString *out;
    guint i;
    out = g_string_new (in->str);

    if (!state->space_preserve) {
        for (i = 0; i < out->len;) {
            if (out->str[i] == '\n')
                g_string_erase (out, i, 1);
            else
                i++;
        }

        for (i = 0; i < out->len; i++)
            if (out->str[i] == '\t')
                out->str[i] = ' ';

        for (i = 0; i < out->len;) {
            if (out->str[i] == ' ' && *lastwasspace)
                g_string_erase (out, i, 1);
            else {
                if (out->str[i] == ' ')
                    *lastwasspace = TRUE;
                else
                    *lastwasspace = FALSE;
                i++;
            }
        }
    }

    return out;
}


static void
_rsvg_node_text_set_atts (RsvgNode * self, RsvgHandle * ctx, RsvgPropertyBag * atts)
{
    const char *klazz = NULL, *id = NULL, *value;
    RsvgNodeText *text = (RsvgNodeText *) self;

    if (rsvg_property_bag_size (atts)) {
        if ((value = rsvg_property_bag_lookup (atts, "x")))
            text->x = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "y")))
            text->y = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "dx")))
            text->dx = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "dy")))
            text->dy = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "class")))
            klazz = value;
        if ((value = rsvg_property_bag_lookup (atts, "id"))) {
            id = value;
            rsvg_defs_register_name (ctx->priv->defs, value, self);
        }

        rsvg_parse_style_attrs (ctx, self->state, "text", klazz, id, atts);
    }
}

static void
 rsvg_text_render_text (RsvgDrawingCtx * ctx, const char *text, gdouble * x, gdouble * y);


static void
 _rsvg_node_text_type_tspan (RsvgNodeText * self, RsvgDrawingCtx * ctx,
                             gdouble * x, gdouble * y, gboolean * lastwasspace,
                             gboolean usetextonly);

static void
 _rsvg_node_text_type_tref (RsvgNodeTref * self, RsvgDrawingCtx * ctx,
                            gdouble * x, gdouble * y, gboolean * lastwasspace,
                            gboolean usetextonly);

static void
_rsvg_node_text_type_text_path (RsvgNodeTextPath * self, RsvgDrawingCtx * ctx,
                           gdouble * x, gdouble * y, gboolean * lastwasspace,
                           gboolean usetextonly);

static void
_rsvg_node_text_type_children (RsvgNode * self, RsvgDrawingCtx * ctx,
                               gdouble * x, gdouble * y, gboolean * lastwasspace,
                               gboolean usetextonly)
{
    guint i;

    rsvg_push_discrete_layer (ctx);
    for (i = 0; i < self->children->len; i++) {
        RsvgNode *node = g_ptr_array_index (self->children, i);
        RsvgNodeType type = RSVG_NODE_TYPE (node);

        if (type == RSVG_NODE_TYPE_CHARS) {
            RsvgNodeChars *chars = (RsvgNodeChars *) node;
            GString *str = _rsvg_text_chomp (rsvg_current_state (ctx), chars->contents, lastwasspace);
            rsvg_text_render_text (ctx, str->str, x, y);
            g_string_free (str, TRUE);
        } else {
            if (usetextonly) {
                _rsvg_node_text_type_children (node, ctx, x, y, lastwasspace,
                                               usetextonly);
            } else {
                if (type == RSVG_NODE_TYPE_TSPAN) {
                    RsvgNodeText *tspan = (RsvgNodeText *) node;
                    rsvg_state_push (ctx);
                    _rsvg_node_text_type_tspan (tspan, ctx, x, y, lastwasspace,
                                                usetextonly);
                    rsvg_state_pop (ctx);
                } else if (type == RSVG_NODE_TYPE_TREF) {
                    RsvgNodeTref *tref = (RsvgNodeTref *) node;
                    _rsvg_node_text_type_tref (tref, ctx, x, y, lastwasspace,
                                               usetextonly);
                } else if (type == RSVG_NODE_TYPE_TEXT_PATH) {
                    RsvgNodeTextPath *textpath = (RsvgNodeTextPath *) node;
                    _rsvg_node_text_type_text_path(textpath, ctx, x, y,
                                                   lastwasspace, usetextonly);
                }
            }
        }
    }
    rsvg_pop_discrete_layer (ctx);
}

static int
 _rsvg_node_text_length_tref (RsvgNodeTref * self, RsvgDrawingCtx * ctx,
                              gdouble * x, gboolean * lastwasspace,
                              gboolean usetextonly);

static int
 _rsvg_node_text_length_tspan (RsvgNodeText * self, RsvgDrawingCtx * ctx,
                               gdouble * x, gboolean * lastwasspace,
                               gboolean usetextonly);

static gdouble rsvg_text_length_text_as_string (RsvgDrawingCtx * ctx, const char *text);

static int
_rsvg_node_text_length_children (RsvgNode * self, RsvgDrawingCtx * ctx,
                                 gdouble * length, gboolean * lastwasspace,
                                 gboolean usetextonly)
{
    guint i;
    int out = FALSE;
    for (i = 0; i < self->children->len; i++) {
        RsvgNode *node = g_ptr_array_index (self->children, i);
        RsvgNodeType type = RSVG_NODE_TYPE (node);

        rsvg_state_push (ctx);
        rsvg_state_reinherit_top (ctx, node->state, 0);
        if (type == RSVG_NODE_TYPE_CHARS) {
            RsvgNodeChars *chars = (RsvgNodeChars *) node;
            GString *str = _rsvg_text_chomp (rsvg_current_state (ctx), chars->contents, lastwasspace);
            *length += rsvg_text_length_text_as_string (ctx, str->str);
            g_string_free (str, TRUE);
        } else {
            if (usetextonly || type == RSVG_NODE_TYPE_TEXT_PATH) {
                out = _rsvg_node_text_length_children(node, ctx, length,
                                                      lastwasspace,
                                                      usetextonly);
            } else {
                if (type == RSVG_NODE_TYPE_TSPAN) {
                    RsvgNodeText *tspan = (RsvgNodeText *) node;
                    out = _rsvg_node_text_length_tspan (tspan, ctx, length,
                                                        lastwasspace,
                                                        usetextonly);
                } else if (type == RSVG_NODE_TYPE_TREF) {
                    RsvgNodeTref *tref = (RsvgNodeTref *) node;
                    out = _rsvg_node_text_length_tref (tref, ctx, length,
                                                       lastwasspace,
                                                       usetextonly);
                }
            }
        }
        rsvg_state_pop (ctx);
        if (out)
            break;
    }
    return out;
}


static void
_rsvg_node_text_draw (RsvgNode * self, RsvgDrawingCtx * ctx, int dominate)
{
    double x, y, dx, dy, length = 0;
    gboolean lastwasspace = TRUE;
    RsvgNodeText *text = (RsvgNodeText *) self;
    rsvg_state_reinherit_top (ctx, self->state, dominate);

    x = _rsvg_css_normalize_length (&text->x, ctx, 'h');
    y = _rsvg_css_normalize_length (&text->y, ctx, 'v');
    dx = _rsvg_css_normalize_length (&text->dx, ctx, 'h');
    dy = _rsvg_css_normalize_length (&text->dy, ctx, 'v');

    if (rsvg_current_state (ctx)->text_anchor != TEXT_ANCHOR_START) {
        _rsvg_node_text_length_children (self, ctx, &length, &lastwasspace, FALSE);
        if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
            length /= 2;
    }
    if (PANGO_GRAVITY_IS_VERTICAL (rsvg_current_state (ctx)->text_gravity)) {
        y -= length;
        if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
            dy /= 2;
        if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_END)
            dy = 0;
    } else {
        x -= length;
        if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
            dx /= 2;
        if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_END)
            dx = 0;
    }
    x += dx;
    y += dy;

    lastwasspace = TRUE;
    _rsvg_node_text_type_children (self, ctx, &x, &y, &lastwasspace, FALSE);
}

RsvgNode *
rsvg_new_text (void)
{
    RsvgNodeText *text;
    text = g_new (RsvgNodeText, 1);
    _rsvg_node_init (&text->super, RSVG_NODE_TYPE_TEXT);
    text->super.draw = _rsvg_node_text_draw;
    text->super.set_atts = _rsvg_node_text_set_atts;
    text->x = text->y = text->dx = text->dy = _rsvg_css_parse_length ("0");
    return &text->super;
}

static void
_rsvg_node_text_type_tspan (RsvgNodeText * self, RsvgDrawingCtx * ctx,
                            gdouble * x, gdouble * y, gboolean * lastwasspace,
                            gboolean usetextonly)
{
    double dx, dy, length = 0;
    rsvg_state_reinherit_top (ctx, self->super.state, 0);

    dx = _rsvg_css_normalize_length (&self->dx, ctx, 'h');
    dy = _rsvg_css_normalize_length (&self->dy, ctx, 'v');

    if (rsvg_current_state (ctx)->text_anchor != TEXT_ANCHOR_START) {
        gboolean lws = *lastwasspace;
        _rsvg_node_text_length_children (&self->super, ctx, &length, &lws,
                                         usetextonly);
        if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
            length /= 2;
    }

    if (self->x.factor != 'n') {
        *x = _rsvg_css_normalize_length (&self->x, ctx, 'h');
        if (!PANGO_GRAVITY_IS_VERTICAL (rsvg_current_state (ctx)->text_gravity)) {
            *x -= length;
            if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
                dx /= 2;
            if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_END)
                dx = 0;
        }
    }
    *x += dx;

    if (self->y.factor != 'n') {
        *y = _rsvg_css_normalize_length (&self->y, ctx, 'v');
        if (PANGO_GRAVITY_IS_VERTICAL (rsvg_current_state (ctx)->text_gravity)) {
            *y -= length;
            if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
                dy /= 2;
            if (rsvg_current_state (ctx)->text_anchor == TEXT_ANCHOR_END)
                dy = 0;
        }
    }
    *y += dy;
    _rsvg_node_text_type_children (&self->super, ctx, x, y, lastwasspace,
                                   usetextonly);
}

static int
_rsvg_node_text_length_tspan (RsvgNodeText * self,
                              RsvgDrawingCtx * ctx, gdouble * length,
                              gboolean * lastwasspace, gboolean usetextonly)
{
    if (self->x.factor != 'n' || self->y.factor != 'n')
        return TRUE;

    if (PANGO_GRAVITY_IS_VERTICAL (rsvg_current_state (ctx)->text_gravity))
        *length += _rsvg_css_normalize_length (&self->dy, ctx, 'v');
    else
        *length += _rsvg_css_normalize_length (&self->dx, ctx, 'h');

    return _rsvg_node_text_length_children (&self->super, ctx, length,
                                             lastwasspace, usetextonly);
}

static void
_rsvg_node_tspan_set_atts (RsvgNode * self, RsvgHandle * ctx, RsvgPropertyBag * atts)
{
    const char *klazz = NULL, *id = NULL, *value;
    RsvgNodeText *text = (RsvgNodeText *) self;

    if (rsvg_property_bag_size (atts)) {
        if ((value = rsvg_property_bag_lookup (atts, "x")))
            text->x = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "y")))
            text->y = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "dx")))
            text->dx = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "dy")))
            text->dy = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "class")))
            klazz = value;
        if ((value = rsvg_property_bag_lookup (atts, "id"))) {
            id = value;
            rsvg_defs_register_name (ctx->priv->defs, value, self);
        }

        rsvg_parse_style_attrs (ctx, self->state, "tspan", klazz, id, atts);
    }
}

RsvgNode *
rsvg_new_tspan (void)
{
    RsvgNodeText *text;
    text = g_new (RsvgNodeText, 1);
    _rsvg_node_init (&text->super, RSVG_NODE_TYPE_TSPAN);
    text->super.set_atts = _rsvg_node_tspan_set_atts;
    text->x.factor = text->y.factor = 'n';
    text->dx = text->dy = _rsvg_css_parse_length ("0");
    return &text->super;
}

static void
_rsvg_node_text_type_tref (RsvgNodeTref * self, RsvgDrawingCtx * ctx,
                           gdouble * x, gdouble * y, gboolean * lastwasspace,
                           gboolean usetextonly)
{
    if (self->link)
        _rsvg_node_text_type_children (self->link, ctx, x, y, lastwasspace,
                                                              TRUE);
}

static int
_rsvg_node_text_length_tref (RsvgNodeTref * self, RsvgDrawingCtx * ctx, gdouble * x,
                             gboolean * lastwasspace, gboolean usetextonly)
{
    if (self->link)
        return _rsvg_node_text_length_children (self->link, ctx, x, lastwasspace, TRUE);
    return FALSE;
}

static void
_rsvg_node_tref_set_atts (RsvgNode * self, RsvgHandle * ctx, RsvgPropertyBag * atts)
{
    const char *value;
    RsvgNodeTref *text = (RsvgNodeTref *) self;

    if (rsvg_property_bag_size (atts)) {
        if ((value = rsvg_property_bag_lookup (atts, "xlink:href")))
            rsvg_defs_add_resolver (ctx->priv->defs, &text->link, value);
        if ((value = rsvg_property_bag_lookup (atts, "id")))
            rsvg_defs_register_name (ctx->priv->defs, value, self);
    }
}

RsvgNode *
rsvg_new_tref (void)
{
    RsvgNodeTref *text;
    text = g_new (RsvgNodeTref, 1);
    _rsvg_node_init (&text->super, RSVG_NODE_TYPE_TREF);
    text->super.set_atts = _rsvg_node_tref_set_atts;
    text->link = NULL;
    return &text->super;
}

static void
_rsvg_node_text_path_set_atts (RsvgNode * self, RsvgHandle * ctx, RsvgPropertyBag * atts)
{
    const char *klazz = NULL, *id = NULL, *value;
    RsvgNodeTextPath *text = (RsvgNodeTextPath *) self;

    if (rsvg_property_bag_size (atts)) {
        // TODO add support for method, spacing?
        if ((value = rsvg_property_bag_lookup (atts, "xlink:href")))
            rsvg_defs_add_resolver (ctx->priv->defs, &text->link, value);
        if ((value = rsvg_property_bag_lookup (atts, "startOffset")))
            text->startOffset = _rsvg_css_parse_length (value);
        if ((value = rsvg_property_bag_lookup (atts, "class")))
            klazz = value;
        if ((value = rsvg_property_bag_lookup (atts, "id")))
            rsvg_defs_register_name (ctx->priv->defs, value, self);

        rsvg_parse_style_attrs (ctx, self->state, "textPath", klazz, id, atts);
    }
}

RsvgNode *
rsvg_new_text_path (void)
{
    RsvgNodeTextPath *text;
    text = g_new (RsvgNodeTextPath, 1);
    _rsvg_node_init (&text->super, RSVG_NODE_TYPE_TEXT_PATH);
    text->super.set_atts = _rsvg_node_text_path_set_atts;
    text->link = NULL;
    return &text->super;
}

typedef struct _RsvgTextLayout RsvgTextLayout;

struct _RsvgTextLayout {
    PangoLayout *layout;
    RsvgDrawingCtx *ctx;
    TextAnchor anchor;
    gdouble x, y;
};

static void
rsvg_text_layout_free (RsvgTextLayout * layout)
{
    g_object_unref (layout->layout);
    g_free (layout);
}

static PangoLayout *
rsvg_text_create_layout (RsvgDrawingCtx * ctx,
                         RsvgState * state, const char *text, PangoContext * context)
{
    PangoFontDescription *font_desc;
    PangoLayout *layout;
    PangoAttrList *attr_list;
    PangoAttribute *attribute;

    if (state->lang)
        pango_context_set_language (context, pango_language_from_string (state->lang));

    if (state->unicode_bidi == UNICODE_BIDI_OVERRIDE || state->unicode_bidi == UNICODE_BIDI_EMBED)
        pango_context_set_base_dir (context, state->text_dir);

    if (PANGO_GRAVITY_IS_VERTICAL (state->text_gravity))
        pango_context_set_base_gravity (context, state->text_gravity);

    font_desc = pango_font_description_copy (pango_context_get_font_description (context));

    if (state->font_family)
        pango_font_description_set_family_static (font_desc, state->font_family);

    pango_font_description_set_style (font_desc, state->font_style);
    pango_font_description_set_variant (font_desc, state->font_variant);
    pango_font_description_set_weight (font_desc, state->font_weight);
    pango_font_description_set_stretch (font_desc, state->font_stretch);
    pango_font_description_set_size (font_desc,
                                     _rsvg_css_normalize_font_size (state, ctx) *
                                     PANGO_SCALE / ctx->dpi_y * 72);

    layout = pango_layout_new (context);
    pango_layout_set_font_description (layout, font_desc);
    pango_font_description_free (font_desc);

    attr_list = pango_attr_list_new ();
    attribute = pango_attr_letter_spacing_new (_rsvg_css_normalize_length (&state->letter_spacing,
                                                                           ctx, 'h') * PANGO_SCALE);
    attribute->start_index = 0;
    attribute->end_index = G_MAXINT;
    pango_attr_list_insert (attr_list, attribute); 

    if (state->has_font_decor && text) {
        if (state->font_decor & TEXT_UNDERLINE) {
            attribute = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
            attribute->start_index = 0;
            attribute->end_index = -1;
            pango_attr_list_insert (attr_list, attribute);
        }
	if (state->font_decor & TEXT_STRIKE) {
            attribute = pango_attr_strikethrough_new (TRUE);
            attribute->start_index = 0;
            attribute->end_index = -1;
            pango_attr_list_insert (attr_list, attribute);
	}
    }

    pango_layout_set_attributes (layout, attr_list);
    pango_attr_list_unref (attr_list);

    if (text)
        pango_layout_set_text (layout, text, -1);
    else
        pango_layout_set_text (layout, NULL, 0);

    pango_layout_set_alignment (layout, (state->text_dir == PANGO_DIRECTION_LTR) ?
                                PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);

    return layout;
}


static RsvgTextLayout *
rsvg_text_layout_new (RsvgDrawingCtx * ctx, RsvgState * state, const char *text)
{
    RsvgTextLayout *layout;

    if (ctx->pango_context == NULL)
        ctx->pango_context = ctx->render->create_pango_context (ctx);

    layout = g_new0 (RsvgTextLayout, 1);

    layout->layout = rsvg_text_create_layout (ctx, state, text, ctx->pango_context);
    layout->ctx = ctx;

    layout->anchor = state->text_anchor;

    return layout;
}

void
rsvg_text_render_text (RsvgDrawingCtx * ctx, const char *text, gdouble * x, gdouble * y)
{
    PangoContext *context;
    PangoLayout *layout;
    PangoLayoutIter *iter;
    RsvgState *state;
    gint w, h, offsetX, offsetY;

    state = rsvg_current_state (ctx);

    /* Do not render the text if the font size is zero. See bug #581491. */
    if (state->font_size.length == 0)
        return;

    context = ctx->render->create_pango_context (ctx);
    layout = rsvg_text_create_layout (ctx, state, text, context);
    pango_layout_get_size (layout, &w, &h);
    iter = pango_layout_get_iter (layout);
    if (PANGO_GRAVITY_IS_VERTICAL (state->text_gravity)) {
        offsetX = -pango_layout_iter_get_baseline (iter) / (double)PANGO_SCALE;
        offsetY = 0;
    } else {
        offsetX = 0;
        offsetY = pango_layout_iter_get_baseline (iter) / (double)PANGO_SCALE;
    }
    pango_layout_iter_free (iter);
    ctx->render->render_pango_layout (ctx, layout, *x - offsetX, *y - offsetY);
    if (PANGO_GRAVITY_IS_VERTICAL (state->text_gravity))
        *y += w / (double)PANGO_SCALE;
    else
        *x += w / (double)PANGO_SCALE;

    g_object_unref (layout);
    g_object_unref (context);
}

static gdouble
rsvg_text_layout_width (RsvgTextLayout * layout)
{
    gint width;

    pango_layout_get_size (layout->layout, &width, NULL);

    return width / (double)PANGO_SCALE;
}

static gdouble
rsvg_text_length_text_as_string (RsvgDrawingCtx * ctx, const char *text)
{
    RsvgTextLayout *layout;
    gdouble x;

    layout = rsvg_text_layout_new (ctx, rsvg_current_state (ctx), text);
    layout->x = layout->y = 0;

    x = rsvg_text_layout_width (layout);

    rsvg_text_layout_free (layout);
    return x;
}


/* Returns Euclidean distance between two points */
static double
two_points_distance (cairo_path_data_t *a, cairo_path_data_t *b)
{
    double dx, dy;

    dx = b->point.x - a->point.x;
    dy = b->point.y - a->point.y;

    return sqrt (dx * dx + dy * dy);
}

/* Returns length of a Bezier curve.
 * Seems like computing that analytically is not easy.  The
 * code just flattens the curve using cairo and adds the length
 * of segments.
 */
static double
curve_length (double x0, double y0,
              double x1, double y1,
              double x2, double y2,
              double x3, double y3)
{
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_path_t *path;
    cairo_path_data_t *data, current_point;
    int i;
    double length;

    surface = cairo_image_surface_create (CAIRO_FORMAT_A8, 0, 0);
    cr = cairo_create (surface);
    cairo_surface_destroy (surface);

    cairo_move_to (cr, x0, y0);
    cairo_curve_to (cr, x1, y1, x2, y2, x3, y3);

    length = 0;
    path = cairo_copy_path_flat (cr);
    for (i=0; i < path->num_data; i += path->data[i].header.length) {
        data = &path->data[i];
        switch (data->header.type) {

        case CAIRO_PATH_MOVE_TO:
            current_point = data[1];
            break;

        case CAIRO_PATH_LINE_TO:
            length += two_points_distance (&current_point, &data[1]);
            current_point = data[1];
            break;

        default:
        case CAIRO_PATH_CURVE_TO:
        case CAIRO_PATH_CLOSE_PATH:
            g_assert_not_reached ();
        }
    }
    cairo_path_destroy (path);

    cairo_destroy (cr);

    return length;
}


typedef double parametrization_t;

/* Compute parametrization info.  That is, for each part of the 
 * cairo path, tags it with its length.
 *
 * Free returned value with g_free().
 */
static parametrization_t *
parametrize_path (cairo_path_t *path)
{
    int i;
    cairo_path_data_t *data, last_move_to, current_point;
    parametrization_t *parametrization;

    parametrization = g_malloc (path->num_data * sizeof (parametrization[0]));

    for (i=0; i < path->num_data; i += path->data[i].header.length) {
        data = &path->data[i];
        parametrization[i] = 0.0;
        switch (data->header.type) {
        case CAIRO_PATH_MOVE_TO:
            last_move_to = data[1];
            current_point = data[1];
            break;
        case CAIRO_PATH_CLOSE_PATH:
            /* Make it look like it's a line_to to last_move_to */
            data = (&last_move_to) - 1;
            /* fall through */
        case CAIRO_PATH_LINE_TO:
            parametrization[i] = two_points_distance (&current_point, &data[1]);
            current_point = data[1];
            break;
        case CAIRO_PATH_CURVE_TO:
            /* naive curve-length, treating bezier as three line segments:
               parametrization[i] = two_points_distance (&current_point, &data[1])
               + two_points_distance (&data[1], &data[2])
               + two_points_distance (&data[2], &data[3]);
            */
            parametrization[i] = curve_length (current_point.point.x, current_point.point.x,
                                               data[1].point.x, data[1].point.y,
                                               data[2].point.x, data[2].point.y,
                                               data[3].point.x, data[3].point.y);

            current_point = data[3];
            break;
        default:
            g_assert_not_reached ();
        }
    }

    return parametrization;
}


typedef void (*transform_point_func_t) (void *closure, double *x, double *y);

/* Project a path using a function.  Each point of the path (including
 * Bezier control points) is passed to the function for transformation.
 */
static void
transform_path (cairo_path_t *path, transform_point_func_t f, void *closure)
{
    int i;
    cairo_path_data_t *data;

    for (i=0; i < path->num_data; i += path->data[i].header.length) {
        data = &path->data[i];
        switch (data->header.type) {
        case CAIRO_PATH_CURVE_TO:
            f (closure, &data[3].point.x, &data[3].point.y);
            f (closure, &data[2].point.x, &data[2].point.y);
        case CAIRO_PATH_MOVE_TO:
        case CAIRO_PATH_LINE_TO:
            f (closure, &data[1].point.x, &data[1].point.y);
            break;
        case CAIRO_PATH_CLOSE_PATH:
            break;
        default:
            g_assert_not_reached ();
        }
    }
}


/* Simple struct to hold a path and its parametrization */
typedef struct {
    cairo_path_t *path;
    parametrization_t *parametrization;
} parametrized_path_t;


/* Project a point X,Y onto a parameterized path.  The final point is
 * where you get if you walk on the path forward from the beginning for X
 * units, then stop there and walk another Y units perpendicular to the
 * path at that point.  In more detail:
 *
 * There's three pieces of math involved:
 *
 *   - The parametric form of the Line equation
 *     http://en.wikipedia.org/wiki/Line
 *
 *   - The parametric form of the Cubic Bézier curve equation
 *     http://en.wikipedia.org/wiki/B%C3%A9zier_curve
 *
 *   - The Gradient (aka multi-dimensional derivative) of the above
 *     http://en.wikipedia.org/wiki/Gradient
 *
 * The parametric forms are used to answer the question of "where will I be
 * if I walk a distance of X on this path".  The Gradient is used to answer
 * the question of "where will I be if then I stop, rotate left for 90
 * degrees and walk straight for a distance of Y".
 */
static void
point_on_path (parametrized_path_t *param,
                   double *x, double *y)
{
    int i;
    double ratio, the_y = *y, the_x = *x, dx, dy;
    cairo_path_data_t *data, last_move_to, current_point;
    cairo_path_t *path = param->path;
    parametrization_t *parametrization = param->parametrization;

    for (i=0; i + path->data[i].header.length < path->num_data &&
             (the_x > parametrization[i] ||
              path->data[i].header.type == CAIRO_PATH_MOVE_TO);
         i += path->data[i].header.length) {
        the_x -= parametrization[i];
        data = &path->data[i];
        switch (data->header.type) {
        case CAIRO_PATH_MOVE_TO:
            current_point = data[1];
            last_move_to = data[1];
            break;
        case CAIRO_PATH_LINE_TO:
            current_point = data[1];
            break;
        case CAIRO_PATH_CURVE_TO:
            current_point = data[3];
            break;
        case CAIRO_PATH_CLOSE_PATH:
            break;
        default:
            g_assert_not_reached ();
        }
    }
    data = &path->data[i];

    switch (data->header.type) {

    case CAIRO_PATH_MOVE_TO:
        break;
    case CAIRO_PATH_CLOSE_PATH:
        /* Make it look like it's a line_to to last_move_to */
        data = (&last_move_to) - 1;
        /* fall through */
    case CAIRO_PATH_LINE_TO:
        {
            ratio = the_x / parametrization[i];
            /* Line polynomial */
            *x = current_point.point.x * (1 - ratio) + data[1].point.x * ratio;
            *y = current_point.point.y * (1 - ratio) + data[1].point.y * ratio;

            /* Line gradient */
            dx = -(current_point.point.x - data[1].point.x);
            dy = -(current_point.point.y - data[1].point.y);

            /*optimization for: ratio = the_y / sqrt (dx * dx + dy * dy);*/
            ratio = the_y / parametrization[i];
            *x += -dy * ratio;
            *y +=  dx * ratio;
        }
        break;
    case CAIRO_PATH_CURVE_TO:
        {
            /* FIXME the formulas here are not exactly what we want, because the
             * Bezier parametrization is not uniform.  But I don't know how to do
             * better.  The caller can do slightly better though, by flattening the
             * Bezier and avoiding this branch completely.  That has its own cost
             * though, as large y values magnify the flattening error drastically.
             */

            double ratio_1_0, ratio_0_1;
            double ratio_2_0, ratio_0_2;
            double ratio_3_0, ratio_2_1, ratio_1_2, ratio_0_3;
            double _1__4ratio_1_0_3ratio_2_0, _2ratio_1_0_3ratio_2_0;

            ratio = the_x / parametrization[i];

            ratio_1_0 = ratio;
            ratio_0_1 = 1 - ratio;

            ratio_2_0 = ratio_1_0 * ratio_1_0; /*      ratio  *      ratio  */
            ratio_0_2 = ratio_0_1 * ratio_0_1; /* (1 - ratio) * (1 - ratio) */

            ratio_3_0 = ratio_2_0 * ratio_1_0; /*      ratio  *      ratio  *      ratio  */
            ratio_2_1 = ratio_2_0 * ratio_0_1; /*      ratio  *      ratio  * (1 - ratio) */
            ratio_1_2 = ratio_1_0 * ratio_0_2; /*      ratio  * (1 - ratio) * (1 - ratio) */
            ratio_0_3 = ratio_0_1 * ratio_0_2; /* (1 - ratio) * (1 - ratio) * (1 - ratio) */

            _1__4ratio_1_0_3ratio_2_0 = 1 - 4 * ratio_1_0 + 3 * ratio_2_0;
            _2ratio_1_0_3ratio_2_0    =     2 * ratio_1_0 - 3 * ratio_2_0;

            /* Bezier polynomial */
            *x = current_point.point.x * ratio_0_3
                + 3 *   data[1].point.x * ratio_1_2
                + 3 *   data[2].point.x * ratio_2_1
                +       data[3].point.x * ratio_3_0;
            *y = current_point.point.y * ratio_0_3
                + 3 *   data[1].point.y * ratio_1_2
                + 3 *   data[2].point.y * ratio_2_1
                +       data[3].point.y * ratio_3_0;

            /* Bezier gradient */
            dx =-3 * current_point.point.x * ratio_0_2
                + 3 *       data[1].point.x * _1__4ratio_1_0_3ratio_2_0
                + 3 *       data[2].point.x * _2ratio_1_0_3ratio_2_0
                + 3 *       data[3].point.x * ratio_2_0;
            dy =-3 * current_point.point.y * ratio_0_2
                + 3 *       data[1].point.y * _1__4ratio_1_0_3ratio_2_0
                + 3 *       data[2].point.y * _2ratio_1_0_3ratio_2_0
                + 3 *       data[3].point.y * ratio_2_0;

            ratio = the_y / sqrt (dx * dx + dy * dy);
            *x += -dy * ratio;
            *y +=  dx * ratio;
        }
        break;
    default:
        g_assert_not_reached ();
    }
}

/* Projects the current path of cr onto the provided path. */
static void
map_path_onto (cairo_t *cr, cairo_path_t *path)
{
    cairo_path_t *current_path;
    parametrized_path_t param;

    param.path = path;
    param.parametrization = parametrize_path (path);

    current_path = cairo_copy_path (cr);
    cairo_new_path (cr);

    transform_path (current_path,
                    (transform_point_func_t) point_on_path, &param);

    cairo_append_path (cr, current_path);

    cairo_path_destroy (current_path);
    g_free (param.parametrization);
}

static void
draw_text (RsvgDrawingCtx * ctx, cairo_t *cr, double *x, double *y,
           const char *text)
{
    PangoContext *context;
    PangoLayout *layout;
    RsvgState *state;
    PangoLayoutLine *line;
    int w, h;

    state = rsvg_current_state (ctx);
    if (state->font_size.length == 0)
        return;

    context = ctx->render->create_pango_context (ctx);
    layout = rsvg_text_create_layout (ctx, state, text, context);
    pango_layout_get_size(layout, &w, &h);

    // From pango example
    // http://git.gnome.org/browse/pango/tree/examples/cairotwisted.c
    line = pango_layout_get_line_readonly (layout, 0);

    cairo_move_to (cr, *x, *y);
    pango_cairo_layout_line_path (cr, line);

    // Update x, y based on dimensions of text drawn
    if (PANGO_GRAVITY_IS_VERTICAL (state->text_gravity))
        *y += w / (double)PANGO_SCALE;
    else
        *x += w / (double)PANGO_SCALE;

    g_object_unref (layout);
    g_object_unref (context);
}

static void
draw_twisted (RsvgDrawingCtx * ctx, cairo_t *cr, cairo_path_t *path,
              double *x, double *y, const char *text)
{

    cairo_path_t *path_copy;

    cairo_save (cr);

    /* Decrease tolerance a bit, since it's going to be magnified */
    cairo_set_tolerance (cr, 0.01);

    /* Using cairo_copy_path() here shows our deficiency in handling
     * Bezier curves, specially around sharper curves.
     *
     * Using cairo_copy_path_flat() on the other hand, magnifies the
     * flattening error with large off-path values.  We decreased
     * tolerance for that reason.  Increase tolerance to see that
     * artifact.
     */

    // Load the previously drawn path in order to make a copy
    cairo_new_path(cr);
    cairo_append_path(cr, path);
    path_copy = cairo_copy_path_flat (cr);

    // Draw the text
    cairo_new_path (cr);

    draw_text (ctx, cr, x, y, text);
    map_path_onto (cr, path_copy);

    cairo_path_destroy (path_copy);

    // Render the text path using current text settings
    //cairo_fill_preserve (cr);
    path_copy = cairo_copy_path(cr);
    ctx->render->render_path(ctx, path_copy);
    cairo_path_destroy (path_copy);

    cairo_restore (cr);
}

/* Returns the length of path. path must be flattened
 * (i.e. only consist of line segments)
 */
static double _rsvg_path_length(cairo_path_t *path) {
    int i;
    double distance = 0.0;
    cairo_path_data_t *last_point, *cur_point, *last_move_to;

    for (i=0; i < path->num_data; i += path->data[i].header.length) {
        cairo_path_data_t *data = &path->data[i];
        switch (data->header.type) {

        case CAIRO_PATH_MOVE_TO:
            last_move_to = data;
            last_point = &data[1];
            break;
        case CAIRO_PATH_CLOSE_PATH:
            // Make it look like it's a line_to to last_move_to
            data = last_move_to;
            // fall through
        case CAIRO_PATH_LINE_TO:
            cur_point = &data[1];
            distance += two_points_distance(last_point, cur_point);
            last_point = cur_point;
            break;
        default:
            g_assert_not_reached ();
        }
    }

    return distance;
}

/* Returns the x, y, and tangent vector angle of a point distance
 * d down a path. path must be flattened.
 */
static void _rsvg_path_point_on_path(cairo_path_t *path, double d,
                                     double *x, double *y, double *theta)
{
    int i;
    double dist_so_far = 0.0;
    cairo_path_data_t *last_point, *cur_point, *last_move_to;

    for (i=0; i < path->num_data; i += path->data[i].header.length) {
        cairo_path_data_t *data = &path->data[i];
        switch (data->header.type) {

        case CAIRO_PATH_MOVE_TO:
            last_move_to = data;
            last_point = &data[1];
            break;
        case CAIRO_PATH_CLOSE_PATH:
            // Make it look like it's a line_to to last_move_to
            data = last_move_to;
            // fall through
        case CAIRO_PATH_LINE_TO:
            cur_point = &data[1];
            dist_so_far += two_points_distance(last_point, cur_point);

            // Reached distance d along the path
            if (dist_so_far > d) {
                double dlength = dist_so_far - d;
                double angle = atan2(cur_point->point.y - last_point->point.y,
                                     cur_point->point.x - last_point->point.x);
                *x = dlength * cos(angle) + last_point->point.x;
                *y = dlength * sin(angle) + last_point->point.y;
                // Find the normal vector and normalize to [-pi,pi]
                angle -= G_PI/2.0;
                if (angle < -G_PI)
                    angle += 2*G_PI;
                *theta = angle;
            } else {
                last_point = cur_point;                
            }
            break;
        default:
            g_assert_not_reached ();
        }
    }
}

/* TODO: renders each character individually */
static void _rsvg_node_text_path_type_children(RsvgNode * self,
                                               RsvgNodePath * path,
                         RsvgDrawingCtx * ctx, gdouble * x, gdouble * y,
                         gboolean * lastwasspace, gboolean usetextonly)
{
    int i;

    rsvg_push_discrete_layer (ctx);
    for (i = 0; i < self->children->len; i++) {
        RsvgNode *node = g_ptr_array_index (self->children, i);
        RsvgNodeType type = RSVG_NODE_TYPE (node);
        
        if (type == RSVG_NODE_TYPE_CHARS) {
            RsvgNodeChars *chars = (RsvgNodeChars *) node;
            GString *str = _rsvg_text_chomp (rsvg_current_state (ctx), chars->contents, lastwasspace);
            //draw_twisted (ctx, cr, path, x, y, str->str);
            g_string_free (str, TRUE);
        }
    }
    rsvg_pop_discrete_layer (ctx);    
}

static void
_rsvg_node_text_type_text_path (RsvgNodeTextPath * self, RsvgDrawingCtx * ctx,
                                gdouble * x, gdouble * y,
                                gboolean * lastwasspace, gboolean usetextonly)
{
    if (self->link && self->link->type == RSVG_NODE_TYPE_PATH) {
        guint i;

        // Resolve the xlink:href id and get its cairo path object
        cairo_path_t *path = ((RsvgNodePath *)self->link)->path;

        // Get the cairo_t context
        RsvgCairoRender *render = RSVG_CAIRO_RENDER (ctx->render);
        cairo_t *cr = render->cr;

        // Flatten path
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_append_path(cr, path);
        path = cairo_copy_path_flat (cr);
        cairo_restore(cr);

        // TODO recursively draw children
        rsvg_push_discrete_layer (ctx);
        for (i = 0; i < self->super.children->len; i++) {
            RsvgNode *node = g_ptr_array_index (self->super.children, i);
            RsvgNodeType type = RSVG_NODE_TYPE (node);

            if (type == RSVG_NODE_TYPE_CHARS) {
                RsvgNodeChars *chars = (RsvgNodeChars *) node;
                GString *str = _rsvg_text_chomp (rsvg_current_state (ctx), chars->contents, lastwasspace);
                double offset = 0;

                // Factor in start offset
                // Handle percentages as percent of path length
                if (self->startOffset.factor == 'p') {
                    offset = self->startOffset.length *_rsvg_path_length(path);
                } else {
                    offset = _rsvg_css_normalize_length (&self->startOffset,
                                                       ctx, 'h');
                }

                if (PANGO_GRAVITY_IS_VERTICAL (rsvg_current_state(ctx)->text_gravity))
                    *y += offset;
                else
                    *x += offset;

                draw_twisted (ctx, cr, path, x, y, str->str);
                g_string_free (str, TRUE);
            }
        }
        rsvg_pop_discrete_layer (ctx);

        cairo_path_destroy (path);
    }
}
