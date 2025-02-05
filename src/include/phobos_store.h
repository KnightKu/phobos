/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2022 CEA/DAM.
 *
 *  This file is part of Phobos.
 *
 *  Phobos is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  Phobos is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
 */
/**
 * \brief  Phobos Object Store interface
 */
#ifndef _PHO_STORE_H
#define _PHO_STORE_H

#include "pho_attrs.h"
#include "pho_types.h"
#include <stdlib.h>

struct pho_xfer_desc;

/**
 * Transfer (GET / PUT / MPUT) flags.
 * Exact semantic depends on the operation it is applied on.
 */
enum pho_xfer_flags {
    /**
     * put: replace the object if it already exists (_not supported_)
     * get: replace the target file if it already exists
     */
    PHO_XFER_OBJ_REPLACE    = (1 << 0),
    /* get: check the object's location before getting it */
    PHO_XFER_OBJ_BEST_HOST  = (1 << 1),
};

/**
 * Multiop completion notification callback.
 * Invoked with:
 *  - user-data pointer
 *  - the operation descriptor
 *  - the return code for this operation: 0 on success, neg. errno on failure
 */
typedef void (*pho_completion_cb_t)(void *u, const struct pho_xfer_desc *, int);

/**
 * Phobos XFer operations.
 */
enum pho_xfer_op {
    PHO_XFER_OP_PUT,   /**< PUT operation. */
    PHO_XFER_OP_GET,   /**< GET operation. */
    PHO_XFER_OP_GETMD, /**< GET metadata operation. */
    PHO_XFER_OP_DEL,   /**< DEL operation. */
    PHO_XFER_OP_UNDEL, /**< UNDEL operation. */
    PHO_XFER_OP_LAST
};

static const char * const xfer_op_names[] = {
    [PHO_XFER_OP_PUT]   = "PUT",
    [PHO_XFER_OP_GET]   = "GET",
    [PHO_XFER_OP_GETMD] = "GETMD",
    [PHO_XFER_OP_DEL]   = "DELETE",
    [PHO_XFER_OP_UNDEL] = "UNDELETE",
};

static inline const char *xfer_op2str(enum pho_xfer_op op)
{
    if (op >= PHO_XFER_OP_LAST)
        return NULL;

    return xfer_op_names[op];
}

/**
 * PUT parameters.
 * Family, layout_name and tags can be set directly or by using an alias.
 * An alias is a name defined in the phobos config to combine these parameters.
 * The alias will not override family and layout if they have been specified
 * in this struct but extend existing tags.
 */
struct pho_xfer_put_params {
    ssize_t          size;        /**< Amount of data to write. */
    enum rsc_family  family;      /**< Targeted resource family. */
    const char      *layout_name; /**< Name of the layout module to use. */
    struct pho_attrs lyt_params;  /**< Parameters used for the layout */
    struct tags      tags;        /**< Tags to select a media to write. */
    const char      *alias;       /**< Identifier for family, layout,
                                    *  tag combination
                                    */
    bool             overwrite;   /**< true if the put command could be an
                                    *  update.
                                    */
};

/**
 * GET parameters.
 * Node_name corresponds to the name of the node the object can be retrieved
 * from, if a phobos_get call fails.
 */
struct pho_xfer_get_params {
    char *node_name;                    /**< Node name [out] */
};

/**
 * Operation parameters.
 */
union pho_xfer_params {
    struct pho_xfer_put_params put;     /**< PUT parameters. */
    struct pho_xfer_get_params get;     /**< GET parameters */
};

/**
 * Xfer descriptor.
 * The source/destination semantics of the fields vary
 * depending on the nature of the operation.
 * See below:
 *  - pĥobos_getmd()
 *  - phobos_get()
 *  - phobos_put()
 *  - phobos_undelete()
 */
struct pho_xfer_desc {
    char                   *xd_objid;  /**< Object ID to read or write. */
    char                   *xd_objuuid;/**< Object UUID to read or write. */
    int                     xd_version;/**< Object version. */
    enum pho_xfer_op        xd_op;     /**< Operation to perform. */
    int                     xd_fd;     /**< FD of the source/destination. */
    struct pho_attrs        xd_attrs;  /**< User defined attributes. */
    union pho_xfer_params   xd_params; /**< Operation parameters. */
    enum pho_xfer_flags     xd_flags;  /**< See enum pho_xfer_flags doc. */
    int                     xd_rc;     /**< Outcome of this xfer. */
};

/**
 * Put N files to the object store with minimal overhead.
 * Each desc entry contains:
 * - objid: the target object identifier
 * - fd: an opened fd to read from
 * - size: amount of data to read from fd
 * - layout_name: (optional) name of the layout module to use
 * - attrs: the metadata (optional)
 * - flags: behavior flags
 * - tags: tags defining constraints on which media can be selected to put the
 *   data
 * Other fields are not used.
 *
 * Individual completion notifications are issued via xd_callback.
 * This function returns the first encountered error or 0 if all
 * sub-operations have succeeded.
 */
int phobos_put(struct pho_xfer_desc *xfers, size_t n,
               pho_completion_cb_t cb, void *udata);

/**
 * Retrieve N files from the object store
 * desc contains:
 * - objid:   identifier of the object to retrieve, this is mandatory.
 *
 * - objuuid: uuid of the object to retrieve
 *            if not NULL, this field is duplicated internally and freed by
 *            pho_xfer_desc_clean(). The caller have to make sure to keep
 *            a copy of this pointer if it needs to be freed.
 *            if NULL and there is an object alive, get the current generation
 *            if NULL and there is no object alive, check the deprecated
 *            objects:
 *                if they all share the same uuid, the object matching
 *                the version criteria is retrieved
 *
 * - version: version of the object to retrieve
 *            if 0, get the most recent object. Otherwise, the object with the
 *            matching version is returned if it exists
 *            if there is an object in the object table and its version does
 *            not match, phobos_get() will target the current generation and
 *            query the deprecated_object table
 *
 * - fd: an opened fd to write to
 * - attrs: unused (can be NULL)
 * - flags: behavior flags
 *
 * If objuuid and version are NULL and 0, phobos_get() will only query the
 * object table. Otherwise, the object table is queried first and then the
 * deprecated_object table.
 *
 * Other fields are not used.
 *
 * Individual completion notifications are issued via xd_callback.
 * This function returns the first encountered error or 0 if all
 * sub-operations have succeeded.
 */
int phobos_get(struct pho_xfer_desc *xfers, size_t n,
               pho_completion_cb_t cb, void *udata);

/**
 * Retrieve N file metadata from the object store
 * desc contains:
 * - objid: identifier of the object to retrieve
 * - attrs: unused (can be NULL)
 * - flags: behavior flags
 * Other fields are not used.
 *
 * Individual completion notifications are issued via xd_callback.
 * This function returns the first encountered error of 0 if all
 * sub-operations have succeeded.
 */
int phobos_getmd(struct pho_xfer_desc *xfers, size_t n,
                 pho_completion_cb_t cb, void *udata);

/** query metadata of the object store */
/* TODO int phobos_query(criteria, &obj_list); */

/**
 * Delete an object from the object store
 *
 * This deletion is not a hard remove, and only deprecate the object.
 *
 * @param[in]   xfers       Objects to delete, only the oid field is used
 * @param[in]   num_xfers   Number of objects to delete
 *
 * @return                  0 on success, -errno on failure
 */
int phobos_delete(struct pho_xfer_desc *xfers, size_t num_xfers);

/**
 * Undelete a deprecated object from the object store
 *
 * The latest version of each deprecated object is moved back.
 *
 * @param[in]   xfers       Objects to undelete, only the uuid field is used
 * @param[in]   num_xfers   Number of objects to undelete
 *
 * @return                  0 on success, -errno on failure
 */
int phobos_undelete(struct pho_xfer_desc *xfers, size_t num_xfers);

/**
 * Retrieve one node name from which an object can be accessed.
 *
 * If the media having this object are locked by a node, this function returns
 * the hostname of this node. If there is currently no node that locks the media
 * having this object, \p hostname is set to NULL with a return code of 0 to
 * indicate that any node can perform an operation on this object.
 *
 * At least one of \p oid or \p uuid must not be NULL.
 *
 * If \p version is not provided (zero as input), the latest one is located.
 *
 * If \p uuid is not provided, we first try to find the corresponding \p oid
 * from living objects into the object table. If there is no living object with
 * \p oid, we check amongst all deprecated objects. If there is only one
 * corresponding \p uuid, in the deprecated objects, we take this one. If there
 * is more than one \p uuid corresponding to this \p oid, we return -EINVAL.
 *
 * @param[in]   oid         OID of the object to locate (ignored if NULL and
 *                          \p uuid must not be NULL)
 * @param[in]   uuid        UUID of the object to locate (ignored if NULL and
 *                          \p oid must not be NULL)
 * @param[in]   version     Version of the object to locate (ignored if zero)
 * @param[out]  hostname    Allocated and returned hostname of the node which
 *                          can give access to the object (NULL is returned on
 *                          error or if no locks are found on the object)
 *
 * @return                  0 on success (\p hostname can be NULL)
 *                          or -errno on failure,
 *                          -ENOENT if no object corresponds to input
 *                          -EINVAL if more than one object corresponds to input
 *                          -EAGAIN if there is not any convenient node to
 *                          currently retrieve this object
 *                          -ENODEV if there is no existing medium to retrieve
 *                          this object
 *                          -EADDRNOTAVAIL if we cannot get self hostname
 */
int phobos_locate(const char *obj_id, const char *uuid, int version,
                  char **hostname);

/**
 * Clean a pho_xfer_desc structure by freeing the uuid and attributes, and
 * the tags in case the xfer corresponds to a PUT operation.
 *
 * @param[in]   xfer        The xfer structure to clean.
 */
void pho_xfer_desc_clean(struct pho_xfer_desc *xfer);

/**
 * Retrieve the objects that match the given pattern and metadata.
 * If given multiple objids or patterns, retrieve every item with name
 * matching any of those objids or patterns.
 * If given multiple objids or patterns, and metadata, retrieve every item
 * with name matching any of those objids or pattersn, but containing
 * every given metadata.
 *
 * The caller must release the list calling phobos_store_object_list_free().
 *
 * \param[in]       res             Objids or patterns, depending on
 *                                  \a is_pattern.
 * \param[in]       n_res           Number of requested objids or patterns.
 * \param[in]       is_pattern      True if search using POSIX pattern.
 * \param[in]       metadata        Metadata filter.
 * \param[in]       n_metadata      Number of requested metadata.
 * \param[in]       deprecated      true if search from deprecated objects.
 * \param[out]      objs            Retrieved objects.
 * \param[out]      n_objs          Number of retrieved items.
 *
 * \return                          0     on success,
 *                                 -errno on failure.
 */
int phobos_store_object_list(const char **res, int n_res, bool is_pattern,
                             const char **metadata, int n_metadata,
                             bool deprecated, struct object_info **objs,
                             int *n_objs);

/**
 * Release the list retrieved using phobos_store_object_list().
 *
 * \param[in]       objs            Objects to release.
 * \param[in]       n_objs          Number of objects to release.
 */
void phobos_store_object_list_free(struct object_info *objs, int n_objs);

#endif
