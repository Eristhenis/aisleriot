/*
 *  Copyright © 2008 Neil Roberts
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include <clutter/clutter-actor.h>
#include <clutter/clutter-container.h>
#include <clutter/clutter-timeline.h>
#include <clutter/clutter-behaviour-rotate.h>
#include <clutter/clutter-behaviour-depth.h>
#include <clutter/clutter-behaviour-path.h>
#include <gtk/gtk.h>
#include <cogl/cogl.h>
#include <string.h>

#include "slot-renderer.h"
#include "card.h"

static void aisleriot_slot_renderer_dispose (GObject *object);
static void aisleriot_slot_renderer_finalize (GObject *object);

static void aisleriot_slot_renderer_set_property (GObject *object,
                                                  guint property_id,
                                                  const GValue *value,
                                                  GParamSpec *pspec);
static void aisleriot_slot_renderer_get_property (GObject *object,
                                                  guint property_id,
                                                  GValue *value,
                                                  GParamSpec *pspec);

static void aisleriot_slot_renderer_paint (ClutterActor *actor);

static void aisleriot_slot_renderer_set_cache (AisleriotSlotRenderer *srend,
                                               AisleriotCardCache *cache);

static void clutter_container_iface_init (ClutterContainerIface *iface);

static void aisleriot_slot_renderer_allocate (ClutterActor *actor,
                                              const ClutterActorBox *box,
                                              gboolean origin_changed);

static void completed_cb (AisleriotSlotRenderer *srend);

G_DEFINE_TYPE_WITH_CODE (AisleriotSlotRenderer, aisleriot_slot_renderer,
                         CLUTTER_TYPE_ACTOR,
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_CONTAINER,
                                                clutter_container_iface_init));

#define AISLERIOT_SLOT_RENDERER_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), AISLERIOT_TYPE_SLOT_RENDERER, \
                                AisleriotSlotRendererPrivate))

typedef struct _AnimationData AnimationData;

struct _AisleriotSlotRendererPrivate
{
  AisleriotCardCache *cache;

  Slot *slot;

  gboolean show_highlight;
  guint highlight_start;

  ClutterTimeline *timeline;
  guint completed_handler;
  GArray *animations;
};

struct _AnimationData
{
  ClutterActor *card_tex;
  ClutterBehaviour *move, *rotate, *depth;
};

enum
{
  PROP_0,

  PROP_CACHE,
  PROP_SLOT,
  PROP_HIGHLIGHT
};

static void
aisleriot_slot_renderer_class_init (AisleriotSlotRendererClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  ClutterActorClass *actor_class = (ClutterActorClass *) klass;
  GParamSpec *pspec;

  gobject_class->dispose = aisleriot_slot_renderer_dispose;
  gobject_class->finalize = aisleriot_slot_renderer_finalize;

  gobject_class->set_property = aisleriot_slot_renderer_set_property;
  gobject_class->get_property = aisleriot_slot_renderer_get_property;

  actor_class->paint = aisleriot_slot_renderer_paint;
  actor_class->allocate = aisleriot_slot_renderer_allocate;

  pspec = g_param_spec_object ("cache", NULL, NULL,
                               AISLERIOT_TYPE_CARD_CACHE,
                               G_PARAM_WRITABLE |
                               G_PARAM_CONSTRUCT_ONLY |
                               G_PARAM_STATIC_NAME |
                               G_PARAM_STATIC_NICK |
                               G_PARAM_STATIC_BLURB);
  g_object_class_install_property (gobject_class, PROP_CACHE, pspec);

  pspec = g_param_spec_pointer ("slot", NULL, NULL,
                                G_PARAM_WRITABLE |
                                G_PARAM_CONSTRUCT_ONLY |
                                G_PARAM_STATIC_NAME |
                                G_PARAM_STATIC_NICK |
                                G_PARAM_STATIC_BLURB);
  g_object_class_install_property (gobject_class, PROP_SLOT, pspec);

  pspec = g_param_spec_uint ("highlight", NULL, NULL,
                             0, G_MAXUINT, 0,
                             G_PARAM_WRITABLE |
                             G_PARAM_READABLE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_NAME |
                             G_PARAM_STATIC_NICK |
                             G_PARAM_STATIC_BLURB);
  g_object_class_install_property (gobject_class, PROP_HIGHLIGHT, pspec);

  g_type_class_add_private (klass, sizeof (AisleriotSlotRendererPrivate));
}

static void
aisleriot_slot_renderer_init (AisleriotSlotRenderer *self)
{
  AisleriotSlotRendererPrivate *priv;

  priv = self->priv = AISLERIOT_SLOT_RENDERER_GET_PRIVATE (self);

  priv->highlight_start = G_MAXUINT;
  priv->animations = g_array_new (FALSE, FALSE, sizeof (AnimationData));
  priv->timeline = clutter_timeline_new_for_duration (500);
  g_signal_connect_swapped (priv->timeline, "completed",
                            G_CALLBACK (completed_cb), self);

}

static void
aisleriot_slot_renderer_dispose (GObject *object)
{
  AisleriotSlotRenderer *self = (AisleriotSlotRenderer *) object;
  AisleriotSlotRendererPrivate *priv = self->priv;

  aisleriot_slot_renderer_set_cache (self, NULL);

  /* Get rid of any running animations */
  aisleriot_slot_renderer_set_animations (self, 0, NULL);

  if (priv->timeline) {
    g_object_unref (priv->timeline);
    priv->timeline = NULL;
  }

  G_OBJECT_CLASS (aisleriot_slot_renderer_parent_class)->dispose (object);
}

static void
aisleriot_slot_renderer_finalize (GObject *object)
{
  AisleriotSlotRenderer *self = (AisleriotSlotRenderer *) object;
  AisleriotSlotRendererPrivate *priv = self->priv;

  g_array_free (priv->animations, TRUE);

  G_OBJECT_CLASS (aisleriot_slot_renderer_parent_class)->finalize (object);
}

ClutterActor *
aisleriot_slot_renderer_new (AisleriotCardCache *cache, Slot *slot)
{
  ClutterActor *self = g_object_new (AISLERIOT_TYPE_SLOT_RENDERER,
                                     "cache", cache,
                                     "slot", slot,
                                     NULL);

  return self;
}

static void
aisleriot_slot_renderer_set_cache (AisleriotSlotRenderer *srend,
                                   AisleriotCardCache *cache)
{
  AisleriotSlotRendererPrivate *priv = srend->priv;

  if (cache)
    g_object_ref (cache);

  if (priv->cache)
    g_object_unref (priv->cache);

  priv->cache = cache;
}

static void
aisleriot_slot_renderer_set_property (GObject *object,
                                      guint property_id,
                                      const GValue *value,
                                      GParamSpec *pspec)
{
  AisleriotSlotRenderer *srend = AISLERIOT_SLOT_RENDERER (object);
  AisleriotSlotRendererPrivate *priv = srend->priv;

  switch (property_id) {
    case PROP_CACHE:
      aisleriot_slot_renderer_set_cache (srend, g_value_get_object (value));
      break;

    case PROP_SLOT:
      priv->slot = g_value_get_pointer (value);
      break;

    case PROP_HIGHLIGHT:
      aisleriot_slot_renderer_set_highlight (srend,
                                             g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
aisleriot_slot_renderer_get_property (GObject *object,
                                      guint property_id,
                                      GValue *value,
                                      GParamSpec *pspec)
{
  AisleriotSlotRenderer *srend = AISLERIOT_SLOT_RENDERER (object);

  switch (property_id) {
    case PROP_HIGHLIGHT:
      g_value_set_uint (value,
                        aisleriot_slot_renderer_get_highlight (srend));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
aisleriot_slot_renderer_paint (ClutterActor *actor)
{
  AisleriotSlotRenderer *srend = (AisleriotSlotRenderer *) actor;
  AisleriotSlotRendererPrivate *priv = srend->priv;
  guint n_cards, first_exposed_card_id, i;
  guint8 *cards;
  int cardx, cardy;

  g_return_if_fail (priv->cache != NULL);
  g_return_if_fail (priv->slot != NULL);

  cards = priv->slot->cards->data;
  n_cards = priv->slot->cards->len;

  g_assert (n_cards >= priv->slot->exposed);
  first_exposed_card_id = n_cards - priv->slot->exposed;

  if (priv->slot->cards->len == 0) {
    CoglHandle cogl_tex;
    guint tex_width, tex_height;

    cogl_tex = aisleriot_card_cache_get_slot_texture (priv->cache,
                                                      priv->show_highlight);
    tex_width = cogl_texture_get_width (cogl_tex);
    tex_height = cogl_texture_get_height (cogl_tex);

    cogl_texture_rectangle (cogl_tex,
                            0, 0,
                            CLUTTER_INT_TO_FIXED (tex_width),
                            CLUTTER_INT_TO_FIXED (tex_height),
                            0, 0, CFX_ONE, CFX_ONE);
  }

  cardx = 0;
  cardy = 0;

  for (i = first_exposed_card_id; i < n_cards; ++i) {
    Card card = CARD (cards[i]);
    gboolean is_highlighted;
    CoglHandle cogl_tex;
    guint tex_width, tex_height;

    is_highlighted = priv->show_highlight && (i >= priv->highlight_start);

    cogl_tex = aisleriot_card_cache_get_card_texture (priv->cache,
                                                      card,
                                                      is_highlighted);

    tex_width = cogl_texture_get_width (cogl_tex);
    tex_height = cogl_texture_get_height (cogl_tex);

    cogl_texture_rectangle (cogl_tex,
                            CLUTTER_INT_TO_FIXED (cardx),
                            CLUTTER_INT_TO_FIXED (cardy),
                            CLUTTER_INT_TO_FIXED (cardx + tex_width),
                            CLUTTER_INT_TO_FIXED (cardy + tex_height),
                            0, 0, CFX_ONE, CFX_ONE);

    cardx += priv->slot->pixeldx;
    cardy += priv->slot->pixeldy;
  }

  /* Paint the animated actors */
  for (i = 0; i < priv->animations->len; i++) {
    AnimationData *data = &g_array_index (priv->animations, AnimationData, i);

    if (CLUTTER_ACTOR_IS_VISIBLE (data->card_tex))
      clutter_actor_paint (data->card_tex);
  }
}

guint
aisleriot_slot_renderer_get_highlight (AisleriotSlotRenderer *srend)
{
  g_return_val_if_fail (AISLERIOT_IS_SLOT_RENDERER (srend), 0);

  return srend->priv->highlight_start;
}

void
aisleriot_slot_renderer_set_highlight (AisleriotSlotRenderer *srend,
                                       guint highlight)
{
  AisleriotSlotRendererPrivate *priv = srend->priv;

  g_return_if_fail (AISLERIOT_IS_SLOT_RENDERER (srend));

  priv->highlight_start = highlight;
  priv->show_highlight = priv->highlight_start != G_MAXUINT;

  clutter_actor_queue_redraw (CLUTTER_ACTOR (srend));
}

void
aisleriot_slot_renderer_set_animations (AisleriotSlotRenderer *srend,
                                        guint n_anims,
                                        const AisleriotAnimStart *anims)
{
  AisleriotSlotRendererPrivate *priv;
  guint i;
  gint card_num;

  g_return_if_fail (AISLERIOT_IS_SLOT_RENDERER (srend));

  priv = srend->priv;

  g_return_if_fail (n_anims <= priv->slot->exposed);

  /* Destroy the current animations */
  for (i = 0; i < priv->animations->len; i++) {
    AnimationData *anim_data;

    anim_data = &g_array_index (priv->animations, AnimationData, i);

    if (anim_data->move)
      g_object_unref (anim_data->move);
    if (anim_data->rotate)
      g_object_unref (anim_data->rotate);
    if (anim_data->depth)
      g_object_unref (anim_data->depth);

    clutter_actor_destroy (anim_data->card_tex);
    g_object_unref (anim_data->card_tex);
  }

  g_array_set_size (priv->animations, 0);

  card_num = priv->slot->cards->len - n_anims;

  for (i = 0; i < n_anims; i++) {
    AnimationData anim_data;
    ClutterAlpha *alpha;
    ClutterKnot knots[2];
    Card card = CARD (priv->slot->cards->data[card_num]);
    guint card_width, card_height;
    CoglHandle cogl_tex;

    memset (&anim_data, 0, sizeof (anim_data));

    anim_data.card_tex = aisleriot_card_new (priv->cache, card);
    g_object_ref_sink (anim_data.card_tex);
    clutter_actor_set_parent (anim_data.card_tex, CLUTTER_ACTOR (srend));

    cogl_tex = aisleriot_card_cache_get_card_texture (priv->cache, card, FALSE);
    card_width = cogl_texture_get_width (cogl_tex);
    card_height = cogl_texture_get_height (cogl_tex);

    clutter_actor_set_position (anim_data.card_tex,
                                anims[i].cardx, anims[i].cardy);

    knots[0].x = anims[i].cardx;
    knots[0].y = anims[i].cardy;
    knots[1].x = priv->slot->pixeldx * card_num;
    knots[1].y = priv->slot->pixeldy * card_num;

    alpha = clutter_alpha_new_full (priv->timeline, CLUTTER_ALPHA_RAMP_INC,
                                    NULL, NULL);

    anim_data.move = clutter_behaviour_path_new (alpha, knots,
                                                 G_N_ELEMENTS (knots));
    clutter_behaviour_apply (anim_data.move, anim_data.card_tex);

    if (anims[i].face_down != card.attr.face_down) {
      gint center_x = card_width / 2;
      gint center_y = card_height / 2;

      clutter_actor_set_rotation (anim_data.card_tex, CLUTTER_Y_AXIS,
                                  180.0,
                                  center_x, center_y, 0);

      anim_data.rotate = clutter_behaviour_rotate_new (alpha,
                                                       CLUTTER_Y_AXIS,
                                                       CLUTTER_ROTATE_CW,
                                                       180.0, 0.0);
      clutter_behaviour_rotate_set_center (CLUTTER_BEHAVIOUR_ROTATE
                                           (anim_data.rotate),
                                           center_x, center_y, 0);

      clutter_behaviour_apply (anim_data.rotate, anim_data.card_tex);
    }

    alpha = clutter_alpha_new_full (priv->timeline, CLUTTER_ALPHA_SINE,
                                    NULL, NULL);

    anim_data.depth = clutter_behaviour_depth_new (alpha,
                                                   0, card_height);
    clutter_behaviour_apply (anim_data.depth, anim_data.card_tex);

    g_array_append_val (priv->animations, anim_data);

    card_num++;
  }

  if (n_anims > 0) {
    clutter_timeline_rewind (priv->timeline);
    clutter_timeline_start (priv->timeline);
  }

  clutter_actor_queue_redraw (CLUTTER_ACTOR (srend));
}

static void
completed_cb (AisleriotSlotRenderer *srend)
{
  /* Get rid of all animation actors */
  aisleriot_slot_renderer_set_animations (srend, 0, NULL);

  /* Redraw so that the animated actors will be drawn as part of the
     renderer instead */
  clutter_actor_queue_redraw (CLUTTER_ACTOR (srend));
}

static void
aisleriot_slot_renderer_real_add (ClutterContainer *container,
                                  ClutterActor     *actor)
{
  g_critical ("Do not add actors to an AisleriotSlotRenderer directly");
}

static void
aisleriot_slot_renderer_real_remove (ClutterContainer *container,
                                     ClutterActor     *actor)
{
  g_object_ref (actor);

  clutter_actor_unparent (actor);

  clutter_actor_queue_relayout (CLUTTER_ACTOR (container));

  if (CLUTTER_ACTOR_IS_VISIBLE (CLUTTER_ACTOR (container)))
    clutter_actor_queue_redraw (CLUTTER_ACTOR (container));

  g_object_unref (actor);
}

static void
aisleriot_slot_renderer_real_foreach (ClutterContainer *container,
                                      ClutterCallback   callback,
                                      gpointer          user_data)
{
  AisleriotSlotRenderer *srend = AISLERIOT_SLOT_RENDERER (container);
  AisleriotSlotRendererPrivate *priv = srend->priv;
  guint i;

  for (i = 0; i < priv->animations->len; i++) {
    AnimationData *data = &g_array_index (priv->animations, AnimationData, i);

    (* callback) (data->card_tex, user_data);
  }
}

static void
aisleriot_slot_renderer_real_raise (ClutterContainer *container,
                                    ClutterActor     *actor,
                                    ClutterActor     *sibling)
{
}

static void
aisleriot_slot_renderer_real_lower (ClutterContainer *container,
                                    ClutterActor     *actor,
                                    ClutterActor     *sibling)
{
}

static void
aisleriot_slot_renderer_real_sort_depth_order (ClutterContainer *container)
{
}

static void
clutter_container_iface_init (ClutterContainerIface *iface)
{
  iface->add = aisleriot_slot_renderer_real_add;
  iface->remove = aisleriot_slot_renderer_real_remove;
  iface->foreach = aisleriot_slot_renderer_real_foreach;
  iface->raise = aisleriot_slot_renderer_real_raise;
  iface->lower = aisleriot_slot_renderer_real_lower;
  iface->sort_depth_order = aisleriot_slot_renderer_real_sort_depth_order;
}

static void
aisleriot_slot_renderer_allocate (ClutterActor *actor,
                                  const ClutterActorBox *box,
                                  gboolean origin_changed)
{
  AisleriotSlotRenderer *srend = (AisleriotSlotRenderer *) actor;
  AisleriotSlotRendererPrivate *priv = srend->priv;
  guint i;

  /* chain up to set actor->allocation */
  CLUTTER_ACTOR_CLASS (aisleriot_slot_renderer_parent_class)
    ->allocate (actor, box, origin_changed);

  for (i = 0; i < priv->animations->len; i++) {
    AnimationData *anim = &g_array_index (priv->animations, AnimationData, i);

    clutter_actor_allocate_preferred_size (anim->card_tex,
                                           origin_changed);
  }
}