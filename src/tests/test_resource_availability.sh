#!/bin/bash
# -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
# vim:expandtab:shiftwidth=4:tabstop=4:

#
#  All rights reserved (c) 2014-2020 CEA/DAM.
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

set -xe

LOG_VALG="$LOG_COMPILER $LOG_FLAGS"

# set python and phobos environment
test_dir=$(dirname $(readlink -e $0))
. $test_dir/test_env.sh
. $test_dir/setup_db.sh
. $test_dir/test_launch_daemon.sh

DRIVE_ARRAY=( /dev/st0 /dev/st1 )
TAPE_ARRAY=( P00000L5 P00001L5 )
DIR_ARRAY=( /tmp/test.pho.1 /tmp/test.pho.2 )

function error
{
    echo "$*"
    exit 1
}

function setup_dir
{
    mkdir ${DIR_ARRAY[@]}
}

function clean_dir
{
    rm -rf ${DIR_ARRAY[@]}
}

function empty_drives
{
    # make sure no LTFS filesystem is mounted, so we can unmount it
    /usr/share/ltfs/ltfs stop || true

    # make sure all drives are empty
    mtx status | grep "Data Transfer Element" | grep "Full" |
        while read line; do
            echo "full drive: $line"
            drive=$(echo $line | awk '{print $4}' | cut -d ':' -f 1)
            slot=$(echo $line | awk '{print $7}')
            echo "Unloading drive $drive in slot $slot"
            mtx unload $slot $drive || echo "mtx failure"
        done
}

function cleanup
{
    waive_daemon
    drop_tables
    clean_dir
    if [[ -w /dev/changer ]]; then
        empty_drives
    fi
}

function put_tape_simple
{
    local id=test/tape.simple

    $LOG_VALG $phobos drive add --unlock ${DRIVE_ARRAY[0]}

    # test tape availability
    $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects without media"

    $LOG_VALG $phobos tape add -t lto5 ${TAPE_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects without formatted media"

    $LOG_VALG $phobos tape format ${TAPE_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects without unlocked media"

    $LOG_VALG $phobos tape unlock ${TAPE_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id ||
        error "Put with an available media should have worked"

    # test drive availability
    # TODO: to uncomment once the admin lock on device is considered
    # when selecting one
    # $LOG_VALG $phobos drive lock ${DRIVE_ARRAY[0]}
    # $LOG_VALG $phobos put /etc/hosts ${id}.2 &&
    #     error "Should not be able to put objects without unlocked device"
}

function put_dir_simple
{
    local id=test/dir.simple

    # test dir availability
    $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects without media"

    $LOG_VALG $phobos dir add ${DIR_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects without formatted media"

    $LOG_VALG $phobos dir format --fs posix ${DIR_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects without unlocked media"

    $LOG_VALG $phobos dir unlock ${DIR_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id ||
        error "Put with an available medium should have worked"
}

function put_tape_raid
{
    local id=test/tape.raid

    $LOG_VALG $phobos drive add --unlock ${DRIVE_ARRAY[@]}
    $LOG_VALG $phobos tape add -t lto5 ${TAPE_ARRAY[1]}
    $LOG_VALG $phobos tape format --unlock ${TAPE_ARRAY[1]}

    # test tape availability
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects with 1/2 media"

    $LOG_VALG $phobos tape add -t lto5 ${TAPE_ARRAY[0]}
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects with 1/2 formatted media"

    $LOG_VALG $phobos tape format ${TAPE_ARRAY[0]}
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects with 1/2 unlocked media"

    $LOG_VALG $phobos tape unlock ${TAPE_ARRAY[0]}
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id ||
        error "Put with available media should have worked"

    # test drive availability
    # TODO: to uncomment once the admin lock on device is considered
    # when selecting one
    # $LOG_VALG $phobos drive lock ${DRIVE_ARRAY[0]}
    # $LOG_VALG $phobos put /etc/hosts ${id}.2 &&
    #     error "Should not be able to put objects with 1/2 unlocked devices"
}

function put_dir_raid
{
    local id=test/dir.raid

    $LOG_VALG $phobos dir add ${DIR_ARRAY[1]}
    $LOG_VALG $phobos dir format --fs posix --unlock ${DIR_ARRAY[1]}

    # test dir availability
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects with 1/2 media"

    $LOG_VALG $phobos dir add ${DIR_ARRAY[0]}
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects with 1/2 formatted media"

    $LOG_VALG $phobos dir format --fs posix ${DIR_ARRAY[0]}
    PHOBOS_STORE_default_layout=raid1 $LOG_VALG $phobos put /etc/hosts $id &&
        error "Should not be able to put objects with 1/2 unlocked media"

    # resources are available
    $LOG_VALG $phobos dir unlock ${DIR_ARRAY[0]}
    $LOG_VALG $phobos put /etc/hosts $id ||
        error "Put with available media should have worked"
}

function get_tape_simple
{
    local id=test/tape.simple

    # test drive availability
    # TODO: to uncomment once the admin lock on device is considered
    # when selecting one
    # $LOG_VALG $phobos get $id /tmp/out &&
    #    error "Should not be able to get objects without unlocked device"
    # rm /tmp/out

    # resources are available
    $LOG_VALG $phobos drive unlock ${DRIVE_ARRAY[0]}
    $LOG_VALG $phobos get $id /tmp/out ||
        error "Get with available medium should have worked"
    rm /tmp/out

    # test tape availability
    $LOG_VALG $phobos tape lock ${TAPE_ARRAY[0]}
    $LOG_VALG $phobos get $id /tmp/out &&
        error "Should not be able to get objects without unlocked medium"
    rm /tmp/out
}

function get_dir_simple
{
    local id=test/dir.simple

    # ressources are available
    $LOG_VALG $phobos get $id /tmp/out ||
        error "Get with available medium should have worked"
    rm /tmp/out

    # test dir availability
    $LOG_VALG $phobos dir lock ${DIR_ARRAY[0]}
    $LOG_VALG $phobos get $id /tmp/out &&
        error "Should not be able to get objects without unlocked medium"
    rm /tmp/out
}

function get_tape_raid
{
    local id=test/tape.raid

    # test drive availability
    # TODO: to uncomment once the admin lock on device is considered
    # when selecting one
    # $LOG_VALG $phobos get $id /tmp/out &&
    #    error "Get with 1/2 available device should have worked"
    # rm /tmp/out

    # $LOG_VALG $phobos drive lock ${DRIVE_ARRAY[1]}
    # $LOG_VALG $phobos get $id /tmp/out &&
    #     error "Should not be able to get objects without unlocked devices"
    # rm /tmp/out

    # resources are available
    $LOG_VALG $phobos drive unlock ${DRIVE_ARRAY[@]}
    $LOG_VALG $phobos get $id /tmp/out ||
        error "Get with available medium should have worked"
    rm /tmp/out

    # test tape availability
    $LOG_VALG $phobos tape lock ${TAPE_ARRAY[0]}
    $LOG_VALG $phobos get $id /tmp/out ||
        error "Get with 1/2 available medium should have worked"
    rm /tmp/out

    $LOG_VALG $phobos tape lock ${TAPE_ARRAY[1]}
    $LOG_VALG $phobos get $id /tmp/out &&
        error "Should not be able to get objects without unlocked media"
    rm /tmp/out
}

function get_dir_raid
{
    local id=test/dir.raid

    #ressources are available
    $LOG_VALG $phobos get $id /tmp/out ||
        error "Get with available medium should have worked"
    rm /tmp/out

    # test dir availability
    $LOG_VALG $phobos dir lock ${DIR_ARRAY[0]}
    $LOG_VALG $phobos get $id /tmp/out ||
        error "Get with 1/2 available medium should have worked"
    rm /tmp/out

    $LOG_VALG $phobos dir lock ${DIR_ARRAY[1]}
    $LOG_VALG $phobos get $id /tmp/out &&
        error "Should not be able to get objects without unlocked medium"
    rm /tmp/out
}

if [ -w /dev/changer ]; then
    export PHOBOS_LRS_families="dir,tape"
else
    export PHOBOS_LRS_families="dir"
fi

setup_dir
setup_tables
invoke_daemon
trap cleanup EXIT

# availability with simple layout
export PHOBOS_STORE_default_family="dir"
put_dir_simple
get_dir_simple

if [[ -w /dev/changer ]]; then
    # in case the previous test did not unload the drives
    empty_drives

    export PHOBOS_STORE_default_family="tape"
    put_tape_simple
    get_tape_simple
    empty_drives
fi

waive_daemon
drop_tables
clean_dir
setup_dir
setup_tables
invoke_daemon

# availability with raid layout
export PHOBOS_STORE_default_family="dir"
put_dir_raid
get_dir_raid

if [[ -w /dev/changer ]]; then
    export PHOBOS_STORE_default_family="tape"
    put_tape_raid
    get_tape_raid
fi
