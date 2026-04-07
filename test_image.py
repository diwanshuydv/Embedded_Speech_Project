import cv2
import numpy as np

# Load a test image, convert to grayscale, and resize to 96x96
img = cv2.imread("test_np2.jpg", cv2.IMREAD_GRAYSCALE)
img = cv2.resize(img, (96, 96))

# Write to a C header file
with open("test_image.h", "w") as f:
    f.write("#pragma once\n")
    f.write("const uint8_t dummy_image[9216] = {\n")
    arr = img.flatten()
    f.write(", ".join(map(str, arr)))
    f.write("\n};\n")

print("test_image.h generated!")