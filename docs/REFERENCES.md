# SmazkaVG references

Checked online 2026-08-07. References are grouped as primary design sources, solver/numerical sources, rendering standards, and comparative research.

## Primary design references

1. **Dalstein, Ronfard, van de Panne — [Vector Graphics Complexes](https://www.borisdalstein.com/research/vgc/), ACM TOG 2014, [DOI](https://doi.org/10.1145/2601097.2601169).** Primary representation reference: colored incidence graph, shared non-manifold topology, and global cell depth order. Source successor: [vgc/vgc](https://github.com/vgc/vgc).
2. **Dalstein et al. — [Vector Graphics Animation with Time-Varying Topology](https://doi.org/10.1145/2766913), ACM TOG 2015.** Reference before adding temporal cell topology.
3. **SodoMita — [psolve](https://github.com/SodoMita/psolve).** Mandatory pinned backend providing revised-simplex LP, active-set convex QP, branch-and-bound MIP, and fixed-point PGS.
4. **Boyd and Vandenberghe — [Convex Optimization](http://www.seas.ucla.edu/~vandenbe/cvxbook.html), 2004.** Definitions and formulations for convex sets, LP/QP, KKT systems, and lexicographic optimization foundations.
5. **Schulman et al. — [Motion Planning with Sequential Convex Optimization and Convex Collision Checking](https://escholarship.org/uc/item/6km506db), IJRR 2014, [DOI](https://doi.org/10.1177/0278364914528132).** Reference for honestly describing local sequential convexification of non-convex separation constraints.

## Constraint and numerical implementations

6. **Stellato et al. — [OSQP](https://osqp.org/), [source](https://github.com/osqp/osqp).** Solver statuses, bounded convex-QP API, warm starts, and embeddable implementation.
7. **[HiGHS](https://highs.dev/), [source](https://github.com/ERGO-Code/HiGHS).** Independent LP/QP/MIP behavior and reporting reference.
8. **Monniaux — [The Pitfalls of Verifying Floating-Point Computations](https://hal.science/hal-00128124), TOPLAS 2008, [DOI](https://doi.org/10.1145/1353445.1353446).** Why floating types alone do not imply cross-platform bit reproducibility.
9. **Petteri Aimonen — [libfixmath](https://github.com/PetteriAimonen/libfixmath).** Practical Q16.16 implementation and tests.
10. **Google — [Protocol Buffers Encoding](https://protobuf.dev/programming-guides/encoding/).** Bounded base-128 varints and ZigZag encoding reference.
11. **Bormann and Hoffman — [RFC 8949: CBOR](https://www.rfc-editor.org/rfc/rfc8949).** Data-model versus deterministic-serialization separation and decoder resource limits.

## Rendering and compositing standards

12. **W3C — [SVG 2 Rendering Model](https://www.w3.org/TR/SVG2/render.html).** Compositing an already resolved back-to-front scene. Smazka source itself remains unordered; absolute cell Z produces the render order.
13. **W3C — [SVG 2 Painting](https://w3c.github.io/svgwg/svg2-draft/painting.html).** Fill, stroke, cap, join, and paint behavior.
14. **W3C — [Compositing and Blending Level 1](https://www.w3.org/TR/compositing-1/).** Source-over and premultiplied-alpha equations.
15. **Porter and Duff — [Compositing Digital Images](https://doi.org/10.1145/800031.808606), SIGGRAPH 1984.** Foundational compositing algebra.
16. **W3C — [PNG Specification, Third Edition](https://www.w3.org/TR/png-3/).** Future raster writer requirements.
17. **TinyVG — [specification](https://github.com/TinyVG/specification), [SDK](https://github.com/TinyVG/sdk).** Compact binary vector-format comparison.
18. **Loop and Blinn — [Resolution Independent Curve Rendering Using Programmable Graphics Hardware](https://www.microsoft.com/en-us/research/wp-content/uploads/2005/01/p1000-loop.pdf), [DOI](https://doi.org/10.1145/1073204.1073303).** Typed implicit quadratic/cubic painting kernels.
19. **[resvg](https://github.com/linebender/resvg), [Blend2D](https://github.com/blend2d/blend2d), [Skia](https://skia.googlesource.com/skia/), [Cairo](https://gitlab.freedesktop.org/cairo/cairo).** Differential references for path validation, antialiasing, compositing, and pathological geometry.

## Curves and smooth color fields

20. **Catmull and Rom — “A Class of Local Interpolating Splines,” [DOI](https://doi.org/10.1016/B978-0-12-079050-0.50020-5), 1974.** Original local interpolating spline.
21. **Yuksel, Schaefer, Keyser — [Parameterization and Applications of Catmull–Rom Curves](http://www.cemyuksel.com/research/catmullrom_param/), [DOI](https://doi.org/10.1016/j.cad.2010.08.008).** Centripetal parameterization and cusp/self-intersection behavior.
22. **Orzan et al. — [Diffusion Curves](https://maverick.inria.fr/Publications/2008/OBWBTS08/), ACM TOG 2008, [DOI](https://doi.org/10.1145/1399504.1360691).** Reference for a future typed diffusion edge kernel.
23. **Berk Bavaş — [DiffusionCurveRenderer](https://github.com/berkbavas/DiffusionCurveRenderer).** Accessible implementation for differential study.

## Vectorization and topology

24. **Noris et al. — [Topology-Driven Vectorization of Clean Line Drawings](https://doi.org/10.1145/2421636.2421640), ACM TOG 2013.** Junction ambiguity and topology-first reconstruction.
25. **Favreau, Lafarge, Bousseau — [Fidelity vs. Simplicity](https://hal.science/hal-01309271), [DOI](https://doi.org/10.1145/2897824.2925946).** Explicit fidelity/editability tradeoff.
26. **Bessmeltsev and Solomon — [Vectorization of Line Drawings via PolyVector Fields](https://doi.org/10.1145/3202661), [preprint](https://arxiv.org/abs/1801.01922).** Topology extraction around X/T junctions.
27. **Puhachov et al. — [Keypoint-Driven Line Drawing Vectorization via PolyVector Flow](https://doi.org/10.1145/3478513.3480529).** Separates keypoints, topology, and geometry.
28. **Yin et al. — [Viewer-Perceived Intended Vector Sketch Connectivity](https://www.cs.ubc.ca/labs/imager/tr/2022/SketchConnectivity/), [source](https://github.com/enjmiah/SketchConnectivity).** Local/global intended-connectivity evidence.
29. **Li et al. — [diffvg](https://people.csail.mit.edu/tzumao/diffvg/), [source](https://github.com/BachiLi/diffvg), [DOI](https://doi.org/10.1145/3414685.3417871).** Differentiable raster-space fitting and renderer comparison.
30. **Ma et al. — [LIVE](https://openaccess.thecvf.com/content/CVPR2022/html/Ma_Towards_Layer-Wise_Image_Vectorization_CVPR_2022_paper.html), [source](https://github.com/Picsart-AI-Research/LIVE-Layerwise-Image-Vectorization).** Layer-wise topology and self-crossing/unsigned-distance losses.
31. **Cheng et al. — [Boundary IoU](https://openaccess.thecvf.com/content/CVPR2021/html/Cheng_Boundary_IoU_Improving_Object-Centric_Image_Segmentation_Evaluation_CVPR_2021_paper.html), [source](https://github.com/bowenc0221/boundary-iou-api).** Boundary-sensitive evaluation.

## LLM/VLM vector-graphics work

32. **Rodriguez et al. — [StarVector](https://openaccess.thecvf.com/content/CVPR2025/html/Rodriguez_StarVector_Generating_Scalable_Vector_Graphics_Code_from_Images_and_Text_CVPR_2025_paper.html), [source](https://github.com/joanrod/star-vector).** SVG code generation and structural evaluation beyond pixel MSE.
33. **Zou et al. — [VGBench](https://aclanthology.org/2024.emnlp-main.213/), [source](https://github.com/vgbench/VGBench).** Vector-code understanding and generation benchmark.
34. **Nishina and Matsui — [SVGEditBench](https://openaccess.thecvf.com/content/CVPR2024W/GDUG/html/Nishina_SVGEditBench_A_Benchmark_Dataset_for_Quantitative_Assessment_of_LLMs_SVG_CVPRW_2024_paper.html), [source](https://github.com/mti-lab/SVGEditBench).** Executable targeted edit checks.
35. **Chen et al. — [SVGenius](https://arxiv.org/abs/2506.03139), [source](https://github.com/ZJU-REAL/SVGenius).** Complexity-stratified understanding/editing/generation.
36. **Wang et al. — [InternSVG / SArena](https://hmwang2002.github.io/release/internsvg/), [source](https://github.com/hmwang2002/InternSVG).** Unified SVG data, benchmark, and model.
37. **Gupta and Hebbar — [Vector-Bench](https://arxiv.org/abs/2607.19056), [artifact](https://github.com/yug-space/vector-edit-gym).** Requested-edit correctness, untouched-structure preservation, validity, and transparent traces.

## Robustness

38. **LLVM — [libFuzzer](https://llvm.org/docs/LibFuzzer.html).** Coverage-guided parsing tests with sanitizers.
39. **Google — [OSS-Fuzz](https://github.com/google/oss-fuzz).** Continuous fuzzing model for public parsers and binary decoders.
