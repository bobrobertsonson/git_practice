"""bpy-free geometry of the cab "open" ortho views (grille off), shared by cab_4x12.py, cab_2x12.py and
export_ui_assets.py (which writes the driver centres / radii sidecars for the plugin's mic page).

Cab coordinates are the scripts' panel coordinates in mm: x to the right, y up, origin at the baffle centre.
The ortho camera looks along the panel normal at the baffle centre, so panel (x, y) maps to image pixels
    px = width / 2 + x * s,   py = height / 2 - y * s,   s = max(width, height) / (ortho_scale * 1000)
(Blender's ortho_scale spans the larger render dimension; the cab and the camera are lifted by the same amount).
"""

# 12" driver from common.build_speaker: cone (outer edge of the cone / start of the surround) and dust cap radius.
CONE_RADIUS_MM = 128.5
DUST_CAP_RADIUS_MM = 34.0

CABS = {
    '4x12': dict(speakers=[(-180.0, 180.0), (180.0, 180.0), (-180.0, -180.0), (180.0, -180.0)],
                 ortho_scale_m=0.86, res=(1400, 1400), lift_mm=60.0, open_png='cab_open_ortho.png'),
    '2x12': dict(speakers=[(-165.0, 0.0), (165.0, 0.0)],
                 ortho_scale_m=0.84, res=(1400, 1000), lift_mm=50.0, open_png='cab2x12_open_ortho.png'),
}


def drivers(cab, stored_width):
    """Driver centres and radii in pixels of the open view stored at `stored_width` px wide."""
    c = CABS[cab]
    w, h = c['res']
    s = max(w, h) / (c['ortho_scale_m'] * 1000.0) * (stored_width / w)
    sw, sh = stored_width, round(h * stored_width / w)
    out = []
    for i, (x, y) in enumerate(c['speakers']):
        out.append({'slot': i + 1, 'cx': round(sw / 2 + x * s, 2), 'cy': round(sh / 2 - y * s, 2),
                    'cone_radius': round(CONE_RADIUS_MM * s, 2), 'cap_radius': round(DUST_CAP_RADIUS_MM * s, 2)})
    return {'cab': cab, 'width': sw, 'height': sh, 'px_per_mm': round(s, 5), 'drivers': out}
