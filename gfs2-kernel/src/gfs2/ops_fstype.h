/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#ifndef __OPS_FSTYPE_DOT_H__
#define __OPS_FSTYPE_DOT_H__

int gfs2_test_bdev_super(struct super_block *sb, void *data);
int gfs2_set_bdev_super(struct super_block *sb, void *data);

extern struct file_system_type gfs2_fs_type;

#endif /* __OPS_FSTYPE_DOT_H__ */
