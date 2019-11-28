/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2017 CEA/DAM.
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
 * \brief  Phobos Local Resource Scheduler (LRS)
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pho_lrs.h"
#include "pho_common.h"
#include "pho_dss.h"
#include "pho_type_utils.h"
#include "pho_ldm.h"
#include "pho_io.h"
#include "lrs_cfg.h"

#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <assert.h>
#include <sys/utsname.h>
#include <jansson.h>
#include <stdbool.h>
#include <stdio.h>

#define TAPE_TYPE_SECTION_CFG "tape_type \"%s\""
#define MODELS_CFG_PARAM "models"
#define DRIVE_RW_CFG_PARAM "drive_rw"
#define DRIVE_TYPE_SECTION_CFG "drive_type \"%s\""

/** Used to indicate that a lock is held by an external process */
#define LRS_MEDIA_LOCKED_EXTERNAL   ((void *)0x19880429)

/**
 * Build a mount path for the given identifier.
 * @param[in] id    Unique drive identified on the host.
 * The result must be released by the caller using free(3).
 */
static char *mount_point(const char *id)
{
    const char  *mnt_cfg;
    char        *mnt_out;

    mnt_cfg = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, mount_prefix);
    if (mnt_cfg == NULL)
        return NULL;

    /* mount the device as PHO_MNT_PREFIX<id> */
    if (asprintf(&mnt_out, "%s%s", mnt_cfg, id) < 0)
        return NULL;

    return mnt_out;
}

/** return the default device family to write data */
static enum dev_family default_family(void)
{
    const char *fam_str;

    fam_str = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, default_family);
    if (fam_str == NULL)
        return PHO_DEV_INVAL;

    return str2dev_family(fam_str);
}

static struct utsname host_info;

/** get host name once (/!\ not thread-safe). */
static const char *get_hostname(void)
{
    if (host_info.nodename[0] == '\0') {
        char *dot;

        if (uname(&host_info) != 0) {
            pho_error(errno, "Failed to get host name");
            return NULL;
        }
        dot = strchr(host_info.nodename, '.');
        if (dot)
            *dot = '\0';
    }
    return host_info.nodename;
}

/** all needed information to select devices */
struct dev_descr {
    struct dev_info     *dss_dev_info; /**< device info from DSS */
    struct lib_drv_info  lib_dev_info; /**< device info from library
                                            (for tape drives) */
    struct ldm_dev_state sys_dev_state; /**< device info from system */

    enum dev_op_status   op_status; /**< operational status of the device */
    char                 dev_path[PATH_MAX]; /**< path to the device */
    struct media_id      media_id; /**< id of the media (if loaded) */
    struct media_info   *dss_media_info;  /**< loaded media info from DSS, if any */
    char                 mnt_path[PATH_MAX]; /**< mount path of the filesystem */
    bool                 locked_local;       /**< dss lock acquired by us */
};

/* Needed local function declarations */
static struct dev_descr *search_loaded_media(struct lrs *lrs,
                                             const struct media_id *id);

/** check that device info from DB is consistent with actual status */
static int check_dev_info(const struct dev_descr *dev)
{
    ENTRY;

    if (dev->dss_dev_info->model == NULL
        || dev->sys_dev_state.lds_model == NULL) {
        if (dev->dss_dev_info->model != dev->sys_dev_state.lds_model)
            LOG_RETURN(-EINVAL, "%s: missing or unexpected device model",
                       dev->dev_path);
        else
            pho_debug("%s: no device model is set", dev->dev_path);

    } else if (strcmp(dev->dss_dev_info->model,
                      dev->sys_dev_state.lds_model) != 0) {
        /* @TODO ignore blanks at the end of the model */
        LOG_RETURN(-EINVAL, "%s: configured device model '%s' differs from "
                   "actual device model '%s'", dev->dev_path,
                   dev->dss_dev_info->model, dev->sys_dev_state.lds_model);
    }

    if (dev->dss_dev_info->serial == NULL
        || dev->sys_dev_state.lds_serial == NULL) {
        if (dev->dss_dev_info->serial != dev->sys_dev_state.lds_serial)
            LOG_RETURN(-EINVAL, "%s: missing or unexpected device serial",
                       dev->dss_dev_info->path);
        else
            pho_debug("%s: no device serial is set", dev->dev_path);
    } else if (strcmp(dev->dss_dev_info->serial,
                      dev->sys_dev_state.lds_serial) != 0) {
        LOG_RETURN(-EINVAL, "%s: configured device serial '%s' differs from "
                   "actual device serial '%s'", dev->dev_path,
                   dev->dss_dev_info->serial, dev->sys_dev_state.lds_serial);
    }

    return 0;
}

/**
 * Lock a device at DSS level to prevent concurrent access.
 */
static int lrs_dev_acquire(struct lrs *lrs, struct dev_descr *pdev)
{
    int rc;
    ENTRY;

    if (!lrs->dss || !pdev)
        return -EINVAL;

    if (pdev->locked_local) {
        pho_debug("Device '%s' already locked (ignoring)", pdev->dev_path);
        return 0;
    }

    rc = dss_device_lock(lrs->dss, pdev->dss_dev_info, 1, lrs->lock_owner);
    if (rc) {
        pho_warn("Cannot lock device '%s': %s", pdev->dev_path,
                 strerror(-rc));
        return rc;
    }

    pho_debug("Acquired ownership on device '%s'", pdev->dev_path);
    pdev->locked_local = true;

    return 0;
}

/**
 * Unlock a device at DSS level.
 */
static int lrs_dev_release(struct lrs *lrs, struct dev_descr *pdev)
{
    int rc;
    ENTRY;

    if (!lrs->dss || !pdev)
        return -EINVAL;

    if (!pdev->locked_local) {
        pho_debug("Device '%s' is not locked (ignoring)", pdev->dev_path);
        return 0;
    }

    rc = dss_device_unlock(lrs->dss, pdev->dss_dev_info, 1, lrs->lock_owner);
    if (rc)
        LOG_RETURN(rc, "Cannot unlock device '%s'", pdev->dev_path);

    pho_debug("Released ownership on device '%s'", pdev->dev_path);
    pdev->locked_local = false;

    return 0;
}

/**
 * Lock a media at DSS level to prevent concurrent access.
 */
static int lrs_media_acquire(struct lrs *lrs, struct media_info *pmedia)
{
    const char  *media_id;
    int          rc;
    ENTRY;

    if (!lrs->dss || !pmedia)
        return -EINVAL;

    media_id = media_id_get(&pmedia->id);

    rc = dss_media_lock(lrs->dss, pmedia, 1, lrs->lock_owner);
    if (rc) {
        pmedia->lock.lock = LRS_MEDIA_LOCKED_EXTERNAL;
        LOG_RETURN(rc, "Cannot lock media '%s'", media_id);
    }

    pho_debug("Acquired ownership on media '%s'", media_id);
    return 0;
}

/**
 * Unlock a media at DSS level.
 */
static int lrs_media_release(struct lrs *lrs, struct media_info *pmedia)
{
    const char  *media_id;
    int          rc;
    ENTRY;

    if (!lrs->dss || !pmedia)
        return -EINVAL;

    media_id = media_id_get(&pmedia->id);

    rc = dss_media_unlock(lrs->dss, pmedia, 1, lrs->lock_owner);
    if (rc)
        LOG_RETURN(rc, "Cannot unlock media '%s'", media_id);

    pho_debug("Released ownership on media '%s'", media_id);
    return 0;
}

/**
 * True if this lock is empty, only used to test the lock when returned from
 * the database and replace it with the generic LRS_MEDIA_LOCKED_EXTERNAL
 */
static bool lock_empty(const struct pho_lock *lock)
{
    return lock->lock == NULL || lock->lock[0] == '\0';
}

/**
 * False if the device is locked or if it contains a locked media,
 * true otherwise.
 */
static bool dev_is_available(const struct dev_descr *devd)
{
    if (devd->locked_local) {
        pho_debug("'%s' is locked\n", devd->dev_path);
        return false;
    }

    /* Test if the contained media lock is taken by another phobos */
    if (devd->dss_media_info != NULL
            && &devd->dss_media_info->lock == LRS_MEDIA_LOCKED_EXTERNAL) {
        pho_debug("'%s' contains a locked media", devd->dev_path);
        return false;
    }
    return true;
}

/**
 * Retrieve media info from DSS for the given label.
 * @param pmedia[out] returned pointer to a media_info structure
 *                    allocated by this function.
 * @param label[in]   label of the media.
 */
static int lrs_fill_media_info(struct dss_handle *dss,
                               struct media_info **pmedia,
                               const struct media_id *id)
{
    struct media_info   *media_res = NULL;
    const char          *id_str;
    struct dss_filter    filter;
    int                  mcnt = 0;
    int                  rc;

    if (id == NULL || pmedia == NULL)
        return -EINVAL;

    id_str = media_id_get(id);

    pho_debug("Retrieving media info for %s '%s'",
              dev_family2str(id->type), id_str);

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          "  {\"DSS::MDA::id\": \"%s\"}"
                          "]}", dev_family2str(id->type), media_id_get(id));
    if (rc)
        return rc;

    /* get media info from DB */
    rc = dss_media_get(dss, &filter, &media_res, &mcnt);
    if (rc)
        GOTO(out_nores, rc);

    if (mcnt == 0) {
        pho_info("No media found matching %s '%s'",
                 dev_family2str(id->type), id_str);
        /* no such device or address */
        GOTO(out_free, rc = -ENXIO);
    } else if (mcnt > 1)
        LOG_GOTO(out_free, rc = -EINVAL,
                 "Too many media found matching id '%s'", id_str);

    media_info_free(*pmedia);
    *pmedia = media_info_dup(media_res);

    /* If the lock is already taken, mark it as externally locked */
    if (!lock_empty(&(*pmedia)->lock)) {
        pho_info("Media '%s' is locked (%s)", id_str, (*pmedia)->lock.lock);
        (*pmedia)->lock.lock = LRS_MEDIA_LOCKED_EXTERNAL;
    }

    pho_debug("%s: spc_free=%zd",
              media_id_get(&(*pmedia)->id), (*pmedia)->stats.phys_spc_free);

    rc = 0;

out_free:
    dss_res_free(media_res, mcnt);
out_nores:
    dss_filter_free(&filter);
    return rc;
}

/**
 * Retrieve device information from system and complementary info from DB.
 * - check DB device info is consistent with mtx output.
 * - get operationnal status from system (loaded or not).
 * - for loaded drives, the mounted volume + LTFS mount point, if mounted.
 * - get media information from DB for loaded drives.
 *
 * @param[in]  dss  handle to dss connection.
 * @param[in]  lib  library handler for tape devices.
 * @param[out] devd dev_descr structure filled with all needed information.
 */
static int lrs_fill_dev_info(struct dss_handle *dss, struct lib_adapter *lib,
                             struct dev_descr *devd)
{
    struct dev_adapter deva;
    struct dev_info   *devi;
    int                rc;
    ENTRY;

    if (devd == NULL)
        return -EINVAL;

    devi = devd->dss_dev_info;

    media_info_free(devd->dss_media_info);
    devd->dss_media_info = NULL;

    rc = get_dev_adapter(devi->family, &deva);
    if (rc)
        return rc;

    /* get path for the given serial */
    rc = ldm_dev_lookup(&deva, devi->serial, devd->dev_path,
                        sizeof(devd->dev_path));
    if (rc) {
        pho_debug("Device lookup failed: serial '%s'", devi->serial);
        return rc;
    }

    /* now query device by path */
    ldm_dev_state_fini(&devd->sys_dev_state);
    rc = ldm_dev_query(&deva, devd->dev_path, &devd->sys_dev_state);
    if (rc) {
        pho_debug("Failed to query device '%s'", devd->dev_path);
        return rc;
    }

    /* compare returned device info with info from DB */
    rc = check_dev_info(devd);
    if (rc)
        return rc;

    /* Query the library about the drive location and whether it contains
     * a media. */
    rc = ldm_lib_drive_lookup(lib, devi->serial, &devd->lib_dev_info);
    if (rc) {
        pho_debug("Failed to query the library about device '%s'",
                  devi->serial);
        return rc;
    }

    if (devd->lib_dev_info.ldi_full) {
        struct fs_adapter fsa;

        devd->op_status = PHO_DEV_OP_ST_LOADED;
        devd->media_id = devd->lib_dev_info.ldi_media_id;

        pho_debug("Device '%s' (S/N '%s') contains media '%s'", devd->dev_path,
                  devi->serial, media_id_get(&devd->media_id));

        /* get media info for loaded drives */
        rc = lrs_fill_media_info(dss, &devd->dss_media_info, &devd->media_id);

        /*
         * If the drive is marked as locally locked, the contained media was in
         * fact locked by us, this happens when the raid1 layout refreshes the
         * drive list.
         */
        if (devd->locked_local &&
                devd->dss_media_info->lock.lock == LRS_MEDIA_LOCKED_EXTERNAL)
            devd->dss_media_info->lock.lock = NULL;

        /* Drive has not been found, mark it as unusable */
        if (rc == -ENXIO)
            devd->op_status = PHO_DEV_OP_ST_FAILED;
        else if (rc)
            return rc;
        else {
            /* See if the device is currently mounted */
            rc = get_fs_adapter(devd->dss_media_info->fs.type, &fsa);
            if (rc)
                return rc;

            /* If device is loaded, check if it is mounted as a filesystem */
            rc = ldm_fs_mounted(&fsa, devd->dev_path, devd->mnt_path,
                                sizeof(devd->mnt_path));

            if (rc == 0) {
                pho_debug("Discovered mounted filesystem at '%s'",
                          devd->mnt_path);
                devd->op_status = PHO_DEV_OP_ST_MOUNTED;
            } else if (rc == -ENOENT)
                /* not mounted, not an error */
                rc = 0;
            else
                LOG_RETURN(rc, "Cannot determine if device '%s' is mounted",
                           devd->dev_path);
        }
    } else {
        devd->op_status = PHO_DEV_OP_ST_EMPTY;
    }

    pho_debug("Drive '%s' is '%s'", devd->dev_path,
              op_status2str(devd->op_status));

    return rc;
}

/** Wrap library open operations
 * @param[out] lib  Library handler.
 */
static int wrap_lib_open(enum dev_family dev_type, struct lib_adapter *lib)
{
    const char *lib_dev;
    int         rc;

    /* non-tape cases: dummy lib adapter (no open required) */
    if (dev_type != PHO_DEV_TAPE)
        return get_lib_adapter(PHO_LIB_DUMMY, lib);

    /* tape case */
    rc = get_lib_adapter(PHO_LIB_SCSI, lib);
    if (rc)
        LOG_RETURN(rc, "Failed to get library adapter");

    /* For now, one single configurable path to library device.
     * This will have to be changed to manage multiple libraries.
     */
    lib_dev = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, lib_device);
    if (!lib_dev)
        LOG_RETURN(rc, "Failed to get default library device from config");

    return ldm_lib_open(lib, lib_dev);
}

/**
 * Load device states into memory.
 * Do nothing if device status is already loaded.
 */
static int lrs_load_dev_state(struct lrs *lrs)
{
    struct dev_info    *devs = NULL;
    int                 dcnt = 0;
    enum dev_family     family;
    struct lib_adapter  lib;
    int                 i;
    int                 rc;
    ENTRY;

    family = default_family();
    if (family == PHO_DEV_INVAL)
        return -EINVAL;

    /* If no device has previously been loaded, load the list of available
     * devices from DSS; otherwise just refresh informations for the current
     * list of devices
     */
    if (lrs->devices == NULL || lrs->dev_count == 0) {
        struct dss_filter   filter;

        rc = dss_filter_build(&filter,
                              "{\"$AND\": ["
                              "  {\"DSS::DEV::host\": \"%s\"},"
                              "  {\"DSS::DEV::adm_status\": \"%s\"},"
                              "  {\"DSS::DEV::family\": \"%s\"}"
                              "]}",
                              get_hostname(),
                              adm_status2str(PHO_DEV_ADM_ST_UNLOCKED),
                              dev_family2str(family));
        if (rc)
            return rc;

        /* get all unlocked devices from DB for the given family */
        rc = dss_device_get(lrs->dss, &filter, &devs, &dcnt);
        dss_filter_free(&filter);
        if (rc)
            GOTO(err_no_res, rc);

        if (dcnt == 0) {
            pho_info("No usable device found (%s): check devices status",
                     dev_family2str(family));
            GOTO(err, rc = -ENXIO);
        }

        lrs->dev_count = dcnt;
        lrs->devices = calloc(dcnt, sizeof(*lrs->devices));
        if (lrs->devices == NULL)
            GOTO(err, rc = -ENOMEM);

        /* Copy information from DSS to local device list */
        for (i = 0 ; i < dcnt; i++)
            lrs->devices[i].dss_dev_info = dev_info_dup(&devs[i]);
    }

    /* get a handle to the library to query it */
    rc = wrap_lib_open(family, &lib);
    if (rc)
        GOTO(err, rc);

    for (i = 0 ; i < lrs->dev_count; i++) {
        rc = lrs_fill_dev_info(lrs->dss, &lib, &lrs->devices[i]);
        if (rc) {
            pho_debug("Marking device '%s' as failed",
                      lrs->devices[i].dev_path);
            lrs->devices[i].op_status = PHO_DEV_OP_ST_FAILED;
        }
    }

    /* close handle to the library */
    ldm_lib_close(&lib);

    rc = 0;

err:
    /* free devs array, as they have been copied to devices[].device */
    dss_res_free(devs, dcnt);
err_no_res:
    return rc;
}

int lrs_device_add(struct lrs *lrs, const struct dev_info *devi)
{
    int rc = 0;
    struct lib_adapter lib;
    struct dev_descr device = {0};

    pho_verb("Adding device '%s' to lrs\n", devi->serial);

    /* get a handle to the library to query it */
    rc = wrap_lib_open(devi->family, &lib);
    if (rc)
        return rc;

    device.dss_dev_info = dev_info_dup(devi);

    /* Retrieve device information */
    rc = lrs_fill_dev_info(lrs->dss, &lib, &device);
    if (rc)
        GOTO(err, rc);

    /* Add the newly initialized device to the device list */
    lrs->devices = realloc(lrs->devices,
                           (lrs->dev_count + 1) * sizeof(*lrs->devices));
    if (lrs->devices == NULL)
        GOTO(err, rc = -ENOMEM);

    lrs->dev_count++;
    lrs->devices[lrs->dev_count - 1] = device;

err:
    ldm_lib_close(&lib);
    return rc;
}

static void dev_descr_fini(struct dev_descr *dev)
{
    dev_info_free(dev->dss_dev_info);
    dev->dss_dev_info = NULL;

    media_info_free(dev->dss_media_info);
    dev->dss_media_info = NULL;

    ldm_dev_state_fini(&dev->sys_dev_state);
}

static __thread uint64_t lrs_lock_number;

int lrs_init(struct lrs *lrs, struct dss_handle *dss)
{
    int rc;

    lrs->devices = NULL;
    lrs->dev_count = 0;
    lrs->dss = dss;
    /*
     * For the lock owner name to generate a collision, either the tid or the
     * lrs_lock_number has to loop in less than 1 second.
     *
     * Ensure that we don't build an identifier bigger than 256 characters.
     */
    rc = asprintf(&lrs->lock_owner, "%.213s:%.8lx:%.16lx:%.16lx",
                  get_hostname(), syscall(SYS_gettid), time(NULL),
                  lrs_lock_number);
    if (rc == -1)
        return -ENOMEM;
    lrs_lock_number++;
    return 0;
}

void lrs_fini(struct lrs *lrs)
{
    size_t i;

    if (lrs == NULL)
        return;

    for (i = 0; i < lrs->dev_count; i++)
        dev_descr_fini(&lrs->devices[i]);

    free(lrs->devices);
    free(lrs->lock_owner);
}

/**
 * Build a filter string fragment to filter on a given tag set. The returned
 * string is allocated with malloc. NULL is returned when ENOMEM is encountered.
 *
 * The returned string looks like the following:
 * {"$AND": [{"DSS:MDA::tags": "tag1"}]}
 */
static char *build_tag_filter(const struct tags *tags)
{
    json_t *and_filter = NULL;
    json_t *tag_filters = NULL;
    char   *tag_filter_json = NULL;
    size_t  i;

    /* Build a json array to properly format tag related DSS filter */
    tag_filters = json_array();
    if (!tag_filters)
        return NULL;

    /* Build and append one filter per tag */
    for (i = 0; i < tags->n_tags; i++) {
        json_t *tag_flt;
        json_t *xjson;

        tag_flt = json_object();
        if (!tag_flt)
            GOTO(out, -ENOMEM);

        xjson = json_object();
        if (!xjson) {
            json_decref(tag_flt);
            GOTO(out, -ENOMEM);
        }

        if (json_object_set_new(tag_flt, "DSS::MDA::tags",
                                json_string(tags->tags[i]))) {
            json_decref(tag_flt);
            json_decref(xjson);
            GOTO(out, -ENOMEM);
        }

        if (json_object_set_new(xjson, "$XJSON", tag_flt)) {
            json_decref(xjson);
            GOTO(out, -ENOMEM);
        }

        if (json_array_append_new(tag_filters, xjson))
            GOTO(out, -ENOMEM);
    }

    and_filter = json_object();
    if (!and_filter)
        GOTO(out, -ENOMEM);

    /* Do not use the _new function and decref inconditionnaly later */
    if (json_object_set(and_filter, "$AND", tag_filters))
        GOTO(out, -ENOMEM);

    /* Convert to string for formatting */
    tag_filter_json = json_dumps(tag_filters, 0);

out:
    json_decref(tag_filters);

    /* json_decref(NULL) is safe but not documented */
    if (and_filter)
        json_decref(and_filter);

    return tag_filter_json;
}

/**
 * Get a suitable media for a write operation.
 */
static int lrs_select_media(struct lrs *lrs, struct media_info **p_media,
                            size_t required_size, enum dev_family family,
                            const struct tags *tags)
{
    struct media_info   *pmedia_res = NULL;
    struct media_info   *media_best;
    struct dss_filter    filter;
    char                *tag_filter_json = NULL;
    bool                 with_tags = tags != NULL && tags->n_tags > 0;
    bool                 avail_media = false;
    int                  mcnt = 0;
    int                  rc;
    int                  i;
    ENTRY;

    if (with_tags) {
        tag_filter_json = build_tag_filter(tags);
        if (!tag_filter_json)
            LOG_GOTO(err_nores, rc = -ENOMEM, "while building tags dss filter");
    }

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          /* Basic criteria */
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          /* Exclude media locked by admin */
                          "  {\"DSS::MDA::adm_status\": \"%s\"},"
                          "  {\"$GTE\": {\"DSS::MDA::vol_free\": %zu}},"
                          "  {\"$NOR\": ["
                               /* Exclude non-formatted media */
                          "    {\"DSS::MDA::fs_status\": \"%s\"},"
                               /* Exclude full media */
                          "    {\"DSS::MDA::fs_status\": \"%s\"}"
                          "  ]}"
                          "  %s%s"
                          "]}",
                          dev_family2str(family),
                          media_adm_status2str(PHO_MDA_ADM_ST_UNLOCKED),
                          required_size, fs_status2str(PHO_FS_STATUS_BLANK),
                          fs_status2str(PHO_FS_STATUS_FULL),
                          with_tags ? ", " : "",
                          with_tags ? tag_filter_json : "");

    free(tag_filter_json);
    if (rc)
        return rc;

    rc = dss_media_get(lrs->dss, &filter, &pmedia_res, &mcnt);
    if (rc)
        GOTO(err_nores, rc);

lock_race_retry:
    media_best = NULL;

    /* get the best fit */
    for (i = 0; i < mcnt; i++) {
        struct media_info *curr = &pmedia_res[i];

        if (curr->stats.phys_spc_free < required_size)
            continue;

        if (media_best == NULL ||
                curr->stats.phys_spc_free < media_best->stats.phys_spc_free) {
            /* Remember that at least one fitting media has been found */
            avail_media = true;

            /* The media is already locked, continue searching */
            if (curr->lock.lock == LRS_MEDIA_LOCKED_EXTERNAL)
                continue;

            media_best = curr;
        }
    }

    if (media_best == NULL) {
        pho_info("No compatible media found to write %zu bytes", required_size);
        /* If we found a matching media but it was locked, return EAGAIN,
         * otherwise return ENOSPC
         */
        if (avail_media)
            GOTO(free_res, rc = -EAGAIN);
        else
            GOTO(free_res, rc = -ENOSPC);
    }

    pho_debug("Acquiring selected media '%s'", media_id_get(&media_best->id));
    rc = lrs_media_acquire(lrs, media_best);
    if (rc) {
        pho_debug("Failed to lock media '%s', looking for another one",
                  media_id_get(&media_best->id));
        media_best->lock.lock = LRS_MEDIA_LOCKED_EXTERNAL;
        goto lock_race_retry;
    }

    pho_verb("Selected %s '%s': %zd bytes free", dev_family2str(family),
             media_id_get(&media_best->id), media_best->stats.phys_spc_free);

    *p_media = media_info_dup(media_best);
    if (*p_media == NULL)
        GOTO(free_res, rc = -ENOMEM);

    rc = 0;

free_res:
    if (rc != 0)
        lrs_media_release(lrs, media_best);

    dss_res_free(pmedia_res, mcnt);

err_nores:
    dss_filter_free(&filter);
    return rc;
}

/**
 * Get the value of the configuration parameter that contains
 * the list of drive models for a given drive type.
 * e.g. "LTO6_drive" -> "ULTRIUM-TD6,ULT3580-TD6,..."
 *
 * @return 0 on success, a negative POSIX error code on failure.
 */
static int drive_models_by_type(const char *drive_type, const char **list)
{
    char *section_name;
    int rc;

    /* build drive_type section name */
    rc = asprintf(&section_name, DRIVE_TYPE_SECTION_CFG,
                  drive_type);
    if (rc < 0)
        return -ENOMEM;

    /* get list of drive models */
    rc = pho_cfg_get_val(section_name, MODELS_CFG_PARAM, list);
    if (rc)
        pho_error(rc, "Unable to find parameter "MODELS_CFG_PARAM" in section "
                  "'%s' for drive type '%s'", section_name, drive_type);

    free(section_name);
    return rc;
}

/**
 * Get the value of the configuration parameter that contains
 * the list of write-compatible drives for a given tape model.
 * e.g. "LTO5" -> "LTO5_drive,LTO6_drive"
 *
 * @return 0 on success, a negative POSIX error code on failure.
 */
static int rw_drive_types_for_tape(const char *tape_model, const char **list)
{
    char *section_name;
    int rc;

    /* build tape_type section name */
    rc = asprintf(&section_name, TAPE_TYPE_SECTION_CFG, tape_model);
    if (rc < 0)
        return -ENOMEM;

    /* get list of drive_rw types */
    rc = pho_cfg_get_val(section_name, DRIVE_RW_CFG_PARAM, list);
    if (rc)
        pho_error(rc, "Unable to find parameter "DRIVE_RW_CFG_PARAM
                  " in section '%s' for tape model '%s'",
                  section_name, tape_model);

    free(section_name);
    return rc;
}

/**
 * Search a given item in a coma-separated list.
 *
 * @param[in]  list     Comma-separated list of items.
 * @param[in]  str      Item to find in the list.
 * @param[out] res      true of the string is found, false else.
 *
 * @return 0 on success. A negative POSIX error code on error.
 */
static int search_in_list(const char *list, const char *str, bool *res)
{
    char *parse_list;
    char *item;
    char *saveptr;

    *res = false;

    /* copy input list to parse it */
    parse_list = strdup(list);
    if (parse_list == NULL)
        return -errno;

    /* check if the string is in the list */
    for (item = strtok_r(parse_list, ",", &saveptr);
         item != NULL;
         item = strtok_r(NULL, ",", &saveptr)) {
        if (strcmp(item, str) == 0) {
            *res = true;
            goto out_free;
        }
    }

out_free:
    free(parse_list);
    return 0;
}

/**
 * This function determines if the input drive and tape are compatible.
 *
 * @param[in]  tape  tape to check compatibility
 * @param[in]  drive drive to check compatibility
 * @param[out] res   true if the tape and drive are compatible, else false
 *
 * @return 0 on success, negative error code on failure and res is false
 */
static int tape_drive_compat(const struct media_info *tape,
                             const struct dev_descr *drive, bool *res)
{
    const char *rw_drives;
    char *parse_rw_drives;
    char *drive_type;
    char *saveptr;
    int rc;

    /* false by default */
    *res = false;

    /** XXX FIXME: this function is called for each drive for the same tape by
     *  the function dev_picker. Each time, we build/allocate same strings and
     *  we parse again the conf. This behaviour is heavy and not optimal.
     */
    rc = rw_drive_types_for_tape(tape->model, &rw_drives);
    if (rc)
        return rc;

    /* copy the rw_drives list to tokenize it */
    parse_rw_drives = strdup(rw_drives);
    if (parse_rw_drives == NULL)
        return -errno;

    /* For each compatible drive type, get list of associated drive models
     * and search the current drive model in it.
     */
    for (drive_type = strtok_r(parse_rw_drives, ",", &saveptr);
         drive_type != NULL;
         drive_type = strtok_r(NULL, ",", &saveptr)) {
        const char *drive_model_list;

        rc = drive_models_by_type(drive_type, &drive_model_list);
        if (rc)
            goto out_free;

        rc = search_in_list(drive_model_list, drive->dss_dev_info->model, res);
        if (rc)
            goto out_free;
        /* drive model found: media is compatible */
        if (*res)
            break;
    }

out_free:
    free(parse_rw_drives);
    return rc;
}


/**
 * Device selection policy prototype.
 * @param[in]     required_size required space to perform the write operation.
 * @param[in]     dev_curr      the current device to consider.
 * @param[in,out] dev_selected  the currently selected device.
 * @retval <0 on error
 * @retval 0 to stop searching for a device
 * @retval >0 to check next devices.
 */
typedef int (*device_select_func_t)(size_t required_size,
                                    struct dev_descr *dev_curr,
                                    struct dev_descr **dev_selected);

/**
 * Select a device according to a given status and policy function.
 * Returns a device in locked state; if a media is in the device, the media is
 * locked first.
 * @param dss     DSS handle.
 * @param op_st   Filter devices by the given operational status.
 *                No filtering is op_st is PHO_DEV_OP_ST_UNSPEC.
 * @param select_func    Drive selection function.
 * @param required_size  Required size for the operation.
 * @param media_tags     Mandatory tags for the contained media (for write
 *                       requests only).
 * @param pmedia         Media that should be used by the drive to check
 *                       compatibility (ignored if NULL)
 */
static struct dev_descr *dev_picker(struct lrs *lrs,
                                    enum dev_op_status op_st,
                                    device_select_func_t select_func,
                                    size_t required_size,
                                    const struct tags *media_tags,
                                    struct media_info *pmedia)
{
    struct dev_descr    *selected = NULL;
    int                  i;
    int                  rc;
    /*
     * lazily allocated array to remember which devices were unsuccessfully
     * acquired
     */
    bool                *failed_dev = NULL;
    ENTRY;

    if (lrs->devices == NULL)
        return NULL;

retry:
    for (i = 0; i < lrs->dev_count; i++) {
        struct dev_descr    *itr = &lrs->devices[i];

        /* Already unsuccessfully tried to acquire this device */
        if (failed_dev && failed_dev[i])
            continue;

        if (!dev_is_available(itr)) {
            pho_debug("Skipping locked or busy device '%s'", itr->dev_path);
            continue;
        }

        if (op_st != PHO_DEV_OP_ST_UNSPEC && itr->op_status != op_st) {
            pho_debug("Skipping device '%s' with incompatible status %s",
                      itr->dev_path, op_status2str(itr->op_status));
            continue;
        }

        /*
         * The intent is to write: exclude medias that are full or do not have
         * the requested tags
         */
        if (required_size > 0 && itr->dss_media_info) {
            if (itr->dss_media_info->fs.status == PHO_FS_STATUS_FULL) {
                pho_debug("Media '%s' is full",
                          media_id_get(&itr->dss_media_info->id));
                continue;
            }
            if (!tags_in(&itr->dss_media_info->tags, media_tags)) {
                pho_debug("Media '%s' does not match required tags",
                          media_id_get(&itr->dss_media_info->id));
                continue;
            }
        }

        /* check tape / drive compat */
        if (pmedia) {
            bool res;

            if (tape_drive_compat(pmedia, itr, &res)) {
                selected = NULL;
                break;
            }

            if (!res)
                continue;
        }

        rc = select_func(required_size, itr, &selected);
        if (rc < 0) {
            pho_debug("Device selection function failed");
            selected = NULL;
            break;
        } else if (rc == 0) /* stop searching */
            break;
    }

    if (selected != NULL) {
        int selected_i = selected - lrs->devices;
        struct media_info *local_pmedia = selected->dss_media_info;

        pho_debug("Picked dev number %d (%s)", selected_i, selected->dev_path);

        rc = 0;
        if (local_pmedia != NULL) {
            pho_debug("Acquiring %s media '%s'",
                      op_status2str(selected->op_status),
                      media_id_get(&local_pmedia->id));
            rc = lrs_media_acquire(lrs, local_pmedia);
            if (rc)
                /* Avoid releasing a media that has not been acquired */
                local_pmedia = NULL;
        }

        /* Potential media locking suceeded (or no media was loaded): acquire
         * device
         */
        if (rc == 0)
            rc = lrs_dev_acquire(lrs, selected);

        /* Something went wrong */
        if (rc != 0) {
            /* Release media if necessary */
            lrs_media_release(lrs, local_pmedia);
            /* clear previously selected device */
            selected = NULL;
            /* Allocate failed_dev if necessary */
            if (failed_dev == NULL) {
                failed_dev = calloc(lrs->dev_count, sizeof(bool));
                if (failed_dev == NULL)
                    return NULL;
            }
            /* Locally mark this device as failed */
            failed_dev[selected_i] = true;
            /* resume loop where it was */
            goto retry;
        }
    } else
        pho_debug("Could not find a suitable %s device", op_status2str(op_st));

    free(failed_dev);
    return selected;
}

/**
 * Get the first device with enough space.
 * @retval 0 to stop searching for a device
 * @retval 1 to check next device.
 */
static int select_first_fit(size_t required_size,
                            struct dev_descr *dev_curr,
                            struct dev_descr **dev_selected)
{
    ENTRY;

    if (dev_curr->dss_media_info == NULL)
        return 1;

    if (dev_curr->dss_media_info->stats.phys_spc_free >= required_size) {
        *dev_selected = dev_curr;
        return 0;
    }
    return 1;
}

/**
 *  Get the device with the lower space to match required_size.
 * @return 1 to check next devices, unless an exact match is found (return 0).
 */
static int select_best_fit(size_t required_size,
                           struct dev_descr *dev_curr,
                           struct dev_descr **dev_selected)
{
    ENTRY;

    if (dev_curr->dss_media_info == NULL)
        return 1;

    /* does it fit? */
    if (dev_curr->dss_media_info->stats.phys_spc_free < required_size)
        return 1;

    /* no previous fit, or better fit */
    if (*dev_selected == NULL || (dev_curr->dss_media_info->stats.phys_spc_free
                      < (*dev_selected)->dss_media_info->stats.phys_spc_free)) {
        *dev_selected = dev_curr;

        if (required_size == dev_curr->dss_media_info->stats.phys_spc_free)
            /* exact match, stop searching */
            return 0;
    }
    return 1;
}

/**
 * Select any device without checking media or available size.
 * @return 0 on first device found, 1 else (to continue searching).
 */
static int select_any(size_t required_size,
                      struct dev_descr *dev_curr,
                      struct dev_descr **dev_selected)
{
    ENTRY;

    if (*dev_selected == NULL) {
        *dev_selected = dev_curr;
        /* found an item, stop searching */
        return 0;
    }
    return 1;
}

/* Get the device with the least space available on the loaded media.
 * If a tape is loaded, it just needs to be unloaded.
 * If the filesystem is mounted, umount is needed before unloading.
 * @return 1 (always check all devices).
 */
static int select_drive_to_free(size_t required_size,
                                struct dev_descr *dev_curr,
                                struct dev_descr **dev_selected)
{
    ENTRY;

    /* skip failed and locked drives */
    if (dev_curr->op_status == PHO_DEV_OP_ST_FAILED
            || !dev_is_available(dev_curr)) {
        pho_debug("Skipping drive '%s' with status %s%s",
                  dev_curr->dev_path, op_status2str(dev_curr->op_status),
                  !dev_is_available(dev_curr) ? " (locked or busy)" : "");
        return 1;
    }

    /* if this function is called, no drive should be empty */
    if (dev_curr->op_status == PHO_DEV_OP_ST_EMPTY) {
        pho_warn("Unexpected drive status for '%s': '%s'",
                 dev_curr->dev_path, op_status2str(dev_curr->op_status));
        return 1;
    }

    /* less space available on this device than the previous ones? */
    if (*dev_selected == NULL || dev_curr->dss_media_info->stats.phys_spc_free
                    < (*dev_selected)->dss_media_info->stats.phys_spc_free) {
        *dev_selected = dev_curr;
        return 1;
    }

    return 1;
}

/** Mount the filesystem of a ready device */
static int lrs_mount(struct dev_descr *dev)
{
    char                *mnt_root;
    struct fs_adapter    fsa;
    const char          *id;
    int                  rc;
    ENTRY;

    rc = get_fs_adapter(dev->dss_media_info->fs.type, &fsa);
    if (rc)
        goto out_err;

    rc = ldm_fs_mounted(&fsa, dev->dev_path, dev->mnt_path,
                        sizeof(dev->mnt_path));
    if (rc == 0) {
        dev->op_status = PHO_DEV_OP_ST_MOUNTED;
        return 0;
    }

#if 0
    /**
     * @TODO If library indicates a media is in the drive but the drive
     * doesn't, we need to query the drive to load the tape.
     */
    if (devd->lib_dev_info->ldi_full && !devd->lds_loaded) {
        pho_info("Tape '%s' is located in drive '%s' but is not online: "
                 "querying the drive to load it...",
                 media_id_get(&devd->ldi_media_id), devi->serial);
        rc = ldm_dev_load(&deva, devd->dev_path);
        if (rc)
            LOG_RETURN(rc, "Failed to load tape in drive '%s'",
                       devi->serial);
#endif

    id = basename(dev->dev_path);
    if (id == NULL)
        return -EINVAL;

    /* mount the device as PHO_MNT_PREFIX<id> */
    mnt_root = mount_point(id);
    if (!mnt_root)
        return -ENOMEM;

    pho_verb("Mounting device '%s' as '%s'", dev->dev_path, mnt_root);

    rc = ldm_fs_mount(&fsa, dev->dev_path, mnt_root,
                      dev->dss_media_info->fs.label);
    if (rc)
        LOG_GOTO(out_free, rc, "Failed to mount device '%s'",
                 dev->dev_path);

    /* update device state and set mount point */
    dev->op_status = PHO_DEV_OP_ST_MOUNTED;
    strncpy(dev->mnt_path,  mnt_root, sizeof(dev->mnt_path));

out_free:
    free(mnt_root);
out_err:
    if (rc != 0)
        dev->op_status = PHO_DEV_OP_ST_FAILED;
    return rc;
}

/** Unmount the filesystem of a 'mounted' device */
static int lrs_umount(struct dev_descr *dev)
{
    struct fs_adapter  fsa;
    int                rc;
    ENTRY;

    if (dev->op_status != PHO_DEV_OP_ST_MOUNTED)
        LOG_RETURN(-EINVAL, "Unexpected drive status for '%s': '%s'",
                   dev->dev_path, op_status2str(dev->op_status));

    if (dev->mnt_path[0] == '\0')
        LOG_RETURN(-EINVAL, "No mount point for mounted device '%s'?!",
                   dev->dev_path);

    if (dev->dss_media_info == NULL)
        LOG_RETURN(-EINVAL, "No media in mounted device '%s'?!",
                   dev->dev_path);

    pho_verb("Unmounting device '%s' mounted as '%s'",
             dev->dev_path, dev->mnt_path);

    rc = get_fs_adapter(dev->dss_media_info->fs.type, &fsa);
    if (rc)
        return rc;

    rc = ldm_fs_umount(&fsa, dev->dev_path, dev->mnt_path);
    if (rc)
        LOG_RETURN(rc, "Failed to umount device '%s' mounted as '%s'",
                   dev->dev_path, dev->mnt_path);

    /* update device state and unset mount path */
    dev->op_status = PHO_DEV_OP_ST_LOADED;
    dev->mnt_path[0] = '\0';

    return 0;
}

/**
 * Load a media into a drive.
 *
 * @return 0 on success, -error number on error. -EBUSY is returned when a drive
 * to drive media movement was prevented by the library.
 */
static int lrs_load(struct dev_descr *dev, struct media_info *media)
{
    struct lib_item_addr media_addr;
    struct lib_adapter   lib;
    int                  rc;
    int                  rc2;
    ENTRY;

    if (dev->op_status != PHO_DEV_OP_ST_EMPTY)
        LOG_RETURN(-EAGAIN, "%s: unexpected drive status: status='%s'",
                   dev->dev_path, op_status2str(dev->op_status));

    if (dev->dss_media_info != NULL)
        LOG_RETURN(-EAGAIN, "No media expected in device '%s' (found '%s')",
                   dev->dev_path, media_id_get(&dev->dss_media_info->id));

    pho_verb("Loading '%s' into '%s'", media_id_get(&media->id),
             dev->dev_path);

    /* get handle to the library depending on device type */
    rc = wrap_lib_open(dev->dss_dev_info->family, &lib);
    if (rc)
        return rc;

    /* lookup the requested media */
    rc = ldm_lib_media_lookup(&lib, media_id_get(&media->id),
                              &media_addr);
    if (rc)
        LOG_GOTO(out_close, rc, "Media lookup failed");

    rc = ldm_lib_media_move(&lib, &media_addr, &dev->lib_dev_info.ldi_addr);
    /* A movement from drive to drive can be prohibited by some libraries.
     * If a failure is encountered in such a situation, it probably means that
     * the state of the library has changed between the moment it has been
     * scanned and the moment the media and drive have been selected. The
     * easiest solution is therefore to return EBUSY to signal this situation to
     * the caller.
     */
    if (rc == -EINVAL
            && media_addr.lia_type == MED_LOC_DRIVE
            && dev->lib_dev_info.ldi_addr.lia_type == MED_LOC_DRIVE) {
        pho_debug("Failed to move a media from one drive to another, trying "
                  "again later");
        /* @TODO: acquire source drive on the fly? */
        return -EBUSY;
    } else if (rc != 0) {
        /* Set operationnal failure state on this drive. It is incomplete since
         * the error can originate from a defect tape too...
         *  - consider marking both as failed.
         *  - consider maintaining lists of errors to diagnose and decide who to
         *    exclude from the cool game. */
        dev->op_status = PHO_DEV_OP_ST_FAILED;
        LOG_GOTO(out_close, rc, "Media move failed");
    }

    /* update device status */
    dev->op_status = PHO_DEV_OP_ST_LOADED;
    /* associate media to this device */
    dev->dss_media_info = media;
    rc = 0;

out_close:
    rc2 = ldm_lib_close(&lib);
    return rc ? rc : rc2;
}

/**
 * Unload a media from a drive and unlock the media.
 */
static int lrs_unload(struct lrs *lrs, struct dev_descr *dev)
{
    /* let the library select the target location */
    struct lib_item_addr    free_slot = { .lia_type = MED_LOC_UNKNOWN };
    struct lib_adapter      lib;
    int                     rc;
    int                     rc2;
    ENTRY;

    if (dev->op_status != PHO_DEV_OP_ST_LOADED)
        LOG_RETURN(-EINVAL, "Unexpected drive status for '%s': '%s'",
                   dev->dev_path, op_status2str(dev->op_status));

    if (dev->dss_media_info == NULL)
        LOG_RETURN(-EINVAL, "No media in loaded device '%s'?!",
                   dev->dev_path);

    pho_verb("Unloading '%s' from '%s'", media_id_get(&dev->dss_media_info->id),
             dev->dev_path);

    /* get handle to the library, depending on device type */
    rc = wrap_lib_open(dev->dss_dev_info->family, &lib);
    if (rc)
        return rc;

    rc = ldm_lib_media_move(&lib, &dev->lib_dev_info.ldi_addr, &free_slot);
    if (rc != 0) {
        /* Set operationnal failure state on this drive. It is incomplete since
         * the error can originate from a defect tape too...
         *  - consider marking both as failed.
         *  - consider maintaining lists of errors to diagnose and decide who to
         *    exclude from the cool game. */
        dev->op_status = PHO_DEV_OP_ST_FAILED;
        LOG_GOTO(out_close, rc, "Media move failed");
    }

    /* update device status */
    dev->op_status = PHO_DEV_OP_ST_EMPTY;

    /* Locked by caller, by convention */
    lrs_media_release(lrs, dev->dss_media_info);

    /* free media resources */
    media_info_free(dev->dss_media_info);
    dev->dss_media_info = NULL;
    rc = 0;

out_close:
    rc2 = ldm_lib_close(&lib);
    return rc ? rc : rc2;
}

/** return the device policy function depending on configuration */
static device_select_func_t get_dev_policy(void)
{
    const char *policy_str;
    ENTRY;

    policy_str = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, policy);
    if (policy_str == NULL)
        return NULL;

    if (!strcmp(policy_str, "best_fit"))
        return select_best_fit;

    if (!strcmp(policy_str, "first_fit"))
        return select_first_fit;

    pho_error(EINVAL, "Invalid LRS policy name '%s' "
              "(expected: 'best_fit' or 'first_fit')", policy_str);

    return NULL;
}

/**
 * Return true if at least one compatible drive is found.
 *
 * The found compatible drive should be not failed and not locked by
 * administrator.
 *
 * @param(in) pmedia   Media that should be used by the drive to check
 *                     compatibility (ignored if NULL, any not failed and not
 *                     administrator locked drive will fit.)
 * @return             True if one compatible drive is found, else false.
 */
static bool compatible_drive_exists(struct lrs *lrs, struct media_info *pmedia)
{
    int i;

    for (i = 0; i < lrs->dev_count; i++) {
        if (lrs->devices[i].op_status == PHO_DEV_OP_ST_FAILED)
            continue;

        if (pmedia) {
            bool is_compat;

            if (tape_drive_compat(pmedia, &(lrs->devices[i]), &is_compat))
                continue;

            if (is_compat)
                return true;
        }
    }

    return false;
}
/**
 * Free one of the devices to allow mounting a new media.
 * On success, the returned device is locked.
 * @param(in)  dss       Handle to DSS.
 * @param(out) dev_descr Pointer to an empty drive.
 * @param(in)  pmedia    Media that should be used by the drive to check
 *                       compatibility (ignored if NULL)
 */
static int lrs_free_one_device(struct lrs *lrs, struct dev_descr **devp,
                               struct media_info *pmedia)
{
    struct dev_descr *tmp_dev;
    int               rc;
    ENTRY;

    while (1) {

        /* get a drive to free (PHO_DEV_OP_ST_UNSPEC for any state) */
        tmp_dev = dev_picker(lrs, PHO_DEV_OP_ST_UNSPEC, select_drive_to_free,
                             0, &NO_TAGS, pmedia);
        if (tmp_dev == NULL) {
            if (compatible_drive_exists(lrs, pmedia))
                LOG_RETURN(-EAGAIN, "No suitable device to free");
            else
                LOG_RETURN(-ENODEV, "No compatible device exists not failed "
                                    "and not locked by admin");
        }

        if (tmp_dev->op_status == PHO_DEV_OP_ST_MOUNTED) {
            /* unmount it */
            rc = lrs_umount(tmp_dev);
            if (rc) {
                /* set it failed and get another device */
                tmp_dev->op_status = PHO_DEV_OP_ST_FAILED;
                goto next;
            }
        }

        if (tmp_dev->op_status == PHO_DEV_OP_ST_LOADED) {
            /* unload the media */
            rc = lrs_unload(lrs, tmp_dev);
            if (rc) {
                /* set it failed and get another device */
                tmp_dev->op_status = PHO_DEV_OP_ST_FAILED;
                goto next;
            }
        }

        if (tmp_dev->op_status != PHO_DEV_OP_ST_EMPTY)
            LOG_RETURN(-EINVAL,
                       "Unexpected dev status '%s' for '%s': should be empty",
                       op_status2str(tmp_dev->op_status), tmp_dev->dev_path);

        /* success: we've got an empty device */
        *devp = tmp_dev;
        return 0;

next:
        lrs_dev_release(lrs, tmp_dev);
        lrs_media_release(lrs, tmp_dev->dss_media_info);
    }
}

/**
 * Get a prepared device to perform a write operation.
 * @param[in]  size  Size of the extent to be written.
 * @param[in]  tags  Tags used to filter candidate media, the selected media
 *                   must have all the specified tags.
 * @param[out] devp  The selected device to write with.
 */
static int lrs_get_write_res(struct lrs *lrs, size_t size,
                             const struct tags *tags, struct dev_descr **devp)
{
    device_select_func_t dev_select_policy;
    struct media_info *pmedia;
    bool media_owner;
    int rc;
    ENTRY;

    rc = lrs_load_dev_state(lrs);
    if (rc != 0)
        return rc;

    dev_select_policy = get_dev_policy();
    if (!dev_select_policy)
        return -EINVAL;

    pmedia = NULL;
    media_owner = false;

    /* 1a) is there a mounted filesystem with enough room? */
    *devp = dev_picker(lrs, PHO_DEV_OP_ST_MOUNTED, dev_select_policy, size,
                       tags, NULL);
    if (*devp != NULL)
        return 0;

    /* 1b) is there a loaded media with enough room? */
    *devp = dev_picker(lrs, PHO_DEV_OP_ST_LOADED, dev_select_policy, size,
                       tags, NULL);
    if (*devp != NULL) {
        /* mount the filesystem and return */
        rc = lrs_mount(*devp);
        if (rc != 0)
            goto out_release;
        return 0;
    }

    /* V1: release a drive and load a tape with enough room.
     * later versions:
     * 2a) is there an idle drive, to eject the loaded tape?
     * 2b) is there an operation that will end soon?
     */

    /* 2) For the next steps, we need a media to write on.
     * It will be loaded into a free drive.
     * Note: lrs_select_media locks the media.
     */
    pho_verb("Not enough space on loaded media: selecting another one");
    rc = lrs_select_media(lrs, &pmedia, size, default_family(), tags);
    if (rc)
        return rc;
    /* we own the media structure */
    media_owner = true;

    /* Check if the media is already in a drive and try to acquire it. This
     * should never fail because media are locked before drives and a drive
     * shall never been locked if the media in it has not previously been
     * locked.
     */
    *devp = search_loaded_media(lrs, &pmedia->id);
    if (*devp != NULL) {
        rc = lrs_dev_acquire(lrs, *devp);
        if (rc != 0)
            GOTO(out_release, rc = -EAGAIN);
        /* Media is in dev, update dev->dss_media_info with fresh media info */
        media_info_free((*devp)->dss_media_info);
        (*devp)->dss_media_info = pmedia;
        return 0;
    }

    /* 3) is there a free drive? */
    *devp = dev_picker(lrs, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                       pmedia);
    if (*devp == NULL) {
        pho_verb("No free drive: need to unload one");
        rc = lrs_free_one_device(lrs, devp, pmedia);
        if (rc)
            goto out_release;
    }

    /* 4) load the selected media into the selected drive */
    rc = lrs_load(*devp, pmedia);
    /* EBUSY means the tape could not be moved between two drives, try again
     * later
     */
    if (rc == -EBUSY)
        GOTO(out_release, rc = -EAGAIN);
    else if (rc)
        goto out_release;

    /* On success or lrs_load, target device becomes the owner of pmedia
     * so pmedia must not be released after that. */
    media_owner = false;

    /* 5) mount the filesystem */
    rc = lrs_mount(*devp);
    if (rc == 0)
        return 0;

out_release:
    if (*devp != NULL) {
        lrs_dev_release(lrs, *devp);
        /* Avoid releasing the same media twice */
        if (pmedia != (*devp)->dss_media_info)
            lrs_media_release(lrs, (*devp)->dss_media_info);
    }

    if (pmedia != NULL) {
        pho_debug("Releasing selected media '%s'", media_id_get(&pmedia->id));
        lrs_media_release(lrs, pmedia);
        if (media_owner)
            media_info_free(pmedia);
    }
    return rc;
}

/** set location structure from device information */
static int set_loc_from_dev(const struct dev_descr *dev,
                            struct lrs_intent *intent)
{
    ENTRY;

    if (dev == NULL || dev->mnt_path == NULL)
        return -EINVAL;

    /* fill intent descriptor with mount point and media info */
    intent->li_location.root_path        = strdup(dev->mnt_path);
    intent->li_location.extent.media     = dev->dss_media_info->id;
    intent->li_location.extent.fs_type   = dev->dss_media_info->fs.type;
    intent->li_location.extent.addr_type = dev->dss_media_info->addr_type;
    intent->li_location.extent.address   = PHO_BUFF_NULL;
    return 0;
}

static struct dev_descr *search_loaded_media(struct lrs *lrs,
                                             const struct media_id *id)
{
    const char *name;
    int         i;
    ENTRY;

    if (id == NULL)
        return NULL;

    name = media_id_get(id);

    for (i = 0; i < lrs->dev_count; i++) {
        const char          *media_id;
        enum dev_op_status   op_st = lrs->devices[i].op_status;

        if (op_st != PHO_DEV_OP_ST_MOUNTED && op_st != PHO_DEV_OP_ST_LOADED)
            continue;

        media_id = media_id_get(&lrs->devices[i].media_id);
        if (media_id == NULL) {
            pho_warn("Cannot retrieve media ID from device '%s'",
                     lrs->devices[i].dev_path);
            continue;
        }

        if (!strcmp(name, media_id))
            return &lrs->devices[i];
    }
    return NULL;
}

static int lrs_media_prepare(struct lrs *lrs, const struct media_id *id,
                             enum lrs_operation op, struct dev_descr **pdev,
                             struct media_info **pmedia)
{
    const char          *label = media_id_get(id);
    struct dev_descr    *dev;
    struct media_info   *med = NULL;
    bool                 post_fs_mount;
    int                  rc;
    ENTRY;

    *pdev = NULL;
    *pmedia = NULL;

    rc = lrs_fill_media_info(lrs->dss, &med, id);
    if (rc != 0)
        return rc;

    /* Check that the media is not already locked */
    if (&med->lock.lock == LRS_MEDIA_LOCKED_EXTERNAL) {
        pho_debug("Media '%s' is locked, returning EAGAIN", label);
        GOTO(out, rc = -EAGAIN);
    }

    switch (op) {
    case LRS_OP_READ:
    case LRS_OP_WRITE:
        if (med->fs.status == PHO_FS_STATUS_BLANK)
            LOG_RETURN(-EINVAL, "Cannot do I/O on unformatted media '%s'",
                       label);
        post_fs_mount = true;
        break;
    case LRS_OP_FORMAT:
        if (med->fs.status != PHO_FS_STATUS_BLANK)
            LOG_RETURN(-EINVAL, "Cannot format non-blank media '%s'", label);
        post_fs_mount = false;
        break;
    default:
        LOG_RETURN(-ENOSYS, "Unknown operation %x", (int)op);
    }

    rc = lrs_media_acquire(lrs, med);
    if (rc != 0)
        GOTO(out, rc = -EAGAIN);

    /* check if the media is already in a drive */
    dev = search_loaded_media(lrs, id);
    if (dev != NULL) {
        rc = lrs_dev_acquire(lrs, dev);
        if (rc != 0)
            GOTO(out_mda_unlock, rc = -EAGAIN);
        /* Media is in dev, update dev->dss_media_info with fresh media info */
        media_info_free(dev->dss_media_info);
        dev->dss_media_info = med;
    } else {
        pho_verb("Media '%s' is not in a drive", media_id_get(id));

        /* Is there a free drive? */
        dev = dev_picker(lrs, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                         med);
        if (dev == NULL) {
            pho_verb("No free drive: need to unload one");
            rc = lrs_free_one_device(lrs, &dev, med);
            if (rc != 0)
                LOG_GOTO(out_mda_unlock, rc, "No device available");
        }

        /* load the media in it */
        rc = lrs_load(dev, med);
        /* EBUSY means the tape could not be moved between two drives, try again
         * later
         */
        if (rc == -EBUSY)
            GOTO(out_dev_unlock, rc = -EAGAIN);
        else if (rc != 0)
            goto out_dev_unlock;
    }

    /* Mount only for READ/WRITE and if not already mounted */
    if (post_fs_mount && dev->op_status != PHO_DEV_OP_ST_MOUNTED) {
        rc = lrs_mount(dev);
    }

out_dev_unlock:
    if (rc)
        lrs_dev_release(lrs, dev);

out_mda_unlock:
    if (rc)
        lrs_media_release(lrs, med);

out:
    if (rc) {
        media_info_free(med);
        *pmedia = NULL;
        *pdev = NULL;
    } else {
        *pmedia = med;
        *pdev = dev;
    }
    return rc;
}


/* see "pho_lrs.h" for function help */

int lrs_format(struct lrs *lrs, const struct media_id *id, enum fs_type fs,
               bool unlock)
{
    const char          *label = media_id_get(id);
    struct dev_descr    *dev = NULL;
    struct media_info   *media_info = NULL;
    int                  rc;
    int                  rc2;
    struct ldm_fs_space  spc = {0};
    struct fs_adapter    fsa;
    ENTRY;

    rc = lrs_load_dev_state(lrs);
    if (rc != 0)
        return rc;

    rc = lrs_media_prepare(lrs, id, LRS_OP_FORMAT, &dev, &media_info);
    if (rc != 0)
        return rc;

    /* -- from now on, device is owned -- */

    if (dev->dss_media_info == NULL)
        LOG_GOTO(err_out, rc = -EINVAL, "Invalid device state");

    pho_verb("Format media '%s' as %s", label, fs_type2str(fs));

    rc = get_fs_adapter(fs, &fsa);
    if (rc)
        LOG_GOTO(err_out, rc, "Failed to get FS adapter");

    rc = ldm_fs_format(&fsa, dev->dev_path, label, &spc);
    if (rc)
        LOG_GOTO(err_out, rc, "Cannot format media '%s'", label);

    /* Systematically use the media ID as filesystem label */
    strncpy(media_info->fs.label, label, sizeof(media_info->fs.label) - 1);

    media_info->stats.phys_spc_used = spc.spc_used;
    media_info->stats.phys_spc_free = spc.spc_avail;

    /* Post operation: update media information in DSS */
    media_info->fs.status = PHO_FS_STATUS_EMPTY;

    if (unlock) {
        pho_verb("Unlocking media '%s'", label);
        media_info->adm_status = PHO_MDA_ADM_ST_UNLOCKED;
    }

    rc = dss_media_set(lrs->dss, media_info, 1, DSS_SET_UPDATE);
    if (rc != 0)
        LOG_GOTO(err_out, rc, "Failed to update state of media '%s'", label);

err_out:
    /* Release ownership. Do not fail the whole operation if unlucky here... */
    rc2 = lrs_dev_release(lrs, dev);
    if (rc2)
        pho_error(rc2, "Failed to release lock on '%s'", dev->dev_path);

    rc2 = lrs_media_release(lrs, media_info);
    if (rc2)
        pho_error(rc2, "Failed to release lock on '%s'", label);

    /* Don't free media_info since it is still referenced inside dev */
    return rc;
}

static bool lrs_mount_is_writable(const struct lrs_intent *intent)
{
    const char          *fs_root = intent->li_location.root_path;
    enum fs_type         fs_type = intent->li_location.extent.fs_type;
    struct ldm_fs_space  fs_info = {0};
    struct fs_adapter    fsa;
    int                  rc;

    rc = get_fs_adapter(fs_type, &fsa);
    if (rc)
        LOG_RETURN(rc, "No FS adapter found for '%s' (type %s)",
                   fs_root, fs_type2str(fs_type));

    rc = ldm_fs_df(&fsa, fs_root, &fs_info);
    if (rc)
        LOG_RETURN(rc, "Cannot retrieve media usage information");

    return !(fs_info.spc_flags & PHO_FS_READONLY);
}

int lrs_write_prepare(struct lrs *lrs, struct lrs_intent *intent,
                      const struct tags *tags)
{
    struct dev_descr    *dev = NULL;
    struct media_info   *media = NULL;
    size_t               size = intent->li_location.extent.size;
    int                  rc;
    ENTRY;

retry:
    rc = lrs_get_write_res(lrs, size, tags, &dev);
    if (rc != 0)
        return rc;

    intent->li_device = dev;

    rc = set_loc_from_dev(dev, intent);
    if (rc != 0)
        LOG_GOTO(err_cleanup, rc, "Cannot set write location");

    /* a single part with the given size */
    intent->li_location.extent.layout_idx = 0;
    intent->li_location.extent.size = size;

    media = dev->dss_media_info;

    /* LTFS can cunningly mount almost-full tapes as read-only, and so would
     * damaged disks. Mark the media as full and retry when this occurs. */
    if (!lrs_mount_is_writable(intent)) {
        pho_warn("Media '%s' OK but mounted R/O, marking full and retrying...",
                 media_id_get(&media->id));

        media->fs.status = PHO_FS_STATUS_FULL;

        rc = dss_media_set(lrs->dss, media, 1, DSS_SET_UPDATE);
        if (rc)
            LOG_GOTO(err_cleanup, rc, "Cannot update media information");

        lrs_dev_release(lrs, dev);
        lrs_media_release(lrs, media);
        dev = NULL;
        media = NULL;
        goto retry;
    }

    pho_verb("Writing to media '%s' using device '%s' "
             "(free space: %zu bytes)",
             media_id_get(&media->id), dev->dev_path,
             dev->dss_media_info->stats.phys_spc_free);

err_cleanup:
    if (rc != 0) {
        lrs_dev_release(lrs, dev);
        lrs_media_release(lrs, media);
        free(intent->li_location.root_path);
        memset(intent, 0, sizeof(*intent));
    }

    return rc;
}

int lrs_read_prepare(struct lrs *lrs, struct lrs_intent *intent)
{
    struct dev_descr    *dev = NULL;
    struct media_info   *media_info = NULL;
    struct media_id     *id;
    int                  rc;
    ENTRY;

    rc = lrs_load_dev_state(lrs);
    if (rc != 0)
        return rc;

    id = &intent->li_location.extent.media;

    /* Fill in information about media and mount it if needed */
    rc = lrs_media_prepare(lrs, id, LRS_OP_READ, &dev, &media_info);
    if (rc)
        return rc;

    intent->li_device = dev;

    if (dev->dss_media_info == NULL)
        LOG_GOTO(out, rc = -EINVAL, "Invalid device state, expected media '%s'",
                 media_id_get(id));

    /* set fs_type and addr_type according to media description. */
    intent->li_location.root_path        = strdup(dev->mnt_path);
    intent->li_location.extent.fs_type   = dev->dss_media_info->fs.type;
    intent->li_location.extent.addr_type = dev->dss_media_info->addr_type;

out:
    /* Don't free media_info since it is still referenced inside dev */
    return rc;
}

static int lrs_media_update(struct lrs *lrs, struct lrs_intent *intent,
                            int fragments, bool err)
{
    struct media_info   *media = intent->li_device->dss_media_info;
    const char          *fsroot = intent->li_location.root_path;
    struct ldm_fs_space  spc = {0};
    struct fs_adapter    fsa;
    int                  rc;

    rc = get_fs_adapter(intent->li_location.extent.fs_type, &fsa);
    if (rc)
        LOG_RETURN(rc, "No FS adapter found for '%s' (type %s)",
                   fsroot, fs_type2str(intent->li_location.extent.fs_type));

    rc = ldm_fs_df(&fsa, fsroot, &spc);
    if (rc)
        LOG_RETURN(rc, "Cannot retrieve media usage information");

    media->stats.nb_obj += fragments;
    media->stats.phys_spc_used = spc.spc_used;
    media->stats.phys_spc_free = spc.spc_avail;

    if (fragments > 0)
        media->stats.logc_spc_used += intent->li_location.extent.size;

    if (media->fs.status == PHO_FS_STATUS_EMPTY)
        media->fs.status = PHO_FS_STATUS_USED;

    if (err || media->stats.phys_spc_free == 0)
        media->fs.status = PHO_FS_STATUS_FULL;

    /* TODO update nb_load, nb_errors, last_load */

    rc = dss_media_set(lrs->dss, media, 1, DSS_SET_UPDATE);
    if (rc)
        LOG_RETURN(rc, "Cannot update media information");

    return 0;
}

int lrs_io_complete(struct lrs *lrs, struct lrs_intent *intent, int fragments,
                    int err_code)
{
    struct pho_ext_loc  *loc = &intent->li_location;
    struct io_adapter    ioa;
    bool                 is_full = false;
    int                  rc;
    ENTRY;

    rc = get_io_adapter(loc->extent.fs_type, &ioa);
    if (rc)
        LOG_RETURN(rc, "No suitable I/O adapter for filesystem type: '%s'",
                   fs_type2str(loc->extent.fs_type));

    /* Come on, this same IOA has just been used to perform the data transfer */
    assert(io_adapter_is_valid(&ioa));

    rc = ioa_flush(&ioa, loc);
    if (rc)
        LOG_RETURN(rc, "Cannot flush media at: %s", loc->root_path);

    if (is_media_global_error(err_code) || is_media_global_error(rc))
        is_full = true;

    rc = lrs_media_update(lrs, intent, fragments, is_full);
    if (rc)
        LOG_RETURN(rc, "Cannot update media information");

    return 0;
}

int lrs_resource_release(struct lrs *lrs, struct lrs_intent *intent)
{
    ENTRY;

    if (intent->li_device) {
        /* Can't release li_device if lrs->dss is NULL (but it should not be) */
        assert(lrs->dss);
        lrs_dev_release(lrs, intent->li_device);
        lrs_media_release(lrs, intent->li_device->dss_media_info);
        intent->li_device = NULL;
    }

    free(intent->li_location.root_path);
    intent->li_location.root_path = NULL;
    return 0;
}
