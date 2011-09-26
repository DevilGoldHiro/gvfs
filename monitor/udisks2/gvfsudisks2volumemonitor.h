/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#ifndef __GVFS_UDISKS2_VOLUME_MONITOR_H__
#define __GVFS_UDISKS2_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>

#include <udisks/udisks.h>

G_BEGIN_DECLS

#define GVFS_TYPE_UDISKS2_VOLUME_MONITOR (gvfs_udisks2_volume_monitor_get_type ())
#define GVFS_UDISKS2_VOLUME_MONITOR(o)   (G_TYPE_CHECK_INSTANCE_CAST ((o), GVFS_TYPE_UDISKS2_VOLUME_MONITOR, GVfsUDisks2VolumeMonitor))
#define G_IS_GDU_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), GVFS_TYPE_UDISKS2_VOLUME_MONITOR))

typedef struct _GVfsUDisks2VolumeMonitor GVfsUDisks2VolumeMonitor;

/* Forward definitions */
typedef struct _GVfsUDisks2Drive GVfsUDisks2Drive;
typedef struct _GVfsUDisks2Volume GVfsUDisks2Volume;
typedef struct _GVfsUDisks2Mount GVfsUDisks2Mount;

GType           gvfs_udisks2_volume_monitor_get_type (void) G_GNUC_CONST;
GVolumeMonitor *gvfs_udisks2_volume_monitor_new      (void);

G_END_DECLS

#endif /* __GVFS_UDISKS2_VOLUME_MONITOR_H__ */