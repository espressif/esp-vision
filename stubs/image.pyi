# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from typing import Any, Callable, Sequence, overload

#: 1-bpp black-and-white pixel format (one bit per pixel).
BINARY: int
#: 8-bit grayscale pixel format.
GRAYSCALE: int
#: 16-bit RGB565 color pixel format.
RGB565: int
#: Raw Bayer mosaic pixel format from the sensor.
BAYER: int
#: Packed YUV422 pixel format.
YUV422: int
#: JPEG-compressed image format.
JPEG: int
#: PNG-compressed image format.
PNG: int

#: False-color rainbow palette (cold to hot).
PALETTE_RAINBOW: int
#: "Ironbow" thermal palette.
PALETTE_IRONBOW: int
#: Depth-map palette.
PALETTE_DEPTH: int
#: Event-camera dark palette.
PALETTE_EVT_DARK: int
#: Event-camera light palette.
PALETTE_EVT_LIGHT: int

#: Scaling/geometry hint flags for copy()/crop()/scale()/draw_image(): area-average downscaling.
AREA: int
#: Bilinear interpolation hint.
BILINEAR: int
#: Bicubic interpolation hint.
BICUBIC: int
#: Mirror horizontally.
HMIRROR: int
#: Flip vertically.
VFLIP: int
#: Swap X and Y (transpose).
TRANSPOSE: int
#: Center the source in the destination.
CENTER: int
#: Extract the RGB channel before applying the color palette.
EXTRACT_RGB_CHANNEL_FIRST: int
#: Apply the color palette before other steps.
APPLY_COLOR_PALETTE_FIRST: int
#: Keep aspect ratio, fitting inside the destination.
SCALE_ASPECT_KEEP: int
#: Keep aspect ratio, expanding to fill the destination.
SCALE_ASPECT_EXPAND: int
#: Ignore aspect ratio, stretching to the destination.
SCALE_ASPECT_IGNORE: int
#: Treat the background as black (needed for correct alpha blending).
BLACK_BACKGROUND: int
#: Rotate 90 degrees.
ROTATE_90: int
#: Rotate 180 degrees.
ROTATE_180: int
#: Rotate 270 degrees.
ROTATE_270: int

#: Let the JPEG encoder pick chroma subsampling automatically.
JPEG_SUBSAMPLING_AUTO: int
#: 4:4:4 JPEG chroma subsampling (best quality).
JPEG_SUBSAMPLING_444: int
#: 4:2:2 JPEG chroma subsampling.
JPEG_SUBSAMPLING_422: int
#: 4:2:0 JPEG chroma subsampling (smallest size).
JPEG_SUBSAMPLING_420: int

#: Exhaustive template-search strategy.
SEARCH_EX: int
#: Diamond-search (faster, approximate) template-search strategy.
SEARCH_DS: int
#: Canny edge-detection method for find_edges().
EDGE_CANNY: int
#: Simple gradient-threshold edge-detection method for find_edges().
EDGE_SIMPLE: int
#: FAST corner detector for find_keypoints().
CORNER_FAST: int
#: AGAST corner detector for find_keypoints().
CORNER_AGAST: int
#: Barcode symbology constants returned by barcode.type() / accepted by find_barcodes().
EAN2: int
EAN5: int
EAN8: int
UPCE: int
ISBN10: int
UPCA: int
EAN13: int
ISBN13: int
I25: int
DATABAR: int
DATABAR_EXP: int
CODABAR: int
CODE39: int
PDF417: int
CODE93: int
CODE128: int
#: AprilTag family constants accepted by find_apriltags().
TAG16H5: int
TAG25H7: int
TAG25H9: int
TAG36H10: int
TAG36H11: int

Color = int | tuple[int, int, int]
Point = tuple[int, int]
LineTuple = tuple[int, int, int, int]
CircleTuple = tuple[int, int, int]
EllipseTuple = tuple[int, int, int, int, int]
Rect = tuple[int, int, int, int]
LineLike = LineTuple | Sequence[int]
CircleLike = CircleTuple | Sequence[int]
EllipseLike = EllipseTuple | Sequence[int]
RectLike = Rect | Sequence[int]
#: Color threshold: grayscale (min, max) or LAB (l_min, l_max, a_min, a_max, b_min, b_max).
Threshold = tuple[int, int] | tuple[int, int, int, int, int, int]
#: A list of color thresholds; a pixel matches if it falls inside any of them.
Thresholds = Sequence[Threshold]


#: A detected line segment with Hough-space attributes.
class line:
    #: Return the line as (x1, y1, x2, y2).
    def line(self) -> LineTuple: ...
    #: Start-point x coordinate.
    def x1(self) -> int: ...
    #: Start-point y coordinate.
    def y1(self) -> int: ...
    #: End-point x coordinate.
    def x2(self) -> int: ...
    #: End-point y coordinate.
    def y2(self) -> int: ...
    #: Segment length in pixels.
    def length(self) -> int: ...
    #: Hough accumulator magnitude (line strength).
    def magnitude(self) -> int: ...
    #: Line angle theta in degrees (Hough space).
    def theta(self) -> int: ...
    #: Line distance rho in pixels (Hough space).
    def rho(self) -> int: ...


#: A detected circle.
class circle:
    #: Return the circle as (x, y, r).
    def circle(self) -> CircleTuple: ...
    #: Center x coordinate.
    def x(self) -> int: ...
    #: Center y coordinate.
    def y(self) -> int: ...
    #: Radius in pixels.
    def r(self) -> int: ...
    #: Hough accumulator magnitude (circle strength).
    def magnitude(self) -> int: ...


#: A detected rectangle (quadrilateral).
class rect:
    #: Return the four corner points in clockwise order.
    def corners(self) -> tuple[Point, Point, Point, Point]: ...
    #: Return the bounding box as (x, y, w, h).
    def rect(self) -> Rect: ...
    #: Bounding-box top-left x.
    def x(self) -> int: ...
    #: Bounding-box top-left y.
    def y(self) -> int: ...
    #: Bounding-box width.
    def w(self) -> int: ...
    #: Bounding-box height.
    def h(self) -> int: ...
    #: Detection strength.
    def magnitude(self) -> int: ...


#: A connected color region found by find_blobs().
class blob:
    #: The four bounding-box corners.
    def corners(self) -> tuple[Point, Point, Point, Point]: ...
    #: The four corners of the minimum-area enclosing rectangle.
    def min_corners(self) -> tuple[Point, Point, Point, Point]: ...
    #: Bounding box as (x, y, w, h).
    def rect(self) -> Rect: ...
    #: Bounding-box top-left x.
    def x(self) -> int: ...
    #: Bounding-box top-left y.
    def y(self) -> int: ...
    #: Bounding-box width.
    def w(self) -> int: ...
    #: Bounding-box height.
    def h(self) -> int: ...
    #: Number of pixels in the blob.
    def pixels(self) -> int: ...
    #: Centroid x (integer).
    def cx(self) -> int: ...
    #: Centroid x (float).
    def cxf(self) -> float: ...
    #: Centroid y (integer).
    def cy(self) -> int: ...
    #: Centroid y (float).
    def cyf(self) -> float: ...
    #: Orientation of the blob's major axis in radians.
    def rotation(self) -> float: ...
    #: Orientation in degrees.
    def rotation_deg(self) -> float: ...
    #: Orientation in radians.
    def rotation_rad(self) -> float: ...
    #: Bitmask of the thresholds this blob matched.
    def code(self) -> int: ...
    #: Number of blobs merged into this one.
    def count(self) -> int: ...
    #: Bounding-box perimeter.
    def perimeter(self) -> int: ...
    #: Roundness metric in [0, 1] (1 == circle).
    def roundness(self) -> float: ...
    #: Elongation metric in [0, 1].
    def elongation(self) -> float: ...
    #: Bounding-box area (w * h).
    def area(self) -> int: ...
    #: Pixel density: pixels / area.
    def density(self) -> float: ...
    #: Alias of density().
    def extent(self) -> float: ...
    #: Compactness metric in [0, 1].
    def compactness(self) -> float: ...
    #: Solidity (pixels / convex-hull area) in [0, 1].
    def solidity(self) -> float: ...
    #: Convexity metric in [0, 1].
    def convexity(self) -> float: ...
    #: Horizontal projection histogram bins.
    def x_hist_bins(self) -> list[int]: ...
    #: Vertical projection histogram bins.
    def y_hist_bins(self) -> list[int]: ...
    #: Major-axis line as (x1, y1, x2, y2).
    def major_axis_line(self) -> LineTuple: ...
    #: Minor-axis line as (x1, y1, x2, y2).
    def minor_axis_line(self) -> LineTuple: ...
    #: Minimum enclosing circle as (x, y, r).
    def enclosing_circle(self) -> CircleTuple: ...
    #: Best-fit enclosed ellipse as (x, y, rx, ry, rotation).
    def enclosed_ellipse(self) -> EllipseTuple: ...


#: A decoded QR code found by find_qrcodes().
class qrcode:
    #: The four corner points.
    def corners(self) -> tuple[Point, Point, Point, Point]: ...
    #: Bounding box as (x, y, w, h).
    def rect(self) -> Rect: ...
    #: Bounding-box top-left x.
    def x(self) -> int: ...
    #: Bounding-box top-left y.
    def y(self) -> int: ...
    #: Bounding-box width.
    def w(self) -> int: ...
    #: Bounding-box height.
    def h(self) -> int: ...
    #: Decoded payload string.
    def payload(self) -> str: ...
    #: QR version (size) number.
    def version(self) -> int: ...
    #: Error-correction level.
    def ecc_level(self) -> int: ...
    #: Data mask pattern index.
    def mask(self) -> int: ...
    #: Encoding mode of the payload.
    def data_type(self) -> int: ...
    #: Extended Channel Interpretation value.
    def eci(self) -> int: ...
    #: True if the payload is numeric.
    def is_numeric(self) -> bool: ...
    #: True if the payload is alphanumeric.
    def is_alphanumeric(self) -> bool: ...
    #: True if the payload is binary.
    def is_binary(self) -> bool: ...
    #: True if the payload is Kanji.
    def is_kanji(self) -> bool: ...


#: A decoded 1D/2D barcode found by find_barcodes().
class barcode:
    #: The four corner points.
    def corners(self) -> tuple[Point, Point, Point, Point]: ...
    #: Bounding box as (x, y, w, h).
    def rect(self) -> Rect: ...
    #: Bounding-box top-left x.
    def x(self) -> int: ...
    #: Bounding-box top-left y.
    def y(self) -> int: ...
    #: Bounding-box width.
    def w(self) -> int: ...
    #: Bounding-box height.
    def h(self) -> int: ...
    #: Decoded payload string.
    def payload(self) -> str: ...
    #: Symbology type constant (e.g. EAN13, CODE128).
    def type(self) -> int: ...
    #: Rotation of the barcode in degrees.
    def rotation(self) -> int: ...
    #: Decode quality (number of agreeing scan lines).
    def quality(self) -> int: ...


#: A detected AprilTag with optional 6-DoF pose. Attributes are fields, not methods.
class apriltag:
    #: The four corner points.
    corners: tuple[Point, Point, Point, Point]
    #: Bounding box as (x, y, w, h).
    rect: Rect
    x: int
    y: int
    w: int
    h: int
    #: Bounding-box area.
    area: int
    #: Tag ID within its family.
    id: int
    #: Tag family constant.
    family: int
    #: Family name string.
    name: str
    #: Centroid x (integer) and float variant.
    cx: int
    cxf: float
    cy: int
    cyf: float
    #: In-plane rotation in radians.
    rotation: float
    #: Decision margin (detection confidence).
    decision_margin: float
    #: Number of corrected bit errors.
    hamming: int
    #: Detection goodness metric.
    goodness: float
    #: Estimated translation along X (requires camera intrinsics).
    x_translation: float
    y_translation: float
    z_translation: float
    #: Estimated rotation about X in radians.
    x_rotation: float
    y_rotation: float
    z_rotation: float


#: Channel histogram returned by get_histogram(). For RGB565 the L/A/B
#: channels correspond to the LAB color space.
class histogram:
    #: Combined bins (grayscale/binary) or L bins (RGB565).
    def bins(self) -> list[float]: ...
    #: L-channel (lightness) bins.
    def l_bins(self) -> list[float]: ...
    #: A-channel bins (RGB565 only).
    def a_bins(self) -> list[float]: ...
    #: B-channel bins (RGB565 only).
    def b_bins(self) -> list[float]: ...
    #: Return the value at the given cumulative percentile (0..1).
    #: p: percentile in the range 0 to 1.
    def get_percentile(self, p: float) -> percentile: ...
    #: Compute an Otsu threshold from the histogram.
    def get_threshold(self) -> threshold: ...
    #: Compute statistics (mean/median/mode/stdev/...) from the histogram.
    def get_stats(self) -> statistics: ...
    #: Alias of get_stats().
    def get_statistics(self) -> statistics: ...
    #: Alias of get_stats().
    def statistics(self) -> statistics: ...


#: A percentile result from histogram.get_percentile().
class percentile:
    #: Combined/L value at the percentile.
    def value(self) -> int: ...
    #: L-channel value.
    def l_value(self) -> int: ...
    #: A-channel value.
    def a_value(self) -> int: ...
    #: B-channel value.
    def b_value(self) -> int: ...


#: An Otsu threshold result from histogram.get_threshold().
class threshold:
    #: Combined/L threshold value.
    def value(self) -> int: ...
    #: L-channel threshold.
    def l_value(self) -> int: ...
    #: A-channel threshold.
    def a_value(self) -> int: ...
    #: B-channel threshold.
    def b_value(self) -> int: ...


#: Region statistics returned by get_statistics(). Combined accessors apply to
#: grayscale/binary; ``l_*``/``a_*``/``b_*`` apply to RGB565 LAB channels.
class statistics:
    def mean(self) -> int: ...
    def median(self) -> int: ...
    def mode(self) -> int: ...
    def stdev(self) -> int: ...
    def min(self) -> int: ...
    def max(self) -> int: ...
    #: Lower quartile (25th percentile).
    def lq(self) -> int: ...
    #: Upper quartile (75th percentile).
    def uq(self) -> int: ...
    def l_mean(self) -> int: ...
    def l_median(self) -> int: ...
    def l_mode(self) -> int: ...
    def l_stdev(self) -> int: ...
    def l_min(self) -> int: ...
    def l_max(self) -> int: ...
    def l_lq(self) -> int: ...
    def l_uq(self) -> int: ...
    def a_mean(self) -> int: ...
    def a_median(self) -> int: ...
    def a_mode(self) -> int: ...
    def a_stdev(self) -> int: ...
    def a_min(self) -> int: ...
    def a_max(self) -> int: ...
    def a_lq(self) -> int: ...
    def a_uq(self) -> int: ...
    def b_mean(self) -> int: ...
    def b_median(self) -> int: ...
    def b_mode(self) -> int: ...
    def b_stdev(self) -> int: ...
    def b_min(self) -> int: ...
    def b_max(self) -> int: ...
    def b_lq(self) -> int: ...
    def b_uq(self) -> int: ...


#: A 2D image backed by a pixel buffer.
#: Create one from a file path, from a width/height/pixformat, or from an
#: array; most often you get one from sensor.snapshot(). Most in-place methods
#: return ``self`` so calls can be chained.
class Image:
    #: Load an image from a file (BMP/PPM/PGM/JPEG/PNG depending on build).
    #: path: source file path.
    #: copy_to_fb: True loads the decoded image into the frame buffer.
    @overload
    def __init__(self, path: str, *, copy_to_fb: bool = False) -> None: ...
    #: Allocate a blank image of the given size and pixel format.
    #: width: image width in pixels.
    #: height: image height in pixels.
    #: pixformat: one of BINARY/GRAYSCALE/RGB565/BAYER/YUV422.
    #: buffer: optional external buffer to wrap instead of allocating.
    #: copy_to_fb: True allocates the image in the frame buffer.
    @overload
    def __init__(
        self,
        width: int,
        height: int,
        pixformat: int,
        *,
        buffer: Any | None = None,
        copy_to_fb: bool = False,
    ) -> None: ...
    #: Wrap an existing array/buffer (e.g. an ndarray) as an image.
    @overload
    def __init__(self, array: Any, *, buffer: Any | None = None, copy_to_fb: bool = False) -> None: ...

    #: Image width in pixels.
    def width(self) -> int: ...
    #: Image height in pixels.
    def height(self) -> int: ...
    #: Pixel format constant (BINARY/GRAYSCALE/RGB565/...).
    def format(self) -> int: ...
    #: Size of the pixel buffer in bytes.
    def size(self) -> int: ...
    #: Return the pixel buffer as a bytearray (no copy).
    def bytearray(self) -> bytearray: ...
    #: Read one pixel.
    #: x, y: pixel coordinates.
    #: rgbtuple: for RGB565, True returns an (r, g, b) tuple, False the packed value.
    def get_pixel(self, x: int, y: int, *, rgbtuple: bool = True) -> Any: ...
    #: Write one pixel.
    #: x, y: pixel coordinates.
    #: pixel: grayscale value or (r, g, b) color.
    def set_pixel(self, x: int, y: int, pixel: Color) -> Image: ...
    #: Push this image to the host preview over USB CDC (EVFRAME JPEG stream).
    def flush(self) -> None: ...
    #: Save the image to a file; the format is inferred from the extension.
    #: path: destination path.
    #: roi: optional source rectangle (x, y, w, h).
    #: quality: JPEG quality from 1 to 100 when saving as JPEG.
    def save(self, path: str, *, roi: RectLike | None = None, quality: int = 50) -> Image: ...

    #: Return a scaled/cropped/converted copy of the image.
    #: roi: source rectangle (x, y, w, h) to read from.
    #: x_scale, y_scale: horizontal/vertical scale factors.
    #: hint: bitwise OR of AREA/BILINEAR/BICUBIC/HMIRROR/VFLIP/TRANSPOSE/ROTATE_*/SCALE_ASPECT_* flags.
    #: rgb_channel: extract a single RGB channel (0, 1, or 2; -1 for all).
    #: alpha: blend alpha from 0 to 255.
    #: copy: True returns a new image; False converts in place.
    def copy(
        self,
        *,
        roi: RectLike | None = None,
        x_scale: float | None = None,
        y_scale: float | None = None,
        rgb_channel: int = -1,
        alpha: int = 255,
        color_palette: Any | None = None,
        alpha_palette: Any | None = None,
        hint: int = 0,
        transform: Any | None = None,
        copy: bool = True,
    ) -> Image: ...
    #: Crop/scale the image to a region of interest. Same options as copy().
    def crop(
        self,
        *,
        roi: RectLike | None = None,
        x_scale: float | None = None,
        y_scale: float | None = None,
        hint: int = 0,
        copy: bool = False,
    ) -> Image: ...
    #: Scale the image by x_scale/y_scale (alias of crop in this build).
    def scale(
        self,
        *,
        x_scale: float | None = None,
        y_scale: float | None = None,
        roi: RectLike | None = None,
        hint: int = 0,
        copy: bool = False,
    ) -> Image: ...
    #: Compress the image to JPEG in place (alias of to_jpeg).
    #: quality: JPEG quality from 1 to 100.
    #: subsampling: one of the JPEG_SUBSAMPLING_* constants.
    def compress(self, *, quality: int = 90, subsampling: int = ..., roi: RectLike | None = None) -> Image: ...
    #: Convert to a 1-bpp bitmap.
    def to_bitmap(self, *, copy: bool = False, roi: RectLike | None = None) -> Image: ...
    #: Convert to grayscale.
    def to_grayscale(self, *, copy: bool = False, roi: RectLike | None = None) -> Image: ...
    #: Convert to RGB565.
    def to_rgb565(self, *, copy: bool = False, roi: RectLike | None = None) -> Image: ...
    #: Apply the rainbow palette and convert to RGB565.
    def to_rainbow(self, *, copy: bool = False, roi: RectLike | None = None) -> Image: ...
    #: Apply the ironbow thermal palette and convert to RGB565.
    def to_ironbow(self, *, copy: bool = False, roi: RectLike | None = None) -> Image: ...
    #: Encode to JPEG.
    #: quality: JPEG quality from 1 to 100.
    #: subsampling: one of the JPEG_SUBSAMPLING_* constants.
    def to_jpeg(self, *, quality: int = 90, subsampling: int = ..., copy: bool = False) -> Image: ...
    #: Encode to PNG.
    def to_png(self, *, copy: bool = False) -> Image: ...

    #: Fill the image with zeros (optionally only where mask is set).
    #: mask: optional 1-bpp image limiting which pixels are cleared.
    def clear(self, *, mask: Image | None = None) -> Image: ...
    @overload
    def draw_line(self, x0: int, y0: int, x1: int, y1: int, color: Color = ..., thickness: int = 1) -> Image: ...
    @overload
    def draw_line(self, line: LineLike, color: Color = ..., thickness: int = 1) -> Image: ...
    @overload
    def draw_rectangle(
        self,
        x: int,
        y: int,
        w: int,
        h: int,
        color: Color = ...,
        thickness: int = 1,
        fill: bool = False,
    ) -> Image: ...
    @overload
    def draw_rectangle(self, rect: RectLike, color: Color = ..., thickness: int = 1, fill: bool = False) -> Image: ...
    @overload
    def draw_circle(
        self,
        x: int,
        y: int,
        r: int,
        color: Color = ...,
        thickness: int = 1,
        fill: bool = False,
    ) -> Image: ...
    @overload
    def draw_circle(self, circle: CircleLike, color: Color = ..., thickness: int = 1, fill: bool = False) -> Image: ...
    @overload
    def draw_ellipse(
        self,
        x: int,
        y: int,
        rx: int,
        ry: int,
        rotation: int,
        color: Color = ...,
        thickness: int = 1,
        fill: bool = False,
    ) -> Image: ...
    @overload
    def draw_ellipse(
        self,
        ellipse: EllipseLike,
        color: Color = ...,
        thickness: int = 1,
        fill: bool = False,
    ) -> Image: ...
    #: Draw text using the built-in bitmap font.
    #: x, y: top-left position of the text.
    #: text: string to draw.
    #: color: text color.
    #: scale: integer/float scale factor of the font.
    def draw_string(
        self,
        x: int,
        y: int,
        text: str,
        color: Color = ...,
        scale: float = 1.0,
        x_spacing: int = 0,
        y_spacing: int = 0,
        mono_space: bool = True,
        char_rotation: int = 0,
        char_hmirror: bool = False,
        char_vflip: bool = False,
        string_rotation: int = 0,
        string_hmirror: bool = False,
        string_vflip: bool = False,
    ) -> Image: ...
    #: Draw a cross marker centered at (x, y).
    def draw_cross(self, x: int, y: int, color: Color = ..., size: int = 5, thickness: int = 1) -> Image: ...
    @overload
    def draw_arrow(
        self,
        x0: int,
        y0: int,
        x1: int,
        y1: int,
        color: Color = ...,
        size: int = 10,
        thickness: int = 1,
    ) -> Image: ...
    @overload
    def draw_arrow(self, line: LineLike, color: Color = ..., size: int = 10, thickness: int = 1) -> Image: ...
    #: Draw connecting edges between a sequence of corner points.
    def draw_edges(
        self,
        corners: Sequence[Point],
        color: Color = ...,
        size: int = 0,
        thickness: int = 1,
        fill: bool = False,
    ) -> Image: ...
    #: Draw a set of keypoints.
    def draw_keypoints(
        self,
        keypoints: Sequence[tuple[int, int, int]] | Any,
        color: Color = ...,
        size: int = 10,
        thickness: int = 1,
        fill: bool = False,
    ) -> Image: ...
    #: Alpha-blend/scale another image (or a solid color) onto this one.
    #: image: source Image, an (r, g, b) tuple, or a packed scalar color.
    #: x, y: destination top-left position.
    #: x_scale, y_scale: scale factors; roi: source rectangle; alpha: 0..255;
    #: hint: SCALE_ASPECT_*/interpolation flags; mask: optional 1-bpp mask.
    def draw_image(
        self,
        image: Image | Color | int,
        x: int = 0,
        y: int = 0,
        *,
        x_scale: float | None = None,
        y_scale: float | None = None,
        roi: RectLike | None = None,
        rgb_channel: int = -1,
        alpha: int = 255,
        color_palette: Any | None = None,
        alpha_palette: Any | None = None,
        hint: int = 0,
        transform: Any | None = None,
        mask: Image | None = None,
    ) -> Image: ...

    #: Threshold the image into a binary mask against a list of color thresholds.
    #: thresholds: list of grayscale (min, max) or LAB 6-tuple thresholds.
    #: invert: invert the match.
    #: zero: zero out matching pixels instead of setting them.
    #: mask: optional 1-bpp image limiting the operation.
    #: to_bitmap: output a 1-bpp bitmap.
    #: copy: return a new image instead of modifying in place.
    def binary(
        self,
        thresholds: Thresholds,
        *,
        invert: bool = False,
        zero: bool = False,
        mask: Image | None = None,
        to_bitmap: bool = False,
        copy: bool = False,
    ) -> Image: ...
    #: Invert pixel values.
    def invert(self) -> Image: ...
    #: Morphological erosion with a (2*ksize+1) square structuring element.
    #: ksize: structuring-element radius.
    #: threshold: minimum number of set neighbors to keep a pixel.
    #: mask: optional 1-bpp image limiting the operation.
    def erode(self, ksize: int, *, threshold: int = 0, mask: Image | None = None) -> Image: ...
    #: Morphological dilation. See erode() for parameters.
    def dilate(self, ksize: int, *, threshold: int = 0, mask: Image | None = None) -> Image: ...
    #: Morphological opening (erode then dilate).
    def open(self, ksize: int, *, threshold: int = 0, mask: Image | None = None) -> Image: ...
    #: Morphological closing (dilate then erode).
    def close(self, ksize: int, *, threshold: int = 0, mask: Image | None = None) -> Image: ...
    #: Absolute per-pixel difference against another image/color (frame differencing).
    #: image: other Image, (r, g, b) tuple, or scalar color.
    #: x, y: placement of the other image.
    def difference(
        self,
        image: Image | Color | int,
        x: int = 0,
        y: int = 0,
        *,
        roi: RectLike | None = None,
        mask: Image | None = None,
    ) -> Image: ...
    #: Histogram equalization (global). adaptive is not supported in this build.
    #: clip_limit: reserved; mask: optional 1-bpp image.
    def histeq(self, *, adaptive: bool = False, clip_limit: float = -1, mask: Image | None = None) -> Image: ...
    #: Box (mean) blur over a (2*ksize+1) window.
    #: ksize: kernel radius.
    #: threshold/offset/invert: adaptive-threshold the result.
    #: mask: optional 1-bpp image.
    def mean(
        self,
        ksize: int,
        *,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Median filter (edge-preserving denoise).
    #: ksize: kernel radius.
    #: percentile: rank in [0, 1] to select (0.5 == median).
    def median(
        self,
        ksize: int,
        *,
        percentile: float = 0.5,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Mode (most-common value) filter.
    #: ksize: kernel radius.
    def mode(
        self,
        ksize: int,
        *,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Midpoint filter ((min + max)/2 blended by bias).
    #: ksize: kernel radius; bias: 0 == min, 1 == max, 0.5 == midpoint.
    def midpoint(
        self,
        ksize: int,
        *,
        bias: float = 0.5,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Apply an arbitrary NxN convolution kernel.
    #: ksize: kernel radius; the kernel must be (2*ksize+1) square.
    #: kernel: flat or nested sequence of integer weights.
    #: mul: output multiplier (defaults to 1/sum(kernel)); add: output bias.
    def morph(
        self,
        ksize: int,
        kernel: Sequence[int],
        *,
        mul: float | None = None,
        add: float | None = None,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Gaussian blur using a separable Pascal-triangle kernel (alias: gaussian, gaussian_blur).
    #: ksize: kernel radius.
    #: unsharp: True turns the filter into an unsharp-mask sharpener.
    def blur(
        self,
        ksize: int,
        *,
        unsharp: bool = False,
        mul: float | None = None,
        add: float | None = None,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Gaussian blur. See blur() for parameters.
    def gaussian(
        self,
        ksize: int,
        *,
        unsharp: bool = False,
        mul: float | None = None,
        add: float | None = None,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Gaussian blur. See blur() for parameters.
    def gaussian_blur(
        self,
        ksize: int,
        *,
        unsharp: bool = False,
        mul: float | None = None,
        add: float | None = None,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Laplacian edge filter (or sharpener when sharpen is True).
    #: ksize: kernel radius.
    def laplacian(
        self,
        ksize: int,
        *,
        sharpen: bool = False,
        mul: float | None = None,
        add: float | None = None,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...
    #: Bilateral filter (edge-preserving smoothing).
    #: ksize: kernel radius.
    #: color_sigma: range sigma (color closeness); larger blurs across more contrast.
    #: space_sigma: spatial sigma (distance closeness).
    def bilateral(
        self,
        ksize: int,
        *,
        color_sigma: float = 0.1,
        space_sigma: float = 1,
        threshold: bool = False,
        offset: int = 0,
        invert: bool = False,
        mask: Image | None = None,
    ) -> Image: ...

    #: Compute a channel histogram over the image or a region.
    #: thresholds: optional list of thresholds to restrict counted pixels.
    #: invert: invert the threshold match.
    #: roi: region of interest (x, y, w, h).
    #: bins / l_bins / a_bins / b_bins: bin counts; -1 selects the channel default.
    #: difference: optional image to histogram the difference against.
    def get_histogram(
        self,
        thresholds: Thresholds | None = None,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        bins: int = -1,
        l_bins: int = -1,
        a_bins: int = -1,
        b_bins: int = -1,
        difference: Image | None = None,
    ) -> histogram: ...
    #: Alias of get_histogram().
    def get_hist(
        self,
        thresholds: Thresholds | None = None,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        bins: int = -1,
        l_bins: int = -1,
        a_bins: int = -1,
        b_bins: int = -1,
        difference: Image | None = None,
    ) -> histogram: ...
    #: Alias of get_histogram().
    def histogram(
        self,
        thresholds: Thresholds | None = None,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        bins: int = -1,
        l_bins: int = -1,
        a_bins: int = -1,
        b_bins: int = -1,
        difference: Image | None = None,
    ) -> histogram: ...
    #: Compute region statistics (mean/median/mode/stdev/quartiles).
    #: thresholds/invert/roi/bins: as in get_histogram().
    def get_statistics(
        self,
        thresholds: Thresholds | None = None,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        bins: int = -1,
        l_bins: int = -1,
        a_bins: int = -1,
        b_bins: int = -1,
        difference: Image | None = None,
    ) -> statistics: ...
    #: Alias of get_statistics().
    def get_stats(
        self,
        thresholds: Thresholds | None = None,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        bins: int = -1,
        l_bins: int = -1,
        a_bins: int = -1,
        b_bins: int = -1,
        difference: Image | None = None,
    ) -> statistics: ...
    #: Alias of get_statistics().
    def statistics(
        self,
        thresholds: Thresholds | None = None,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        bins: int = -1,
        l_bins: int = -1,
        a_bins: int = -1,
        b_bins: int = -1,
        difference: Image | None = None,
    ) -> statistics: ...
    #: Fit a line to the thresholded pixels via least-squares (or robust) regression.
    #: thresholds: list of color thresholds selecting the pixels to fit.
    #: invert: invert the match; roi: region of interest.
    #: x_stride, y_stride: pixel sampling steps.
    #: area_threshold, pixels_threshold: minimum region area / pixel count to fit.
    #: robust: use a robust (Theil-Sen) estimator.
    def get_regression(
        self,
        thresholds: Thresholds,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        x_stride: int = 2,
        y_stride: int = 1,
        area_threshold: int = 10,
        pixels_threshold: int = 10,
        robust: bool = False,
    ) -> line | None: ...
    #: Find connected color regions (blobs) matching the thresholds.
    #: thresholds: list of color thresholds.
    #: invert: invert the match; roi: region of interest.
    #: x_stride, y_stride: pixel sampling steps.
    #: area_threshold, pixels_threshold: minimum bounding-box area / pixel count.
    #: merge: merge overlapping blobs; margin: extra merge margin.
    #: threshold_cb / merge_cb: optional Python filter/merge callbacks.
    def find_blobs(
        self,
        thresholds: Thresholds,
        *,
        invert: bool = False,
        roi: RectLike | None = None,
        x_stride: int = 2,
        y_stride: int = 1,
        area_threshold: int = 10,
        pixels_threshold: int = 10,
        merge: bool = False,
        margin: int = 0,
        threshold_cb: Callable[[blob], bool] | None = None,
        merge_cb: Callable[[blob, blob], bool] | None = None,
        x_hist_bins_max: int = 0,
        y_hist_bins_max: int = 0,
    ) -> list[blob]: ...
    #: Find straight lines with the Hough transform.
    #: roi: region of interest; x_stride, y_stride: sampling steps.
    #: threshold: minimum Hough accumulator magnitude to report a line.
    #: theta_margin, rho_margin: merge tolerance for similar lines.
    def find_lines(
        self,
        *,
        roi: RectLike | None = None,
        x_stride: int = 2,
        y_stride: int = 1,
        threshold: int = 1000,
        theta_margin: int = 25,
        rho_margin: int = 25,
    ) -> list[line]: ...
    #: Find circles with the Hough transform.
    #: threshold: minimum accumulator magnitude.
    #: x_margin, y_margin, r_margin: merge tolerances.
    #: r_min, r_max, r_step: radius search range and step.
    def find_circles(
        self,
        *,
        roi: RectLike | None = None,
        x_stride: int = 2,
        y_stride: int = 1,
        threshold: int = 2000,
        x_margin: int = 10,
        y_margin: int = 10,
        r_margin: int = 10,
        r_min: int = 2,
        r_max: int = ...,
        r_step: int = 2,
    ) -> list[circle]: ...
    #: Find rectangles/quadrilaterals (e.g. for fiducials).
    #: threshold: minimum edge-magnitude score.
    def find_rects(self, *, roi: RectLike | None = None, threshold: int = 1000) -> list[rect]: ...
    #: Find and decode QR codes.
    #: roi: region of interest.
    def find_qrcodes(self, *, roi: RectLike | None = None) -> list[qrcode]: ...
    #: Find and decode 1D/2D barcodes (requires the barcode backend).
    #: roi: region of interest.
    def find_barcodes(self, *, roi: RectLike | None = None) -> list[barcode]: ...
    #: Find and decode AprilTags, optionally estimating 6-DoF pose.
    #: roi: region of interest; families: OR of TAG* family constants.
    #: fx, fy, cx, cy: camera intrinsics for pose estimation.
    #: pose: True computes translation/rotation (needs intrinsics).
    def find_apriltags(
        self,
        *,
        roi: RectLike | None = None,
        families: int = ...,
        fx: float = ...,
        fy: float = ...,
        cx: float = ...,
        cy: float = ...,
        pose: bool = True,
    ) -> list[apriltag]: ...


#: Convert a 1-bpp binary pixel to a grayscale value.
def binary_to_grayscale(pixel: int) -> int: ...
#: Convert a 1-bpp binary pixel to an (r, g, b) tuple.
def binary_to_rgb(pixel: int) -> tuple[int, int, int]: ...
#: Convert a 1-bpp binary pixel to a LAB tuple.
def binary_to_lab(pixel: int) -> tuple[int, int, int]: ...
#: Convert a 1-bpp binary pixel to a YUV tuple.
def binary_to_yuv(pixel: int) -> tuple[int, int, int]: ...
#: Convert a grayscale value to a 1-bpp binary pixel.
def grayscale_to_binary(pixel: int) -> int: ...
#: Convert a grayscale value to an (r, g, b) tuple.
def grayscale_to_rgb(pixel: int) -> tuple[int, int, int]: ...
#: Convert a grayscale value to a LAB tuple.
def grayscale_to_lab(pixel: int) -> tuple[int, int, int]: ...
#: Convert a grayscale value to a YUV tuple.
def grayscale_to_yuv(pixel: int) -> tuple[int, int, int]: ...
#: Convert an (r, g, b) color to a 1-bpp binary pixel.
def rgb_to_binary(pixel: Color) -> int: ...
#: Convert an (r, g, b) color to a grayscale value.
def rgb_to_grayscale(pixel: Color) -> int: ...
#: Convert an (r, g, b) color to a LAB tuple.
def rgb_to_lab(pixel: Color) -> tuple[int, int, int]: ...
#: Convert an (r, g, b) color to a YUV tuple.
def rgb_to_yuv(pixel: Color) -> tuple[int, int, int]: ...
#: Convert a LAB tuple to a 1-bpp binary pixel.
def lab_to_binary(pixel: Sequence[int]) -> int: ...
#: Convert a LAB tuple to a grayscale value.
def lab_to_grayscale(pixel: Sequence[int]) -> int: ...
#: Convert a LAB tuple to an (r, g, b) tuple.
def lab_to_rgb(pixel: Sequence[int]) -> tuple[int, int, int]: ...
#: Convert a LAB tuple to a YUV tuple.
def lab_to_yuv(pixel: Sequence[int]) -> tuple[int, int, int]: ...
#: Convert a YUV tuple to a 1-bpp binary pixel.
def yuv_to_binary(pixel: Sequence[int]) -> int: ...
#: Convert a YUV tuple to a grayscale value.
def yuv_to_grayscale(pixel: Sequence[int]) -> int: ...
#: Convert a YUV tuple to an (r, g, b) tuple.
def yuv_to_rgb(pixel: Sequence[int]) -> tuple[int, int, int]: ...
#: Convert a YUV tuple to a LAB tuple.
def yuv_to_lab(pixel: Sequence[int]) -> tuple[int, int, int]: ...
