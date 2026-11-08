import math
import unittest

import numpy as np

from physics import Cloth


class ClothTests(unittest.TestCase):
    def test_rest_shape_stays_finite_after_drop(self):
        cloth = Cloth(500, 300, 15)
        cloth.layout((600, 400), drop=100)
        for _ in range(120):
            cloth.step(1 / 30)
        self.assertTrue(np.isfinite(cloth.pos).all())
        self.assertGreaterEqual(cloth.pos[:, 2].min(), -0.001)
        self.assertLess(abs(cloth.pose()[0][0] - 600), 15)

    def test_drag_and_rotation(self):
        cloth = Cloth(400, 250, 11)
        cloth.layout((500, 400), drop=0)
        vertex = 0
        cloth.begin_grab(vertex, cloth.pos[vertex] + (50, 20, 100))
        for _ in range(30):
            cloth.step(1 / 30)
        self.assertGreater(cloth.pos[vertex, 2], 50)
        cloth.end_grab()
        cloth.rotate(math.pi / 4)
        self.assertAlmostEqual(cloth.pose()[1], math.pi / 4, delta=0.15)


if __name__ == "__main__":
    unittest.main()
