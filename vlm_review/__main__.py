#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""``python3 -m vlm_review`` 入口(等价于 ``python3 -m vlm_review.server``)."""

from .server import main

if __name__ == "__main__":
    raise SystemExit(main())
