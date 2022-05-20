#
#  All rights reserved (c) 2014-2022 CEA/DAM.
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

#
# Context initializer for RADOS library adapter API call tests
#

test_bin_dir=$(dirname $(readlink -e $0))
test_bin="$test_bin_dir/test_lib_dev_rados"
. $test_bin_dir/../../test_env.sh
. $test_bin_dir/setup_db.sh
ceph_pool="sudo ceph osd pool"
hostname=$(hostname)

set -xe

function setup
{
    setup_tables
    $ceph_pool create pho_pool_valid
}

function cleanup
{
    drop_tables
    $ceph_pool rm pho_pool_valid pho_pool_valid --yes-i-really-really-mean-it
}

trap cleanup EXIT
setup

$LOG_COMPILER $test_bin
