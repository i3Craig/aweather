#!/bin/python
# Copyright (C) 2009-2011 Andy Spencer <andy753421@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# This program was largely written by ChatGPT to pull in the latest data from https://www.ncei.noaa.gov/access/homr/
# (specifically https://www.ncei.noaa.gov/access/homr/file/nexrad-stations.txt) to update the `src/aweather-locations.c` file with the
# latest list of active radar sites.
# Documentation on the radar sites file is available here: https://www.ncei.noaa.gov/access/homr/file/NexRad_Table.txt


import re
import json
import requests
from datetime import date

# State code to name mapping
STATE_CODES = {
    "AL": "Alabama",
    "AK": "Alaska",
    "AZ": "Arizona",
    "AR": "Arkansas",
    "CA": "California",
    "CO": "Colorado",
    "CT": "Connecticut",
    "DE": "Delaware",
    "FL": "Florida",
    "GA": "Georgia",
    "HI": "Hawaii",
    "ID": "Idaho",
    "IL": "Illinois",
    "IN": "Indiana",
    "IA": "Iowa",
    "KS": "Kansas",
    "KY": "Kentucky",
    "LA": "Louisiana",
    "ME": "Maine",
    "MD": "Maryland",
    "MA": "Massachusetts",
    "MI": "Michigan",
    "MN": "Minnesota",
    "MS": "Mississippi",
    "MO": "Missouri",
    "MT": "Montana",
    "NE": "Nebraska",
    "NV": "Nevada",
    "NH": "New Hampshire",
    "NJ": "New Jersey",
    "NM": "New Mexico",
    "NY": "New York",
    "NC": "North Carolina",
    "ND": "North Dakota",
    "OH": "Ohio",
    "OK": "Oklahoma",
    "OR": "Oregon",
    "PA": "Pennsylvania",
    "RI": "Rhode Island",
    "SC": "South Carolina",
    "SD": "South Dakota",
    "TN": "Tennessee",
    "TX": "Texas",
    "UT": "Utah",
    "VT": "Vermont",
    "VA": "Virginia",
    "WA": "Washington",
    "WV": "West Virginia",
    "WI": "Wisconsin",
    "WY": "Wyoming",

    "GU": "Guam",
    "PR": "Puerta Rico",

}

# File parsing functions
def parse_file_a(file_a_path):
    """Parse File A to extract ID, name, lat, long, and state code using fixed positions."""
    data = []
    with open(file_a_path, 'r') as f:
        lines = f.readlines()
        for line in lines[2:]:  # Skip header rows
            cStateCd = line[72:74].strip()  # Extract state code (2 digits)
            if cStateCd in STATE_CODES:
                cStateCd = STATE_CODES[cStateCd] # Convert this state code into a name if we can.
            else:
                # Use the state code if given, otherwise fall back to the country name.
                cStateCd = cCountryCd
            cCountryCd = line[51:71].strip()  # Extract country name
            record = {
                'ID': line[9:13].strip(),
                'name': line[20:50].strip().title(), #The data source provides all city names in all caps.
                'lat': float(line[106:115].strip()),
                'long': float(line[116:126].strip()),
                'state': cStateCd,
                'radarType': line[140:190].strip() # NEXRAD or TDWR
            }
            data.append(record)
    return data

def parse_file_b(file_b_path):
    """Parse File B to extract location data and headers."""
    cities = []
    with open(file_b_path, 'r') as f:
        content = f.read()
        matches = re.findall(r'\{([^}]+\}[^}]+)\}', content) # Capture each line in the current file
        cCurrentState = None
        for match in matches:
            parts = match.split(',')
            if len(parts) >= 5:
                cId = parts[1].strip().strip('"')
                cName = parts[2].strip().strip('"')
                if cId == "NULL":
                    cCurrentState = cName
                else:
                    city = {
                        'type': parts[0].strip(),
                        'ID': cId,
                        'name': cName,
                        'state': cCurrentState,
                        'lat': float(parts[3].strip().strip('{}')),
                        'long': float(parts[4].strip().strip('{}')),
                        'LOD': float(parts[6].strip()),
                    }
                    cities.append(city)
    return cities

def parse_sites_with_files(file_sites_with_files):
    """Parses the file with the list of sites that have level 2 data we can download currently"""
    sites = []
    with open(file_sites_with_files, 'r') as f:
        lines = f.readlines()
        # Look for and append all lines that start with "Site:"
        for line in lines:
            fields = line.split(' ')
            if len(fields) ==3 \
                and fields[1] == 'Site:':
                sites.append(fields[2].strip())
    return sites


# Merge data
def merge_data(file_a_data, file_b_data, file_sites_with_files_data):
    """Merge data from File A into File B, preserving headers."""
    b_data_map = {item['ID']: item for item in file_b_data if item['ID']}
    merged_data = []

    for record in file_a_data:
        if record['ID'] in b_data_map:
            # Update existing record in B
            updated = b_data_map[record['ID']]
            updated.update({
                 #'name': record['name'], - keep original name if set
                'lat': record['lat'],
                'long': record['long'],
            })
            merged_data.append(updated)
        else:
            # New record in B file with default LOD (Level Of Depth)
            merged_data.append({
                'type': 'LOCATION_CITY',
                'ID': record['ID'],
                'name': record['name'],
                'lat': record['lat'],
                'long': record['long'],
                'LOD': (0.5 if record['radarType'] == "NEXRAD" else 0.1), # Don't show non-NEXRAD radars until the user zooms in more
                'state': record['state'],
                'lNewRecord': True,
            })

    for record in merged_data:
        # We are iterating through all sites that currently exist. Mark them all as active. We will add the inactive sites below.
        record['type'] = 'LOCATION_CITY'
        
        # If this is a new record and there is another radar site in the same state that is also active with the same name, then append the site ID to the name.
        # The name doesn't have to be unique, but this can help when identifying a site.
        if 'lNewRecord' in record and record['lNewRecord'] \
            and record['name'] in [r['name'] for r in merged_data if r['state'] == record['state'] and r['type'] == 'LOCATION_CITY' and r['ID'] != record['ID'] ]:
            record['name'] += f' ({record['ID']})'

     # Find all obsolete records that existed in the file, but are no longer active.
    for record in file_b_data:
        if not( record['ID'] in [r['ID'] for r in merged_data] ):
            # If the sites are no longer active, keep them in the list, but mark them as "NOP" (presumably Not Operable)
            record['type'] = 'LOCATION_NOP'
            merged_data.append(record)

    # Now that we have the list of all sites, check if they are in the list of sites that we can get data from. If they are not, then we mark them as NOP.
    for record in merged_data:
        if not(record['ID'] in file_sites_with_files_data):
            record['type'] = 'LOCATION_NOP'

    return merged_data

# File generation function
def generate_file_b(file_b_path, merged_data):
    """Write the merged data back to File B format."""
    header = f"""/*
 * Copyright (C) 2009-2011 Andy Spencer <andy753421@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file was automatically updated with helpers/update-aweather-locations-file-from-web.py on {date.today()}.
 */

#include <config.h>
#include <gtk/gtk.h>

#include "aweather-location.h"

city_t cities[] = {{
"""
    with open(file_b_path, 'w') as f:
        f.write(header)
        # Sort the merged data by state so we can append the state row, then the radar sites in that state
        merged_data.sort(key=lambda x: x['state'])
        cCurrentState = None
        for record in merged_data:
            if cCurrentState != record['state']:
                cCurrentState = record['state']
                # We hit a new state - append a header row
                f.write(
                    f"\t{{LOCATION_STATE,\t\"NULL\",\t\"{record['state']}\",\t{{0,\t0,\t0}},\t0.0}},\n"
                )

            f.write(
                f"\t\t{{{record['type']},\t\"{record['ID']}\",\t\"{record['name']}\",\t{{{record['lat']},\t{record['long']},\t0}},\t{record['LOD']}}},\n"
            )
        f.write("};\n")


def download_file(url, local_filename):
    """Download a file from a URL to a local path."""
    response = requests.get(url, stream=True)
    if response.status_code == 200:
        with open(local_filename, 'wb') as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
    else:
        raise Exception(f"Failed to download file from {url}. Status code: {response.status_code}")


# Main function
def main():
    file_a_url = 'https://www.ncei.noaa.gov/access/homr/file/nexrad-stations.txt'
    file_a_path = 'nexrad-stations.txt'  # Update with actual path
    file_b_path = '../src/aweather-location.c'   # Update with actual path
    file_sites_with_files = 'grlevel2.cfg'

    # Download the latest nexrad sites list.
    download_file(file_a_url, file_a_path)

    # Download the list of sites that actually have NEXRAD files on the server we pull radar files from.
    download_file('https://nomads.ncep.noaa.gov/pub/data/nccf/radar/nexrad_level2/grlevel2.cfg', file_sites_with_files)

    file_a_data = parse_file_a(file_a_path)
    file_b_data = parse_file_b(file_b_path)
    file_sites_with_files_data = parse_sites_with_files(file_sites_with_files)
    merged_data = merge_data(file_a_data, file_b_data, file_sites_with_files_data)
    generate_file_b(file_b_path, merged_data)

if __name__ == "__main__":
    main()

