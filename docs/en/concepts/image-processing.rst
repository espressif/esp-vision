Image Processing
================

:link_to_translation:`zh_CN:[中文]`

The vision algorithms in the :doc:`../api-reference/image` module come from
OpenMV's ``imlib`` library, maintained in this repository as the
``components/imlib`` IDF component. They fall into a few families: point and
geometric transforms, neighborhood filters, statistical analysis, and feature
detection. This page explains what each family does and when to reach for it.

A typical processing pipeline
-----------------------------

Most color-tracking or inspection scripts follow the same shape:

#. **Capture** a frame with :py:func:`sensor.snapshot`.
#. **Pre-process** to reduce noise and normalize: convert with
   :py:meth:`image.Image.to_grayscale`, smooth with
   :py:meth:`image.Image.gaussian`, optionally correct lighting with
   :py:meth:`image.Image.histeq`.
#. **Segment** the pixels of interest with :py:meth:`image.Image.binary` against
   a color threshold.
#. **Analyze** the result: :py:meth:`image.Image.find_blobs`,
   :py:meth:`image.Image.get_statistics`, or a feature detector.
#. **Annotate / act** using the drawing methods.

Thresholding and segmentation
-----------------------------

:py:meth:`image.Image.binary` converts the image into a mask by testing each
pixel against a list of color thresholds (see :doc:`image-model` for why
thresholds are expressed in LAB). A pixel is "on" if it falls inside *any* of
the supplied thresholds, which lets you track several colors at once. The
companion :py:meth:`image.Image.find_blobs` runs an 8-connected component
labeling pass over the same thresholds and returns one :py:class:`image.blob`
per connected region, with centroid, bounding box, pixel count, orientation, and
shape descriptors such as roundness and solidity.

Neighborhood (convolution) filters
-----------------------------------

These replace each pixel with a function of its ``(2*ksize+1)`` square
neighborhood:

- :py:meth:`image.Image.mean` -- box average; fast blur.
- :py:meth:`image.Image.gaussian` -- weighted average using a Pascal-triangle
  (binomial) approximation of a Gaussian; the standard noise-reduction blur. Set
  ``unsharp=True`` to sharpen instead.
- :py:meth:`image.Image.laplacian` -- second-derivative edge response; set
  ``sharpen=True`` to add it back to the original for edge enhancement.
- :py:meth:`image.Image.morph` -- apply an arbitrary integer kernel. ``mul`` and
  ``add`` scale and bias the result; the default ``mul`` is ``1/sum(kernel)`` so
  the image brightness is preserved.

All of these share ``threshold``/``offset``/``invert`` keywords that turn the
filter into an adaptive thresholding operation in a single pass.

Rank and edge-preserving filters
---------------------------------

Rank filters sort the neighborhood instead of averaging it, which removes noise
without blurring edges as much:

- :py:meth:`image.Image.median` -- the percentile (default 0.5) value; excellent
  against salt-and-pepper noise.
- :py:meth:`image.Image.mode` -- the most common value.
- :py:meth:`image.Image.midpoint` -- a ``bias`` blend between the min and max.
- :py:meth:`image.Image.bilateral` -- a Gaussian blur weighted by both spatial
  distance (``space_sigma``) and color similarity (``color_sigma``), so it
  smooths flat regions while keeping edges crisp.

Morphology
----------

:py:meth:`image.Image.erode` and :py:meth:`image.Image.dilate` grow or shrink
the set pixels of a (usually binary) image using a square structuring element;
:py:meth:`image.Image.open` and :py:meth:`image.Image.close` combine them to
remove specks or fill small holes. The ``threshold`` argument controls how many
neighbors must be set, generalizing the classic binary operators.

Histograms and statistics
-------------------------

:py:meth:`image.Image.get_histogram` bins pixel values per channel; the returned
:py:class:`image.histogram` can compute Otsu thresholds
(:py:meth:`image.histogram.get_threshold`), percentiles, and full statistics.
:py:meth:`image.Image.get_statistics` returns mean, median, mode, standard
deviation, quartiles, and min/max in one call, which is useful for auto-tuning
thresholds at runtime.

Feature detection
-----------------

Higher-level detectors find geometric structure:

- :py:meth:`image.Image.find_lines` and :py:meth:`image.Image.find_circles` use
  the Hough transform; their ``threshold`` is the minimum accumulator score and
  the ``*_margin`` keywords merge near-duplicate results.
- :py:meth:`image.Image.find_rects` locates quadrilaterals (useful for fiducials
  and screens).
- :py:meth:`image.Image.find_qrcodes` and
  :py:meth:`image.Image.find_apriltags` detect and decode the corresponding
  marker types; AprilTags can additionally return 6-DoF pose when camera
  intrinsics are supplied.

.. only:: esp32p4

   The current ESP32-P4 board profiles additionally provide
   :py:meth:`image.Image.find_barcodes` through the ZXing-C++ backend.

.. note::

   GPL-licensed OpenMV code paths are disabled in this build (``OMV_NO_GPL=1``),
   so a small number of upstream algorithms are intentionally unavailable. The
   per-board ``imlib_config.h`` selects which optional algorithms are compiled
   in; a method that is not enabled raises an exception at call time.
