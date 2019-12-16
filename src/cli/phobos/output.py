#!/usr/bin/python

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

"""
Output and formatting utilities.
"""

import json
import csv
import StringIO
from tabulate import tabulate
import xml.dom.minidom
import xml.etree.ElementTree

from phobos.core.utils import bytes2human

import yaml

def csv_dump(data):
    """Convert a list of dictionaries to a csv string"""
    outbuf = StringIO.StringIO()
    writer = csv.DictWriter(outbuf, sorted(data[0].keys()))
    if hasattr(writer, 'writeheader'):
        #pylint: disable=no-member
        writer.writeheader()
    else:
        writer.writerow(dict((item, item) for item in data[0]))
    writer.writerows(data)
    out = outbuf.getvalue()
    outbuf.close()
    return out

def xml_dump(data, item_type='item'):
    """Convert a list of dictionaries to xml"""
    top = xml.etree.ElementTree.Element('phobos')
    for item in data:
        children = xml.etree.ElementTree.Element(item_type, **item)
        top.append(children)
    rough_string = xml.etree.ElementTree.tostring(top)
    reparsed = xml.dom.minidom.parseString(rough_string)
    return reparsed.toprettyxml(indent="  ")

def human_dump(data):
    """Convert a list of dictionaries to an identifier list text"""
    out = "\n".join(str(item['label']) for item in data)
    return out

def human_pretty_dump(data):
    """Convert a list of dictionaries to human readable text"""
    # Convert space sizes to human readable sizes
    space_size_attr = [
        "stats.phys_spc_free",
        "stats.logc_spc_used",
        "stats.phys_spc_used"
    ]

    for obj in data:
        for key, attr in obj.items():
            if key in space_size_attr:
                obj[key] = bytes2human(attr)

    # Generate formatted printing
    out = tabulate(data, headers="keys", tablefmt="github")
    return out

def filter_display_dict(objs, attrs):
    info = [x.get_display_dict() for x in objs]

    # If all/* is an attribute, we fetch them all
    obj_list = []
    if 'all' in attrs or '*' in attrs:
        obj_list = info
    else:
        for attr_dict in info:
            obj_list.append({k: attr_dict[k] for k in attr_dict if k in attrs})

    return obj_list

def dump_object_list(objs, item_type, attr=None, fmt="human"):
    """Helper for user friendly object display."""
    if not objs:
        return

    formats = {
        'json' : json.dumps,
        'yaml' : yaml.dump,
        'xml'  : xml_dump,
        'csv'  : csv_dump,
        'human': human_dump,
    }

    if attr != ['label']:
        formats['human'] = human_pretty_dump

    objlist = filter_display_dict(objs, attr)

    # Remove the endstring newline generated by csv, yaml and xml formatters
    print formats[fmt](objlist).rstrip()
