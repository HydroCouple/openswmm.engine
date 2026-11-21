"""Independent NumPy/Radau integration of a sealed 1 m Richards column.
Reads the C++ export; does not import or call its RHS/linear solver.
"""
from pathlib import Path
import json
import numpy as np
from scipy.integrate import solve_ivp
folder = Path(__file__).resolve().parent
summary = []
for count in (8, 16, 32, 64):
    dz = 1.0 / count
    z = 1 - (np.arange(count) + .5) * dz
    volumes = np.r_[.1, np.full(count, dz)]
    m = 1 - 1/1.6
    def rhs(t, w):
        fraction = w[1:]/dz
        se = np.maximum((fraction - .05)/.4, 1.e-14)
        h = np.where(fraction >= .45, (fraction - .45)/1.e-4,
                     -np.maximum(se**(-1/m) - 1, 0)**(1/1.6)/2)
        effective = (1 + np.maximum(-2*h, 0)**1.6)**(-m)
        k = 1.e-5 * effective**.5 * (1 - (1 - effective**(1/m))**m)**2
        heads = z+h
        faces = np.zeros(count)
        faces[0] = (1.e-5+k[0])/dz * (1 + max(0.,w[0]) - heads[0]) * np.clip(w[0]/1.e-6, 0, 1)
        faces[1:] = (.5*(k[:-1]+k[1:])/dz) * (heads[:-1] - heads[1:])
        return np.r_[-faces[0], faces - np.r_[faces[1:],0]]
    initial = np.r_[.02, np.full(count, .17*dz)]
    reference = solve_ivp(rhs,(0,600),initial,method='Radau',rtol=1.e-10,atol=1.e-12*volumes)
    if not reference.success: raise RuntimeError(reference.message)
    production = np.genfromtxt(folder/f'reference.profile{count}.csv', delimiter=',', names=True)
    difference = np.abs(production['water'] - reference.y[:,-1])
    row = {'cells':count,'max_water_error_m3':float(difference.max()),
           'max_moisture_error':float((difference/volumes).max()),
           'surface_infiltrated_m':float(.02-production['water'][0]),
           'reference_infiltrated_m':float(.02-reference.y[0,-1]),
           'reference_rhs':reference.nfev,'reference_balance_m3':float(reference.y[:,-1].sum()-initial.sum())}
    summary.append(row)
    np.savetxt(folder/f'radau_profile{count}.csv',np.c_[np.r_[1,z],reference.y[:,-1]], delimiter=',', header='z,water',comments='')
(folder/'reference_comparison.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
assert all(row['max_moisture_error'] < 1.e-4 for row in summary)
