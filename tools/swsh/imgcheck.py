"""imgcheck.py: image checks for the regression suite (numpy + Pillow).

Regions are fractions of the image (x0, y0, x1, y1), so a check holds at any render resolution.
  luma(path, region)            mean luma 0..255 of the region
  black_fraction(path, region)  share of the region's pixels with luma < 8
  upright_margin(a, b)          corr(a, b) - corr(a, b flipped vertically), on 160x90 grey thumbnails: > 0 when b is
                                the same scene as a and the right way up (G2: the scene-change snapshot drawn upside down)
  ssim(a, b)                    mean SSIM of the grey 320x180 thumbnails (8x8 windows)
"""
import numpy as np
from PIL import Image


def _grey(path, size=None):
    im = Image.open(path).convert("L")
    if size:
        im = im.resize(size, Image.BILINEAR)
    return np.asarray(im, dtype=np.float64)


def _crop(arr, region):
    h, w = arr.shape[:2]
    x0, y0, x1, y1 = region
    return arr[int(y0 * h):max(int(y1 * h), int(y0 * h) + 1), int(x0 * w):max(int(x1 * w), int(x0 * w) + 1)]


def luma(path, region=(0, 0, 1, 1)):
    return float(_crop(_grey(path), region).mean())


def black_fraction(path, region=(0, 0, 1, 1)):
    return float((_crop(_grey(path), region) < 8).mean())


def _corr(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d else 0.0


def upright_margin(a, b):
    ga, gb = _grey(a, (160, 90)), _grey(b, (160, 90))
    return _corr(ga, gb) - _corr(ga, gb[::-1])


def ssim(a, b):
    x, y = _grey(a, (320, 180)), _grey(b, (320, 180))
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    vals = []
    for j in range(0, 180 - 7, 8):
        for i in range(0, 320 - 7, 8):
            p, q = x[j:j + 8, i:i + 8], y[j:j + 8, i:i + 8]
            mp, mq = p.mean(), q.mean()
            vp, vq, cov = p.var(), q.var(), ((p - mp) * (q - mq)).mean()
            vals.append(((2 * mp * mq + c1) * (2 * cov + c2)) / ((mp * mp + mq * mq + c1) * (vp + vq + c2)))
    return float(np.mean(vals))
