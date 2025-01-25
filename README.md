Advanced Weather

3D Radar Viewer for NEXRAD Level 2 Data.
This repo was copied from http://pileus.org/aweather/files/aweather-0.8.1.tar.gz and work was done to restore the program to a semi-functioning state.
Most of the APIs used by the original program (version 0.8.1) had changed or no longer existed. In many cases, alternatives were found. In some cases, none were found. Details are below.

Functional plugins:
- alert: Display NOAA weather alerts by county or by storm. Also displays county borders on the map.
- borders: Displays state borders. Useful if not using the map plugin to get a dark-theme map.
- env: Displays compass and atmosphere
- map: Displays Open Street Maps on the globe.
- radar: Displays NEXRAD level 2 data for stations in the US and a CONUS overview when zoomed out.
- test: Test plugin.

Nonfunctional plugins:
- elev: Previously adjusted the elevation of points on the map to reflect elevation changes in the world. API no longer exists. No drop-in replacement found.
- sat: Displays static satellite imagery on the map instead of the OSM imagery from the map plugin. API no longer exists. No drop-in replacement found.

Installation
- Arch Linux: Available in the AUR (https://aur.archlinux.org/packages/aweather-git)

Changes made from pileus.org version:
- Added vector based dark map mode. State and county borders are drawn, but no requests are made to Open Street Maps. This mode is the default.
- Added animation support for NEXRAD Level2 radar, including 3D isosurfaces.
- Fixed many bugs that caused the application to crash.

Original data sources for elevation and satellite imagery:
Ground    (WMS) - http://www.nasa.network.com/wms
Elevation (WMS) - http://www.nasa.network.com/elev

See also:
WDSS II  - http://www.wdssii.org/
AWIPS    - http://www.nws.noaa.gov/ops2/ops24/awips.htm
AWIPS II - http://www.unidata.ucar.edu/Presentations/AWIPS/AE_Overview_NCEP_v2-1.pdf
