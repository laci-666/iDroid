/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DroidNative snapshot: boot Android once, restore it forever after.
 *
 * Uses QEMU's internal snapshot API (migration/snapshot.h) restricted to the
 * userdata block node ("dnvmstate", named on the -drive line): the VM state
 * and the disk snapshot live inside userdata.qcow2. The system image mounts
 * read-only in the guest and the firmware var store is not needed past
 * firmware, so they must NOT take part -- load_snapshot() would otherwise
 * demand the snapshot on every drive, including the pristine system image.
 *
 * save_snapshot()/load_snapshot() stop and resume the vCPUs at the correct
 * points themselves; stopping by hand first would record "stopped" into the
 * saved run state and the restored machine would come back paused.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "block/aio.h"
#include "qapi/error.h"
#include "migration/snapshot.h"
#include "system/runstate.h"

#include "dn-display.h" /* shared app-facing declarations */

#define DN_SNAPSHOT_NAME "dn-ready"
#define DN_VMSTATE_NODE  "dnvmstate"

static strList dn_snapshot_devices = {
    .value = (char *)DN_VMSTATE_NODE,
    .next  = NULL,
};

/* 0 idle, 1 saving, 2 saved ok, 3 failed. Polled from the app. */
static int dn_snapshot_state_v;

static void dn_snapshot_save_bh(void *opaque)
{
    (void)opaque;
    /*
     * Runs as a bottom half on the main AIO context: the BQL is already
     * held here (taking it again aborts in bql_lock_impl), and
     * save_snapshot() handles vm_stop(RUN_STATE_SAVE_VM) internally.
     */
    Error *err = NULL;
    bool ok = save_snapshot(DN_SNAPSHOT_NAME, true, DN_VMSTATE_NODE,
                            true, &dn_snapshot_devices, &err);
    fprintf(stderr, "[dn-snap] save %s%s%s\n", ok ? "OK" : "FAILED",
            err ? ": " : "", err ? error_get_pretty(err) : "");
    fflush(stderr);
    error_free(err);
    qatomic_set(&dn_snapshot_state_v, ok ? 2 : 3);
}

void dnq_snapshot_save(void)
{
    qatomic_set(&dn_snapshot_state_v, 1);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), dn_snapshot_save_bh, NULL);
}

int dnq_snapshot_state(void)
{
    return qatomic_read(&dn_snapshot_state_v);
}

bool dnq_snapshot_load_at_startup(void)
{
    /*
     * Called straight after qemu_init(), which returns with the BQL held by
     * this thread -- no lock to take. The machine is already RUNNING though
     * (qemu_init ends in qmp_x_exit_preconfig -> qmp_cont() unless -S), and
     * load_snapshot() does not defend against running vCPUs, so stop them
     * first, exactly like -loadvm does.
     */
    RunState saved = runstate_get();
    vm_stop(RUN_STATE_RESTORE_VM);

    Error *err = NULL;
    bool ok = load_snapshot(DN_SNAPSHOT_NAME, DN_VMSTATE_NODE,
                            true, &dn_snapshot_devices, &err);
    if (!ok) {
        /* The normal first-run case: no snapshot yet. Restore the previous
         * run state and let the machine boot normally. */
        fprintf(stderr, "[dn-snap] no snapshot restored (%s); booting normally\n",
                err ? error_get_pretty(err) : "none found");
        fflush(stderr);
        error_free(err);
        vm_resume(saved);
        return false;
    }
    fprintf(stderr, "[dn-snap] restored '%s' -- Android is already booted\n",
            DN_SNAPSHOT_NAME);
    fflush(stderr);
    /* The display bridge was registered before load_snapshot(). Re-announce
     * the current EDID to the restored guest (e.g. a pre-0.4.6 snapshot
     * remembered the old 75 Hz mode). Still under the BQL, before vCPUs
     * resume; the UI timer sends one virtio-gpu display-change event. */
    dn_display_reannounce_ui_info();
    load_snapshot_resume(RUN_STATE_RUNNING);
    return true;
}
