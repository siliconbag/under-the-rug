"""Minimal visible-frame check for an X11 or Windows build environment."""

from PySide6.QtWidgets import QApplication

from app import RUGS, RugOverlay


application = QApplication([])
settings = {"rug": str(RUGS[0]), "scale": 0.46, "poses": {}}
overlay = RugOverlay(application.primaryScreen(), settings, lambda: None, lambda: None)
for _ in range(5):
    application.processEvents()
    overlay.frame()
image = overlay.grabFramebuffer()
assert not image.isNull(), "OpenGL framebuffer is empty"
assert image.pixelColor(image.width() // 2, image.height() // 2).alpha() > 0, "rug not rendered"
assert image.pixelColor(0, 0).alpha() == 0, "background is not transparent"
overlay.close()
