# Automatic Mosaic Remover Detailed Design

**Author** · hiraken0427

## Purpose and scope

I am building a desktop application that lets me select a video, find and follow block mosaics, review the affected pixels, and export a reconstructed video. I use classical signal processing and numerical optimization throughout. The application runs locally on the CPU, and I expect a first-time user to complete the basic workflow with the default settings.

My first target is an SDR, progressive video with an axis-aligned rectangular mosaic produced by averaging or a similar blockwise operation. Square blocks are a special case. I support independent block width, height, and phase because mosaics may use rectangular blocks or start away from the image origin. I support both full-video scanning and editing of a small region over a short frame range. I treat severely compressed, rotated-grid, or non-block obscuration as outside the automatic detector's initial scope. Blur and opaque overlays have different observation models.

An averaged block has lost its individual pixel values. I can recover detail when another frame shows the same surface, or constrain the reconstruction when differently placed blocks observe that surface. A region that stays hidden behind a fixed grid remains ambiguous. I display where reconstruction relies on observed pixels versus estimation so I can decide whether to keep the result.

## Platform and build

I use C++20 for the application. SDL2 provides windows, input, and rendering, and Dear ImGui provides the controls through its SDL2 and SDLRenderer2 backends. FFmpeg provides demuxing, decoding, pixel conversion, encoding, and muxing through libavformat, libavcodec, libavutil, and libswscale. I keep the numerical kernels in the repository so the domain layer needs only the C++ standard library.

I target Windows 11 26H2 with x86-64 GCC and GNU Make from w64devkit. The same Makefile supports Linux for development and automated checks. I build and link SDL2 and FFmpeg for the target's compiler and ABI, and compile the Dear ImGui sources and backends with the application. The implementation will record the exact compiler and library releases verified on both platforms, along with the dependency origins and checksums. I expect the Makefile to support an explicit dependency prefix for Windows and pkg-config discovery on Linux. A separate clean build directory will hold each platform's binaries and generated files.

`make` builds the application, `make run` starts it, and `make test` runs the domain and workflow checks. I want the domain checks to build without GUI or FFmpeg development libraries. Windows distribution includes the executable, the required runtime DLLs, and third-party notices. I will verify the FFmpeg build's H.264 encoder availability and distribution terms before packaging it. The application checks for the required encoder at export setup and gives a clear error if the selected profile is unavailable.

## Architecture and responsibilities

I keep the use cases independent of the GUI and media library. Presentation and infrastructure depend inward on application contracts, while application depends on domain types and algorithms. I keep the composition root in presentation to construct the adapters and inject them into the use cases. The rest of presentation calls application interfaces directly.

| Directory | Responsibility |
| --- | --- |
| `src/domain` | Owns frame views, rectangles, spans, block grids, masks, motion correspondences, and restoration parameters. Implements detection, tracking, reconstruction, and masked composition on CPU buffers. |
| `src/application` | Owns the editing session, scan and export plans, cache policy, progress, and cancellation. Defines media reader and writer ports in terms of application and domain types. |
| `src/infrastructure` | Implements the media ports with FFmpeg, including decoder draining, exact seeks, color conversion, timestamp handling, and output file lifecycle. |
| `src/presentation` | Hosts the SDL2 event loop and Dear ImGui views. Converts gestures into edits, requests previews, and displays scan results and export status. |

I use a stride-aware RGB frame view at the domain boundary. The initial processing path uses 8-bit full-range RGB buffers and floating-point working samples in the normalized range from 0 to 1. Detection and motion matching use luma derived from those samples. I use the decoded video color metadata for conversion and initially model mosaic averaging in encoded RGB sample space, where typical editing effects operate. Input mastered for HDR or interlaced presentation requires a separate conversion design.

The media reader returns one frame in presentation order with its ordinal, presentation timestamp, and duration. It can decode sequentially or seek to a preceding keyframe and decode forward to the requested ordinal. The writer receives frames in presentation order and owns encoder draining and packet muxing. Neither contract exposes FFmpeg pointers to the inner layers.

## Frame, region, and edit semantics

I show frame numbers starting at 1 in the GUI. A user selecting frames 12 through 18 edits all seven frames. Internally I store the range as zero-based `[11, 18)`. Frame ordinals are derived from decoder output in presentation order. I keep the media timestamps separately so variable frame rate and reordered packets do not change which frames an edit targets. I build the ordinal-to-timestamp index as decoding proceeds and show which parts of the timeline have been indexed.

I use integer rectangles with half-open edges in source raster coordinates. The origin is the top-left of the decoded raster, and a width or height of one pixel is valid. Presentation inverts letterboxing, display aspect ratio, and supported quarter-turn orientation metadata when I draw or resize an ROI. I clip the rectangle to the source raster and show the resulting coordinates. I reject empty regions, reversed frame ranges, and invalid block sizes before analysis. The manual minimum block width and height are one pixel, and at least one dimension must be greater than one.

A block grid stores independent width, height, and X and Y phases relative to the raster origin. I normalize phases with nonnegative modulo. For width `w` and phase `px`, vertical grid boundaries occur at `px + k*w` for integer `k`, and I intersect cells with the raster edges. The grid and edit mask are separate. Grid geometry defines observations, while the mask determines which output pixels I permit the application to change. An ROI that crosses a grid boundary never expands the editable area.

An edit contains a frame span, an optional fixed ROI, mode, and per-frame masks. Automatic mode derives masks and tracks for the selected scope. Manual mode uses the ROI or painted selection and the supplied grid. In automatic mode I can accept or exclude a track, override a specific frame or range, and paint or erase pixels. A pinned manual grid overrides the detector for that edit. Explicit pixel overrides take precedence over automatic masks in either mode. The selected frame span and ROI still limit the final write mask. I leave unreviewed low-confidence candidates disabled in the export plan.

## Automatic detection

I first score whether neighboring luma samples differ more at grid boundaries than inside cells. At each candidate horizontal spacing `w` and phase `px`, I compare the mean robust absolute X difference on that boundary class with the corresponding mean on interior classes. I score `h` and `py` using Y differences in the same way. These means are normalized by sample counts so large periods cannot win merely by having fewer proposed boundaries. I start with a configurable search from 2 through 64 pixels per axis, limited by the available observations. I consider a one-pixel axis when the perpendicular direction has strong repeated boundary evidence.

I generate joint grid candidates from the leading scores on both axes and evaluate them in overlapping image windows. A good candidate has repeated boundary support, low within-cell variance, and coherent phase across neighboring windows. I penalize aliases when a smaller spacing explains the same repeated boundaries. I retain only spatially connected supported cells, merge adjacent masks when their grids agree, and keep scores and the required evidence count alongside each result. I ignore flat windows with too little contrast to distinguish a grid. Regions with insufficient observations remain available for manual assignment, including ROIs smaller than one cell.

I use the detector's score as a measure of evidence, and calibrate its default acceptance cutoff on generated mosaics and clean textures. Edge-aligned features such as windows and brickwork are especially useful negative fixtures. For a very small ROI, I may analyze a surrounding margin to estimate phase and period, while clipping the returned edit mask to the ROI. I expose the detected dimensions, phase, candidate overlay, and the reason an ambiguous candidate needs review.

## Tracking and temporal alignment

I keep a track ID across frames with a position, local mask, grid estimate, and confidence. I match textured, unmasked patches near a tracked region using a coarse-to-fine search with a robust luma difference. I accept a match only when its score is distinct from competing matches and the forward and backward matches agree. I use the median consistent patch displacement to propose the next region, then re-run local grid detection near that region. I estimate the mosaic phase in the new raster independently of the object's displacement.

I run full-scope detection periodically during automatic scanning and after a lost track, and associate new candidates using overlap, predicted position, and grid agreement. This lets me find regions that appear after the first frame and reacquire regions that leave and re-enter the view. A track may be tentative, confirmed, awaiting review, or lost. I end a track at a scene cut detected from a large luma-histogram change and widespread correspondence failure. I also reject correspondences in occlusions and where motion estimates disagree. A manual correction at a keyframe seeds the next analysis pass for that track.

For reconstruction I use local correspondences between a target frame and nearby decoded frames in the same shot. The proposed initial window is eight frames before and eight after, with a 500 ms limit in either direction. I may read beyond the selected editing interval to find observations. The frame interval still limits where edits are written. Cross-frame samples require consistent motion, in-bounds coordinates, and a source mask that confirms the sample is unaffected. If the surface is deforming or the match is unreliable, reconstruction falls back to the other available observations.

## Restoration model

I model observation `y_t` in frame `t` as the block average of an unknown image `x` after mapping that image into the observation frame, plus a residual for quantization, color conversion, and compression. Warping is estimated separately from textured and unaffected areas. When a cell is clipped by the raster, its observation uses only the visible cell footprint. I use block-average constraints only when the observed cell appears uniformly mosaicked and its sampling footprint maps reliably to the target. I reject cells intersected by an uncertain mosaic boundary or an occlusion. For a verified cell spanning the edit boundary, I hold the fixed pixels at their observed values and include them in the cell sum. A fixed pixel can itself be mosaicked, so I downweight that cell constraint when part of its unknown input lies outside the edit mask. I reject temporal constraints whose bilinear footprint extends outside the available target patch.

For each target patch I solve the following convex approximation after fixing the warps, observation weights, and mask.

```text
Minimize over 0 <= x <= 1 with x fixed outside the write mask

  sum_t || sqrt(W_t) (A_t B_t x - y_t) ||_2^2
  + mu * sum_j q_j || S_j x - z_j ||_2^2
  + lambda * TV(x)
```

Here `B_t` maps the target estimate into frame `t` by bilinear sampling, and `A_t` averages samples in accepted grid cells. `W_t` weights cells by detection, alignment, and noise. `S_j` samples the estimate where a valid unmosaicked observation `z_j` is available, and `q_j` is its confidence. I give these direct samples higher weight than indirect block means. `TV` is the sum of isotropic forward-gradient magnitudes over the patch, including edges between unknown and fixed pixels. `mu` controls direct-sample fitting and `lambda` controls smoothing. Independently shifted grids add useful constraints only when they cover the same surface with sufficiently reliable alignment.

I initialize from aligned direct samples where available and the observed block color elsewhere. I use a primal-dual solver with matched forward and adjoint operators and step sizes bounded by an estimate of the combined operator norm. I project each iterate onto the valid sample range and keep the exterior pixels fixed. The initial limits are 60 iterations for preview and 200 for export, with early stopping after five consecutive iterations below a relative change of `1e-4`. I measure convergence and tune those defaults on fixtures before release. I check cancellation between iterations. If temporal evidence is rejected, I use a single-frame solve with block fidelity and the same regularizer.

I compose each output by copying the source pixels and replacing only the intersection of the accepted mask, ROI, and active frame span. I show a confidence overlay that distinguishes direct temporal observations, indirect block evidence, and regularized estimates. Values in this overlay describe evidence quality rather than a probability of recovering the original. A fixed hidden region will often stay smooth even after export.

## User workflow and responsiveness

I want the default flow to be **Open Video → Detect → Review → Export**. The preview contains a scrubber, source-frame number, time, and editable first and last frames. I can draw an ROI at native-pixel zoom and switch between original, processed, and overlay views. The timeline shows detected frame spans, track interruptions, and edits awaiting review. Detailed grid fields and paint tools sit behind a manual adjustment control.

The SDL thread owns all windows, textures, and event processing. A worker owns decoding and analysis, and an export job uses a fixed copy of the accepted edits. The application identifies requests by generation so a late response from a canceled preview cannot overwrite a more recent frame. I cache thumbnails, track summaries, and a bounded set of decoded frames. I represent long sequences of unchanged mask geometry compactly and retain pixel masks only where needed. The decoded-frame cache has a proposed 256 MiB default budget. If the requested temporal window exceeds the budget, I reduce the usable neighbors rather than buffer the whole video. I check cancellation between decoded frames, search batches, and solver iterations. The launch target for measuring responsiveness is 1080p video. I will record the test hardware, scan time, preview latency, export throughput, and peak memory before setting performance guarantees.

## Export and failure handling

The default output is MP4 with H.264 video. For exact pixel comparison I provide a separate FFV1 in Matroska profile with a lossless RGB-compatible pixel format. I verify exact composition in the processing raster before either encoder runs. H.264 encoding with chroma subsampling or lossy compression may change decoded values outside the edit mask. The exact-output check compares decoded FFV1 pixels against the composed processing raster, including the untouched region and frames.

I preserve the relative stream timeline using the decoded video display timestamps and durations, rescaled into the encoder and muxer time bases. I preserve variable frame intervals and use the same time-origin adjustment for video and audio. I drain reordered decoder and encoder frames and let the encoder supply output packet decode timestamps. I preserve supported display orientation and aspect-ratio metadata because the processing raster remains in source orientation. I copy the selected audio stream when accepted by the output container. If it is incompatible, I report the reason before export and allow the user to explicitly export without audio or select another compatible profile.

I write output to an exclusively created temporary sibling file on the destination filesystem and publish it after encoding, trailer writing, and close succeed. I check file identity to avoid overwriting the input through a different path. I ask for overwrite confirmation before starting and use the platform's same-volume replacement operation when overwriting an existing destination. If the destination appears after export starts and no overwrite was approved, I stop before publication. I remove the temporary file after cancel or recoverable failure. Existing output remains available if encoding, trailer writing, or publication fails. Unreadable files, unsupported formats, missing encoders, and disk failures are surfaced in the GUI with the failed operation and whatever corrective context the application can determine.

## Validation and handoff

I use deterministic generated fixtures so I can compare output against known clean frames and exact masks. I measure quality inside the true mask separately from detector accuracy. The reconstruction baseline is the observed mosaic cropped to that mask, and I compare masked mean absolute error and PSNR against it. I also keep fixtures with no hidden-detail evidence to verify the estimation fallback and its display.

| Area | Acceptance check |
| --- | --- |
| Grid detection | Recover rectangular and square dimensions and phase on clean synthetic mosaics, including an 8 by 12 grid with a nonzero phase. Exercise aliasing, clipped cells, mild compression, flat imagery, and clean periodic textures. Record precision and recall before selecting the automatic cutoff. |
| Range and mask | Select frames 12 through 18 and a small ROI crossing cell edges. Verify exact equality before encoding outside the accepted mask and on every out-of-range frame. Verify that exclusion overrides a detection and that a one-pixel selection changes at most one pixel. |
| Tracking | Verify mask overlap against moving ground truth, then exercise a scene cut, occlusion, low-texture region, and a later mosaic appearance. Verify that conflicting matches end or downgrade a track. |
| Restoration | On exactly aligned direct-observation fixtures, recover the source pixels within one level of 8-bit RGB. On generated shifted-grid fixtures, reduce masked error relative to the observed mosaic. On fixed-grid fixtures, verify bounded output, and report limited evidence in the overlay. |
| Media I/O | Export a video with variable frame intervals, reordered pictures, and compatible audio. Verify frame count, presentation order, and relative timestamps within muxer time-base rounding. Decode the lossless profile and compare raster pixels exactly. |
| GUI and export lifecycle | On Windows 11 26H2 and Linux, exercise open, scrub, ROI selection, detect, override, preview, cancel, and export. Verify that a failed or canceled export preserves a pre-existing destination. |

My first milestone is the four-layer skeleton and a reproducible Windows and Linux build. I will bring up FFmpeg decode, the frame scrubber, exact ROI and frame selection, and masked export with a manual grid. This gives the next developer a working path from input to saved output and fixes the coordinate and time conventions early.

My second milestone is single-frame grid detection and reconstruction. I will implement the block observation and its adjoint, add TV restoration, and verify output boundaries and detector behavior on generated fixtures. I will expose candidate review and manual overrides alongside the original and reconstructed previews.

My third milestone is full-video detection and tracking with motion-compensated observations. I will add track correction, shifted-grid fitting, and a bounded streaming workflow, then check scene boundaries and edits across multiple timestamps.

My release milestone is the Windows package and an end-to-end validation run on both platforms. I will record the tested build and dependency versions, packaging steps, fixture results, performance measurements, and remaining input limitations so the next maintainer can reproduce the release.
