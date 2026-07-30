# v2 improvement plan (from handoff.md "Recommended next direction")

1. Unified 5x5 patch classifier: edge / checker-Nyquist / junction-crossing / smooth / texture
   - single pass stats -> class map (per source pixel), reusable by all improved modes
   - diagnostic `classmap` mode renders the classification as RGB
2. New flagship `--mode adaptive`
   - checker cells -> explicit `--checker-policy lowpass|bilinear|nearest|mitchell|scale2x|auto`
   - coherent edges -> fitted edge model sharpening (gated)
   - junctions -> conservative bilinear/lowpass mix
   - base -> bounded Mitchell
3. scale2x policy for pixel-art checker regions (hard, invents no colours)
4. Gated deblurcompress: precomputed per-cell weights (edge gated backprojection,
   edge-gated unsharp) -> fewer soft hourglasses + big speedup
5. Gated dehourglass: hourglass basis removal focused by checker confidence
6. Evaluation: baseline vs v2 MAE + new checker/crosshatch torture scenes with
   quantitative hourglass-amplitude metric + visual sheets incl. classmap
7. Docs: README_lab.md, handoff.md, MANIFEST, rezip v2 archive
