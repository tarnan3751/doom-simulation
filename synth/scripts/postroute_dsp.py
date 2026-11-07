# nextpnr --post-route hook (runs inside nextpnr's Python, `ctx` is predefined).
#
# This nextpnr build has no timing arcs for DSP cells (MULT*): paths into and out of the
# multiplier are neither analysed nor listed. This hook records what can be measured, the
# routed delay of every net at every DSP port plus the cell on the other end, for
# report.py's "time left for the multiplier" figure.
# Output: $DSP_ROUTES_JSON (default dsp_routes.json in the working directory).
import json
import os

out = []
for cname, cell in ctx.cells:
    if not str(cell.type).startswith('MULT'):
        continue
    for pname, port in cell.ports:
        net = port.net
        if net is None:
            continue
        drv = net.driver
        drv_cell = drv.cell.name if drv.cell is not None else None
        if drv_cell == cname:
            # output port: one entry per sink
            for user in net.users:
                d = ctx.getDelayNS(ctx.getNetinfoRouteDelay(net, user))
                out.append({'cell': cname, 'port': pname, 'dir': 'out', 'net': net.name,
                            'other': user.cell.name, 'other_type': str(user.cell.type),
                            'other_port': user.port, 'route_ns': d})
        else:
            for user in net.users:
                if user.cell.name == cname and user.port == pname:
                    d = ctx.getDelayNS(ctx.getNetinfoRouteDelay(net, user))
                    out.append({'cell': cname, 'port': pname, 'dir': 'in', 'net': net.name,
                                'other': drv_cell, 'other_type': str(drv.cell.type) if drv.cell else None,
                                'other_port': drv.port, 'route_ns': d})

with open(os.environ.get('DSP_ROUTES_JSON', 'dsp_routes.json'), 'w') as f:
    json.dump(out, f, indent=1)
print('postroute_dsp: %d DSP port connections recorded' % len(out))
