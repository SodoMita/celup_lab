# celup_lab generated comparison sheets

Metric: mean absolute error in premultiplied-linear RGBA, excluding 4px border; lower is better.

| case | nearest | bilinear | adaptive | autodeblur | analytical g1 | analytical g5 | sdf |
|---|---|---|---|---|---|---|---|
| badge | 0.00930 | 0.01276 | 0.01189 | 0.00917 | 0.00734 | 0.01101 | 0.01180 |
| lines | 0.03555 | 0.04501 | 0.04176 | 0.03193 | 0.03474 | 0.04225 | 0.04195 |
| curves_alpha | 0.01359 | 0.01697 | 0.01576 | 0.01621 | 0.01009 | 0.01496 | 0.01521 |
| photoish | 0.00280 | 0.00282 | 0.00235 | 0.00272 | 0.00214 | 0.00250 | 0.00245 |
| crossing | 0.01314 | 0.01560 | 0.01403 | 0.01493 | 0.00856 | 0.01366 | 0.01474 |
| soft_gradient | 0.00396 | 0.00400 | 0.00431 | 0.00395 | 0.00345 | 0.00386 | 0.00431 |
| tiny_text | 0.01814 | 0.02527 | 0.02291 | 0.02324 | 0.02306 | 0.02313 | 0.02354 |
| checker1 | 0.00000 | 0.21424 | 0.28500 | 0.28629 | 0.28629 | 0.28629 | 0.28500 |
| checker2 | 0.00048 | 0.09489 | 0.18530 | 0.06047 | 0.05531 | 0.05618 | 0.18530 |
| crosshatch | 0.13148 | 0.14896 | 0.15226 | 0.14832 | 0.14045 | 0.14698 | 0.15226 |
| rings | 0.03946 | 0.05005 | 0.04937 | 0.01848 | 0.02806 | 0.04458 | 0.04997 |

Generated files:

- `comparison_sheets/comparison_sheet.webp`
- `comparison_sheets/example_sources.webp`
- `comparison_sheets/comparison_<case>.webp`
- `examples/*_source_96.webp` and matching mode outputs
