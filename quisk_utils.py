from __future__ import print_function
from __future__ import division

class SplineInterpolator:	# From Numerical Recipes in C
  """Interpolate a table of [x, y] values."""
  def __init__(self, table, Xmin=None, Xmax=None):
    # The table is a list of [x, y]. Any interpolated x value must be within the table range.
    # Do not mix [x, y] and (x, y).  The table can be extended using Xmin and Xmax.
    self.y2a = None
    table.sort()	# entries must be in order
    for i in range(len(table) - 1, 0, -1):	# entries must be unique
      if table[i][0] == table[i - 1][0]:
        del table[i]
    if len(table) < 3:
      return
    yp1 = float(table[1][1] - table[0][1]) / (table[1][0] - table[0][0])	# first derivative at start
    l = len(table) - 1
    ypn = float(table[l][1] - table[l - 1][1]) / (table[l][0] - table[l - 1][0])	# first derivative at end
    if Xmin is not None and table[0][0] > Xmin:	
      Xmin = float(Xmin)
      table.insert(0, [Xmin, table[0][1] - yp1 * (table[0][0] - Xmin)])
    l = len(table) - 1
    if Xmax is not None and table[l][0] < Xmax:
      Xmax = float(Xmax)
      table.append([Xmax, table[l][1] + ypn * (Xmax - table[l][0])])
    n = self.interp_n = len(table)
    self.xa = x = [0.0]	# index is 1, 2, ...
    self.ya = y = [0.0]
    for c, v in table:
      x.append(float(c))
      y.append(float(v))
    u = [0] * n
    y2 = self.y2a = [0] * (n + 1)
    y2[1] = -0.5
    u[1] = (3.0 / (x[2] - x[1])) * ((y[2] - y[1]) / (x[2] - x[1]) -yp1)
    for i in range(2, n):
      sig = (x[i] - x[i-1]) / (x[i+1] - x[i-1])
      p = sig * y2[i-1] + 2.0
      y2[i] = (sig - 1.0) / p
      u[i] = (y[i+1] - y[i]) / (x[i+1] - x[i]) - (y[i] - y[i-1]) / (x[i] - x[i-1])
      u[i] = (6.0 * u[i] / (x[i+1] - x[i-1]) - sig * u[i-1]) / p
    qn = 0.5
    un = (3.0 / (x[n] - x[n-1])) * (ypn - (y[n] - y[n-1]) / (x[n] - x[n-1]))
    y2[n] = (un - qn * u[n-1]) / (qn * y2[n-1] + 1.0)
    for k in range(n-1, 0, -1):
      y2[k] = y2[k] * y2[k+1] + u[k]
  def Interpolate(self, x):
    """Return the y value given x using spline interpolation."""
    if self.y2a is None:
      return 0.0
    x = float(x)
    n = self.interp_n
    xa = self.xa
    ya = self.ya
    y2a = self.y2a
    klo = 1
    khi = n
    while (khi - klo) > 1:
      k = (khi + klo) >> 1
      if xa[k] > x:
        khi = k
      else:
        klo = k
    h = xa[khi] - xa[klo]
    if h == 0.0:
      return 0.0
    a = (xa[khi] - x) / h
    b = (x - xa[klo]) / h
    y = a * ya[klo] + b * ya[khi] + ((a * a * a - a) * y2a[klo] + (b * b * b - b) * y2a[khi]) * (h * h) / 6.0
    return y
