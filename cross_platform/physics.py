"""Small vectorised XPBD cloth used by the Windows and X11 desktop overlay."""

from __future__ import annotations

import math

import numpy as np
from numba import njit


@njit(fastmath=True)
def _ground_heights(pos, bumps):
    result = np.zeros(len(pos), dtype=np.float64)
    for i in range(len(pos)):
        x, y = pos[i, 0], pos[i, 1]
        for j in range(len(bumps)):
            b = bumps[j]
            dx = (x - b[0]) / b[2]
            dy = (y - b[1]) / b[3]
            q = dx * dx + dy * dy
            if q < 1.0:
                result[i] += b[4] * (1.0 - q) ** 0.6
    return result


class Cloth:
    def __init__(self, width: float, height: float, columns: int = 23):
        self.nx = columns
        self.ny = max(4, round(height / (width / (columns - 1))) + 1)
        self.spacing = width / (columns - 1)
        self.width = width
        self.height = self.spacing * (self.ny - 1)
        yy, xx = np.mgrid[0:self.ny, 0:self.nx]
        self.rest = np.stack((xx * self.spacing - width / 2,
                              yy * self.spacing - self.height / 2,
                              np.zeros_like(xx)), axis=-1).reshape(-1, 3).astype(np.float64)
        self.pos = self.rest.copy()
        self.prev = self.pos.copy()
        self.vel = np.zeros_like(self.pos)
        self.mass = np.ones(len(self.pos))
        self.uv = np.stack((xx / (self.nx - 1), yy / (self.ny - 1)), axis=-1).reshape(-1, 2)
        self.groups = self._constraints()
        self.grabbed = np.empty(0, dtype=np.int32)
        self.grab_offsets = np.empty((0, 3))
        self.grab_pos = np.zeros(3)
        self.grab_target = np.zeros(3)
        self.tether = np.zeros(len(self.pos))
        self.max_speed = 1000.0
        self.bumps: list[tuple[float, float, float, float, float]] = []
        self.bump_array = np.empty((0, 5), dtype=np.float64)

    def _constraints(self):
        groups = []
        for dx, dy, compliance in ((1, 0, 0.0), (0, 1, 0.0),
                                   (1, 1, 2e-7), (-1, 1, 2e-7),
                                   (2, 0, 3e-7), (0, 2, 3e-7)):
            # Each colour has no shared vertices, so a vectorised solver is safe.
            for cy in range(max(2, abs(dy) + 1)):
                for cx in range(max(2, abs(dx) + 1)):
                    aa, bb = [], []
                    for y in range(self.ny):
                        for x in range(self.nx):
                            tx, ty = x + dx, y + dy
                            if (x % max(2, abs(dx) + 1) == cx and
                                y % max(2, abs(dy) + 1) == cy and
                                0 <= tx < self.nx and 0 <= ty < self.ny):
                                aa.append(y * self.nx + x)
                                bb.append(ty * self.nx + tx)
                    if aa:
                        a = np.asarray(aa, dtype=np.int32)
                        b = np.asarray(bb, dtype=np.int32)
                        rest = np.linalg.norm(self.rest[b] - self.rest[a], axis=1)
                        groups.append((a, b, rest, compliance))
        return groups

    def layout(self, center: tuple[float, float], angle: float = 0.0, drop: float = 70.0):
        self.end_grab()
        c, s = math.cos(angle), math.sin(angle)
        self.pos[:, 0] = center[0] + self.rest[:, 0] * c - self.rest[:, 1] * s
        self.pos[:, 1] = center[1] + self.rest[:, 0] * s + self.rest[:, 1] * c
        self.pos[:, 2] = drop * (0.55 + 0.45 * self.uv[:, 0])
        self.prev[:] = self.pos
        self.vel[:] = 0
        self.max_speed = 1000

    def pose(self):
        mid = self.ny // 2
        a, b = self.pos[mid * self.nx], self.pos[mid * self.nx + self.nx - 1]
        return self.pos[:, :2].mean(axis=0), math.atan2(b[1] - a[1], b[0] - a[0])

    def rotate(self, angle: float):
        center, _ = self.pose()
        c, s = math.cos(angle), math.sin(angle)
        for points in (self.pos, self.prev):
            delta = points[:, :2] - center
            points[:, 0] = center[0] + delta[:, 0] * c - delta[:, 1] * s
            points[:, 1] = center[1] + delta[:, 0] * s + delta[:, 1] * c
        if len(self.grabbed):
            for point in (self.grab_pos, self.grab_target):
                d = point[:2] - center
                point[:2] = center + (d[0] * c - d[1] * s, d[0] * s + d[1] * c)
        self.max_speed = 1000

    def begin_grab(self, vertex: int, target: np.ndarray):
        self.end_grab()
        x, y = vertex % self.nx, vertex // self.nx
        self.grabbed = np.asarray([j * self.nx + i
                                   for j in range(max(0, y - 1), min(self.ny, y + 2))
                                   for i in range(max(0, x - 1), min(self.nx, x + 2))], dtype=np.int32)
        self.grab_pos = self.pos[vertex].copy()
        self.grab_offsets = self.pos[self.grabbed] - self.grab_pos
        self.grab_offsets[:, 2] = 0
        self.grab_target = target.copy()
        self.tether = np.linalg.norm(self.rest - self.rest[vertex], axis=1) * 1.01
        self.mass[self.grabbed] = 0
        self.max_speed = 1000

    def end_grab(self):
        if len(self.grabbed):
            self.mass[self.grabbed] = 1
            self.vel[self.grabbed] = np.clip(self.vel[self.grabbed], -1000, 1000)
            self.grabbed = np.empty(0, dtype=np.int32)

    def set_bumps(self, bumps):
        self.bumps = bumps
        self.bump_array = np.asarray(bumps, dtype=np.float64).reshape(-1, 5)

    def ground_height(self):
        return _ground_heights(self.pos, self.bump_array)

    def step(self, dt: float):
        dt = min(dt, 1 / 20)
        substeps = 5
        hdt = dt / substeps
        moving = self.mass > 0
        for _ in range(substeps):
            self.vel[moving, 2] -= 5000 * hdt
            self.vel[moving] *= max(0, 1 - 1.3 * hdt)
            speed = np.linalg.norm(self.vel, axis=1)
            cap = 0.2 * self.spacing / hdt
            self.vel *= np.minimum(1, cap / np.maximum(speed, 1))[:, None]
            self.prev[:] = self.pos
            self.pos[moving] += self.vel[moving] * hdt
            if len(self.grabbed):
                delta = (self.grab_target - self.grab_pos) * min(1, hdt * 30)
                norm = np.linalg.norm(delta)
                if norm > self.spacing * 0.3:
                    delta *= self.spacing * 0.3 / norm
                self.grab_pos += delta
                self.pos[self.grabbed] = self.grab_pos + self.grab_offsets
            for a, b, rest, compliance in self.groups:
                delta = self.pos[b] - self.pos[a]
                lengths = np.linalg.norm(delta, axis=1)
                weight = self.mass[a] + self.mass[b]
                valid = (weight > 0) & (lengths > 1e-6)
                corr = np.zeros_like(delta)
                corr[valid] = ((lengths[valid] - rest[valid]) /
                               (weight[valid] + compliance / (hdt * hdt)) /
                               lengths[valid])[:, None] * delta[valid]
                self.pos[a] += corr * self.mass[a, None]
                self.pos[b] -= corr * self.mass[b, None]
            if len(self.grabbed):
                delta = self.pos - self.grab_pos
                lengths = np.linalg.norm(delta, axis=1)
                far = (lengths > self.tether) & moving
                self.pos[far] = self.grab_pos + delta[far] * (self.tether[far] / lengths[far])[:, None]
            floor = self.ground_height()
            below = (self.pos[:, 2] < floor) & moving
            penetration = floor[below] - self.pos[below, 2]
            self.pos[below, 2] = floor[below]
            slide = self.pos[below, :2] - self.prev[below, :2]
            distance = np.linalg.norm(slide, axis=1)
            friction = np.maximum(0, 1 - 1.3 * penetration / np.maximum(distance, 1e-6))
            self.pos[below, :2] = self.prev[below, :2] + slide * friction[:, None]
            self.vel[:] = (self.pos - self.prev) / hdt
        self.max_speed = float(np.linalg.norm(self.vel[moving], axis=1).max(initial=0))
        if not np.isfinite(self.pos).all():
            self.layout((self.width, self.height))

    def projected(self, screen_width: float, screen_height: float):
        depth = max(screen_width, screen_height) * 1.15
        factor = depth / np.maximum(depth - self.pos[:, 2], depth * 0.25)
        center = np.asarray((screen_width / 2, screen_height / 2))
        return center + (self.pos[:, :2] - center) * factor[:, None]

    def pick(self, x: float, y: float, projected: np.ndarray):
        p = projected.reshape(self.ny, self.nx, 2)
        # Closest mesh point is enough for grabbing; the window mask owns exact hit testing.
        d2 = np.sum((projected - (x, y)) ** 2, axis=1)
        return int(np.argmin(d2))
