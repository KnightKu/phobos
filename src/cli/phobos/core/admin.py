#!/usr/bin/env python3

#
#  All rights reserved (c) 2014-2019 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the License, or
#  (at your option) any later version.
#
#  Phobos is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
#

"""
Provide access to admin commands with the right level (tm) of abstraction.
"""

import errno
import os

from ctypes import *

from phobos.core.const import (PHO_FS_LTFS, PHO_FS_POSIX,
                               PHO_RSC_DIR, PHO_RSC_TAPE)
from phobos.core.dss import DSSHandle
from phobos.core.ffi import (CommInfo, ExtentInfo, LayoutInfo, LIBPHOBOS_ADMIN,
                             LRS, Id)

from phobos.core.const import rsc_family2str

class AdminHandle(Structure):
    """Admin handler"""
    _fields_ = [
        ('comm', CommInfo),
        ('dss', DSSHandle),
        ('daemon_is_online', c_int),
        ('lock_owner', c_char_p),
    ]

class Client(object):
    """Wrapper on the phobos admin client"""
    def __init__(self, lrs_required=True):
        super(Client, self).__init__()
        self.lrs_required = lrs_required
        self.handle = None

    def __enter__(self):
        self.init(self.lrs_required)
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.fini()

    def init(self, lrs_required):
        if self.handle is not None:
            self.fini()

        self.handle = AdminHandle()

        rc = LIBPHOBOS_ADMIN.phobos_admin_init(byref(self.handle), lrs_required)
        if rc:
            raise EnvironmentError(rc, 'Admin initialization failed')

    def fini(self):
        if self.handle is not None:
            LIBPHOBOS_ADMIN.phobos_admin_fini(byref(self.handle))
            self.handle = None

    def fs_format(self, medium_id, fs_type, unlock=False):
        """Format a medium through the LRS layer."""
        fs_type = fs_type.lower()
        if fs_type == 'ltfs':
            rsc_family = PHO_RSC_TAPE
            fs_type_enum = PHO_FS_LTFS
        elif fs_type == 'posix':
            rsc_family = PHO_RSC_DIR
            fs_type_enum = PHO_FS_POSIX
        else:
            raise EnvironmentError(errno.EOPNOTSUPP,
                                   "Unknown filesystem type '%s'" % fs_type)

        mstruct = Id(rsc_family, name=medium_id)
        rc = LIBPHOBOS_ADMIN.phobos_admin_format(byref(self.handle),
                                                 byref(mstruct), fs_type_enum,
                                                 unlock)
        if rc:
            raise EnvironmentError(rc, "Cannot format medium '%s'" % medium_id)

    def device_add(self, dev_family, dev_names, keep_locked):
        """Add devices to the LRS."""
        c_id = Id * len(dev_names)
        dev_ids = [Id(dev_family, name=name) for name in dev_names]

        rc = LIBPHOBOS_ADMIN.phobos_admin_device_add(byref(self.handle),
                                                     c_id(*dev_ids),
                                                     len(dev_ids), keep_locked)
        if rc:
            raise EnvironmentError(rc, "Error during device add")

    def device_lock(self, dev_family, dev_names, is_forced):
        """Wrapper for the device lock command."""
        c_id = Id * len(dev_names)
        dev_ids = [Id(dev_family, name=name) for name in dev_names]

        rc = LIBPHOBOS_ADMIN.phobos_admin_device_lock(byref(self.handle),
                                                      c_id(*dev_ids),
                                                      len(dev_ids), is_forced)
        if rc:
            raise EnvironmentError(rc, "Error during device lock")

    def device_unlock(self, dev_family, dev_names, is_forced):
        """Wrapper for the device unlock command."""
        c_id = Id * len(dev_names)
        dev_ids = [Id(dev_family, name=name) for name in dev_names]

        rc = LIBPHOBOS_ADMIN.phobos_admin_device_unlock(byref(self.handle),
                                                        c_id(*dev_ids),
                                                        len(dev_ids),
                                                        is_forced)
        if rc:
            raise EnvironmentError(rc, "Error during device unlock")

    def layout_list(self, pattern, medium, degroup):
        """List layouts."""
        n_layouts = c_int(0)
        layouts = pointer(LayoutInfo())
        rc = LIBPHOBOS_ADMIN.phobos_admin_layout_list(byref(self.handle),
                                                      pattern,
                                                      medium,
                                                      byref(layouts),
                                                      byref(n_layouts))

        if rc:
            raise EnvironmentError(rc)

        if not degroup:
            list_lyts = [layouts[i] for i in range(n_layouts.value)]
        else:
            list_lyts = []
            for i in range(n_layouts.value):
                ptr = layouts[i].extents
                cnt = layouts[i].ext_count
                for j in range(cnt):
                    if medium is None or \
                        medium in cast(ptr, POINTER(ExtentInfo))[j].media.name:
                        lyt = type(layouts[i])()
                        pointer(lyt)[0] = layouts[i]
                        lyt.ext_count = 1
                        lyt.extents = addressof(cast(ptr,
                                                     POINTER(ExtentInfo))[j])
                        list_lyts.append(lyt)

        return list_lyts, layouts, n_layouts

    def layout_list_free(self, layouts, n_layouts):
        """Free a previously obtained layout list."""
        LIBPHOBOS_ADMIN.phobos_admin_layout_list_free(layouts, n_layouts)
