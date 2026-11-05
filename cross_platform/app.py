"""Transparent desktop rug for Windows and Linux X11."""

from __future__ import annotations

import json
import ctypes
import math
import os
from pathlib import Path
import sys
import time

import numpy as np
from OpenGL import GL as gl
from OpenGL.GL.shaders import compileProgram, compileShader
from PySide6.QtCore import QPoint, Qt, QTimer, QStandardPaths
from PySide6.QtGui import QCursor, QGuiApplication, QIcon, QImage, QPixmap, QPolygon, QRegion, QSurfaceFormat
from PySide6.QtOpenGL import QOpenGLWindow
from PySide6.QtWidgets import (QApplication, QComboBox, QDialog, QFileDialog, QFormLayout,
                               QHBoxLayout, QLabel, QMenu, QPushButton, QSlider,
                               QSystemTrayIcon, QVBoxLayout)

from desktop import position_above_desktop, set_click_through, icon_positions
from physics import Cloth


ROOT = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent.parent))
RUGS = sorted((ROOT / "Resources" / "rugs").glob("*.jpg"))


def settings_path():
    folder = Path(QStandardPaths.writableLocation(QStandardPaths.StandardLocation.AppConfigLocation))
    folder.mkdir(parents=True, exist_ok=True)
    return folder / "rug-cross-platform.json"


def load_settings():
    try:
        settings = json.loads(settings_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        settings = {"rug": str(RUGS[0]), "scale": 0.46, "poses": {}}
    saved_rug = Path(settings.get("rug", ""))
    built_in = next((path for path in RUGS if path.name == saved_rug.name), None)
    if built_in:
        settings["rug"] = str(built_in)
    elif not saved_rug.is_file():
        settings["rug"] = str(RUGS[0])
    settings.setdefault("scale", 0.46)
    settings.setdefault("poses", {})
    return settings


def save_settings(settings):
    settings_path().write_text(json.dumps(settings, indent=2), encoding="utf-8")


class RugOverlay(QOpenGLWindow):
    def __init__(self, screen, settings, on_change, open_settings):
        super().__init__()
        flags = (Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint |
                 Qt.WindowType.WindowDoesNotAcceptFocus)
        if sys.platform.startswith("linux"):
            flags |= Qt.WindowType.WindowStaysOnBottomHint
        self.setFlags(flags)
        self.screen = screen
        self.settings = settings
        self.on_change = on_change
        self.open_settings = open_settings
        fmt = QSurfaceFormat()
        fmt.setRenderableType(QSurfaceFormat.RenderableType.OpenGL)
        fmt.setVersion(3, 3)
        fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
        fmt.setAlphaBufferSize(8)
        fmt.setSamples(4)
        self.setFormat(fmt)
        self.image = QPixmap(settings["rug"])
        if self.image.isNull():
            self.image = QPixmap(str(RUGS[0]))
        self.texture_dirty = True
        self.program = None
        self.cloth = None
        self.projected = None
        self.dragging = False
        self.calm = 0
        self.last_time = time.monotonic()
        self.setGeometry(screen.geometry())
        self.rebuild(False)
        self.show()
        if sys.platform == "win32":
            self.click_through = True
            set_click_through(int(self.winId()), True)
            if not position_above_desktop(int(self.winId())):
                print("Warning: could not place rug above Explorer desktop", file=sys.stderr)
            self.position_timer = QTimer(self)
            self.position_timer.timeout.connect(lambda: position_above_desktop(int(self.winId())))
            self.position_timer.start(2000)
            self.hover_timer = QTimer(self)
            self.hover_timer.timeout.connect(self.poll_hover)
            self.hover_timer.start(30)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.frame)
        self.timer.start(16)
        self.scan_timer = QTimer(self)
        self.scan_timer.timeout.connect(self.scan_icons)
        self.scan_timer.start(4000)
        self.scan_icons()

    def rebuild(self, keep_pose=True):
        old_pose = self.cloth.pose() if keep_pose and self.cloth is not None else None
        old_bumps = self.cloth.bumps if self.cloth is not None else []
        width = self.width() * float(self.settings.get("scale", 0.46))
        height = width * self.image.height() / max(1, self.image.width())
        self.cloth = Cloth(width, height, columns=32)
        self.cloth.set_bumps(old_bumps)
        pose = self.settings.get("poses", {}).get(self.screen.name())
        if old_pose:
            center, angle = old_pose
        elif pose:
            center = (pose[0] * self.width(), pose[1] * self.height())
            angle = pose[2]
        else:
            center, angle = (self.width() / 2, self.height() / 2), 0.0
        self.cloth.layout(center, angle)
        self.projected = self.cloth.projected(self.width(), self.height())
        self.render_x = np.unique(np.rint(np.linspace(0, self.cloth.nx - 1, 9)).astype(int))
        rows = max(5, round((self.cloth.ny - 1) / (self.cloth.nx - 1) * 8) + 1)
        self.render_y = np.unique(np.rint(np.linspace(0, self.cloth.ny - 1, rows)).astype(int))
        indices = []
        for y in range(self.cloth.ny - 1):
            for x in range(self.cloth.nx - 1):
                a = y * self.cloth.nx + x
                indices.extend((a, a + 1, a + self.cloth.nx + 1,
                                a, a + self.cloth.nx + 1, a + self.cloth.nx))
        self.indices = np.asarray(indices, dtype=np.uint32)
        self.calm = 0
        self.timer.start(16) if hasattr(self, "timer") else None
        self.update_mask()
        self.update()

    def set_texture(self, path):
        image = QPixmap(path)
        if image.isNull():
            return False
        self.image = image
        self.texture_dirty = True
        self.rebuild()
        return True

    def save_pose(self):
        center, angle = self.cloth.pose()
        self.settings.setdefault("poses", {})[self.screen.name()] = [
            float(center[0] / self.width()), float(center[1] / self.height()), float(angle)]
        self.on_change()

    def scan_icons(self):
        geometry = self.screen.geometry()
        bumps = []
        for x, y in icon_positions():
            x, y = x - geometry.x() + 36, y - geometry.y() + 34
            if 0 <= x < self.width() and 0 <= y < self.height():
                bumps.append((x, y, 36, 30, 18))
        if bumps != self.cloth.bumps:
            self.cloth.set_bumps(bumps)
            self.calm = 0
            self.timer.start(16)

    def frame(self):
        now = time.monotonic()
        dt = max(1 / 120, min(now - self.last_time, 1 / 20))
        self.last_time = now
        self.cloth.step(dt)
        self.projected = self.cloth.projected(self.width(), self.height())
        self.update_mask()
        self.update()
        self.calm = self.calm + 1 if not self.dragging and self.cloth.max_speed < 3 else 0
        if self.calm > 30:
            self.timer.stop()
            self.save_pose()

    def update_mask(self):
        vertices = self.projected.reshape(self.cloth.ny, self.cloth.nx, 2)[self.render_y][:, self.render_x]
        # Union of mesh triangles handles folds and keeps the rest of the desktop clickable.
        mask = QRegion()
        for y in range(len(self.render_y) - 1):
            for x in range(len(self.render_x) - 1):
                a, b = vertices[y, x], vertices[y, x + 1]
                c, d = vertices[y + 1, x], vertices[y + 1, x + 1]
                for triangle in ((a, b, d), (a, d, c)):
                    polygon = QPolygon([QPoint(round(p[0]), round(p[1])) for p in triangle])
                    mask = mask.united(QRegion(polygon))
        self.input_region = mask
        if sys.platform != "win32":
            self.setMask(mask)

    def poll_hover(self):
        point = QCursor.pos() - self.screen.geometry().topLeft()
        through = not self.dragging and not self.input_region.contains(point)
        if through != self.click_through:
            self.click_through = through
            set_click_through(int(self.winId()), through)

    def initializeGL(self):
        vertex = """#version 330 core
        layout(location=0) in vec2 aPos;
        layout(location=1) in float aHeight;
        layout(location=2) in vec2 aUV;
        layout(location=3) in vec3 aNormal;
        uniform vec2 uScreen;
        out vec2 vUV;
        out vec3 vNormal;
        out float vHeight;
        void main() {
            gl_Position = vec4(2.0*aPos.x/uScreen.x-1.0,
                               1.0-2.0*aPos.y/uScreen.y,
                               clamp(-aHeight/400.0, -0.95, 0.95), 1.0);
            vUV = aUV;
            vNormal = aNormal;
            vHeight = aHeight;
        }"""
        fragment = """#version 330 core
        in vec2 vUV;
        in vec3 vNormal;
        in float vHeight;
        uniform sampler2D uTexture;
        out vec4 color;
        void main() {
            vec3 n = normalize(vNormal);
            float diffuse = max(dot(n, normalize(vec3(-0.45, -0.55, 1.0))), 0.0);
            float light = mix(0.58, 1.08, diffuse);
            if (n.z < 0.0) light *= 0.68;
            vec3 tex = texture(uTexture, vUV).rgb;
            color = vec4(tex * light, 1.0);
        }"""
        self.program = compileProgram(compileShader(vertex, gl.GL_VERTEX_SHADER),
                                      compileShader(fragment, gl.GL_FRAGMENT_SHADER))
        self.vao = gl.glGenVertexArrays(1)
        self.vbo = gl.glGenBuffers(1)
        self.ebo = gl.glGenBuffers(1)
        self.texture = gl.glGenTextures(1)
        gl.glBindVertexArray(self.vao)
        gl.glBindBuffer(gl.GL_ARRAY_BUFFER, self.vbo)
        gl.glBindBuffer(gl.GL_ELEMENT_ARRAY_BUFFER, self.ebo)
        stride = 8 * 4
        for location, size, offset in ((0, 2, 0), (1, 1, 8), (2, 2, 12), (3, 3, 20)):
            gl.glEnableVertexAttribArray(location)
            gl.glVertexAttribPointer(location, size, gl.GL_FLOAT, gl.GL_FALSE, stride,
                                     ctypes.c_void_p(offset))
        gl.glEnable(gl.GL_DEPTH_TEST)
        gl.glDisable(gl.GL_CULL_FACE)

    def upload_texture(self):
        image = self.image.toImage().convertToFormat(QImage.Format.Format_RGBA8888)
        gl.glBindTexture(gl.GL_TEXTURE_2D, self.texture)
        gl.glTexImage2D(gl.GL_TEXTURE_2D, 0, gl.GL_RGBA8, image.width(), image.height(),
                        0, gl.GL_RGBA, gl.GL_UNSIGNED_BYTE, bytes(image.bits()))
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MIN_FILTER, gl.GL_LINEAR_MIPMAP_LINEAR)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MAG_FILTER, gl.GL_LINEAR)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_S, gl.GL_CLAMP_TO_EDGE)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_T, gl.GL_CLAMP_TO_EDGE)
        gl.glGenerateMipmap(gl.GL_TEXTURE_2D)
        self.texture_dirty = False

    def paintGL(self):
        if self.program is None:
            return
        gl.glViewport(0, 0, round(self.width() * self.devicePixelRatio()),
                      round(self.height() * self.devicePixelRatio()))
        gl.glClearColor(0, 0, 0, 0)
        gl.glClear(gl.GL_COLOR_BUFFER_BIT | gl.GL_DEPTH_BUFFER_BIT)
        if self.texture_dirty:
            self.upload_texture()
        grid = self.cloth.pos.reshape(self.cloth.ny, self.cloth.nx, 3)
        dx = np.gradient(grid, axis=1)
        dy = np.gradient(grid, axis=0)
        normals = np.cross(dx, dy).reshape(-1, 3)
        normals /= np.maximum(np.linalg.norm(normals, axis=1), 1e-6)[:, None]
        vertices = np.column_stack((self.projected, self.cloth.pos[:, 2],
                                    self.cloth.uv, normals)).astype(np.float32)
        gl.glUseProgram(self.program)
        gl.glUniform2f(gl.glGetUniformLocation(self.program, "uScreen"), self.width(), self.height())
        gl.glActiveTexture(gl.GL_TEXTURE0)
        gl.glBindTexture(gl.GL_TEXTURE_2D, self.texture)
        gl.glUniform1i(gl.glGetUniformLocation(self.program, "uTexture"), 0)
        gl.glBindVertexArray(self.vao)
        gl.glBindBuffer(gl.GL_ARRAY_BUFFER, self.vbo)
        gl.glBufferData(gl.GL_ARRAY_BUFFER, vertices.nbytes, vertices, gl.GL_STREAM_DRAW)
        gl.glBindBuffer(gl.GL_ELEMENT_ARRAY_BUFFER, self.ebo)
        gl.glBufferData(gl.GL_ELEMENT_ARRAY_BUFFER, self.indices.nbytes, self.indices, gl.GL_STATIC_DRAW)
        gl.glDrawElements(gl.GL_TRIANGLES, len(self.indices), gl.GL_UNSIGNED_INT, None)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            self.show_menu(event.globalPosition().toPoint())
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        p = event.position()
        vertex = self.cloth.pick(p.x(), p.y(), self.projected)
        z = max(0, self.cloth.pos[vertex, 2]) + 120
        depth = max(self.width(), self.height()) * 1.15
        factor = (depth - z) / depth
        xy = np.array((self.width() / 2, self.height() / 2)) + (
            np.array((p.x(), p.y())) - (self.width() / 2, self.height() / 2)) * factor
        self.cloth.begin_grab(vertex, np.array((xy[0], xy[1], z)))
        self.dragging = True
        self.calm = 0
        self.timer.start(16)

    def mouseMoveEvent(self, event):
        if not self.dragging:
            return
        p = event.position()
        z = self.cloth.grab_target[2]
        depth = max(self.width(), self.height()) * 1.15
        factor = (depth - z) / depth
        xy = np.array((self.width() / 2, self.height() / 2)) + (
            np.array((p.x(), p.y())) - (self.width() / 2, self.height() / 2)) * factor
        self.cloth.grab_target[:2] = xy

    def mouseReleaseEvent(self, event):
        if self.dragging:
            self.dragging = False
            self.cloth.end_grab()
            self.save_pose()

    def wheelEvent(self, event):
        self.cloth.rotate(math.radians(5) * (1 if event.angleDelta().y() > 0 else -1))
        self.calm = 0
        self.timer.start(16)

    def show_menu(self, global_pos):
        menu = QMenu()
        menu.addAction("Settings…", self.open_settings)
        menu.addAction("Center rug", lambda: self.cloth.layout((self.width() / 2, self.height() / 2)))
        menu.addAction("Drop rug", self.rebuild)
        menu.exec(global_pos)
        self.timer.start(16)


class SettingsDialog(QDialog):
    def __init__(self, settings, overlays, changed):
        super().__init__()
        self.settings = settings
        self.overlays = overlays
        self.changed = changed
        self.setWindowTitle("Rug settings")
        self.setMinimumWidth(330)
        form = QFormLayout()
        self.rug = QComboBox()
        for path in RUGS:
            self.rug.addItem(path.stem[2:].strip().title(), str(path))
        self.rug.addItem("Custom image…", "custom")
        current = self.rug.findData(settings.get("rug"))
        self.rug.setCurrentIndex(current if current >= 0 else self.rug.count() - 1)
        self.rug.currentIndexChanged.connect(self.choose_rug)
        form.addRow("Rug", self.rug)
        self.scale = QSlider(Qt.Orientation.Horizontal)
        self.scale.setRange(20, 80)
        self.scale.setValue(round(100 * settings.get("scale", 0.46)))
        self.scale.valueChanged.connect(self.change_scale)
        form.addRow("Size", self.scale)
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        help_text = "Drag an edge or corner to lift the rug. Scroll to rotate. Right-click for a menu."
        if sys.platform.startswith("linux"):
            help_text += " Linux support currently requires an X11 session; automatic icon bumps are unavailable."
        layout.addWidget(QLabel(help_text, wordWrap=True))
        row = QHBoxLayout()
        center = QPushButton("Center rug")
        center.clicked.connect(self.center)
        row.addWidget(center)
        close = QPushButton("Close")
        close.clicked.connect(self.close)
        row.addWidget(close)
        layout.addLayout(row)

    def choose_rug(self):
        path = self.rug.currentData()
        if path == "custom":
            path, _ = QFileDialog.getOpenFileName(self, "Choose rug image", "", "Images (*.png *.jpg *.jpeg *.webp)")
            if not path:
                return
        if all(overlay.set_texture(path) for overlay in self.overlays):
            self.settings["rug"] = path
            self.changed()

    def change_scale(self, value):
        self.settings["scale"] = value / 100
        for overlay in self.overlays:
            overlay.rebuild()
        self.changed()

    def center(self):
        for overlay in self.overlays:
            overlay.cloth.layout((overlay.width() / 2, overlay.height() / 2))
            overlay.timer.start(16)


def main():
    if sys.platform.startswith("linux") and os.environ.get("XDG_SESSION_TYPE", "x11").lower() == "wayland":
        raise SystemExit("Rug requires an X11 desktop session; Wayland does not expose the required desktop window/input APIs.")
    app = QApplication(sys.argv)
    app.setApplicationName("Rug")
    app.setQuitOnLastWindowClosed(False)
    settings = load_settings()
    icon = QIcon(str(ROOT / "Resources" / "icon_1024.png"))
    app.setWindowIcon(icon)
    overlays = []
    dialog = None

    def changed():
        save_settings(settings)

    def open_settings():
        nonlocal dialog
        if dialog is None:
            dialog = SettingsDialog(settings, overlays, changed)
        dialog.show()
        dialog.raise_()
        dialog.activateWindow()

    for screen in QGuiApplication.screens():
        overlays.append(RugOverlay(screen, settings, changed, open_settings))
    tray = QSystemTrayIcon(icon, app)
    menu = QMenu()
    menu.addAction("Settings…", open_settings)
    menu.addAction("Quit", app.quit)
    tray.setContextMenu(menu)
    tray.activated.connect(lambda reason: open_settings() if reason == QSystemTrayIcon.ActivationReason.Trigger else None)
    tray.show()
    open_settings()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
