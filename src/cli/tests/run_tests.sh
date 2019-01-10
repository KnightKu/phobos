#!/bin/sh

#
#  All rights reserved (c) 2014-2017 CEA/DAM.
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

cur_dir=$(dirname $(readlink -m $0))

PY=$(which python)
ARCH=$(uname -m)
PY_VERSION=$($PY -c 'import sys; print "%d.%d" % (sys.version_info[0],\
                                                  sys.version_info[1])')

# pure-python modules
PHO_PYTHON_PATH="$cur_dir/../build/lib.linux-$ARCH-$PY_VERSION/"

# toplevel phobos library
PHO_STORELIB_PATH="$cur_dir/../../store/.libs/"
PHO_LAYOUTLIB_PATH="$cur_dir/../../layout-modules/.libs/"

export LD_LIBRARY_PATH="$PHO_STORELIB_PATH:$PHO_PYTHON_PATH:$PHO_LAYOUTLIB_PATH"
export PYTHONPATH="$PHO_PYTHON_PATH"

PHOBOS_DB="$cur_dir/../../../scripts/phobos_db"

conn_str="dbname=phobos user=phobos password=phobos"
export PHOBOS_DSS_connect_string="$conn_str"
for test_case in *Test.py
do
    $PHOBOS_DB drop_tables
    $PHOBOS_DB setup_tables
    $PY $test_case || exit 1
done

# clean test data
$PHOBOS_DB drop_tables
