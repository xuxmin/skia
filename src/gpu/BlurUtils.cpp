/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/BlurUtils.h"

#include <array>

namespace skgpu {

void Compute2DBlurKernel(SkSize sigma,
                         SkISize radius,
                         SkSpan<float> kernel) {
    // Callers likely had to calculate the radius prior to filling out the kernel value, which is
    // why it's provided; but make sure it's consistent with expectations.
    SkASSERT(BlurSigmaRadius(sigma.width()) == radius.width() &&
             BlurSigmaRadius(sigma.height()) == radius.height());

    // Callers are responsible for downscaling large sigmas to values that can be processed by the
    // effects, so ensure the radius won't overflow 'kernel'
    const int width = BlurKernelWidth(radius.width());
    const int height = BlurKernelWidth(radius.height());
    const size_t kernelSize = SkTo<size_t>(sk_64_mul(width, height));
    SkASSERT(kernelSize <= kernel.size());

    // And the definition of an identity blur should be sufficient that 2sigma^2 isn't near zero
    // when there's a non-trivial radius.
    const float twoSigmaSqrdX = 2.0f * sigma.width() * sigma.width();
    const float twoSigmaSqrdY = 2.0f * sigma.height() * sigma.height();
    SkASSERT((radius.width() == 0 || !SkScalarNearlyZero(twoSigmaSqrdX)) &&
             (radius.height() == 0 || !SkScalarNearlyZero(twoSigmaSqrdY)));

    // Setting the denominator to 1 when the radius is 0 automatically converts the remaining math
    // to the 1D Gaussian distribution. When both radii are 0, it correctly computes a weight of 1.0
    const float sigmaXDenom = radius.width() > 0 ? 1.0f / twoSigmaSqrdX : 1.f;
    const float sigmaYDenom = radius.height() > 0 ? 1.0f / twoSigmaSqrdY : 1.f;

    float sum = 0.0f;
    for (int x = 0; x < width; x++) {
        float xTerm = static_cast<float>(x - radius.width());
        xTerm = xTerm * xTerm * sigmaXDenom;
        for (int y = 0; y < height; y++) {
            float yTerm = static_cast<float>(y - radius.height());
            float xyTerm = sk_float_exp(-(xTerm + yTerm * yTerm * sigmaYDenom));
            // Note that the constant term (1/(sqrt(2*pi*sigma^2)) of the Gaussian
            // is dropped here, since we renormalize the kernel below.
            kernel[y * width + x] = xyTerm;
            sum += xyTerm;
        }
    }
    // Normalize the kernel
    float scale = 1.0f / sum;
    for (size_t i = 0; i < kernelSize; ++i) {
        kernel[i] *= scale;
    }
    // Zero remainder of the array
    memset(kernel.data() + kernelSize, 0, sizeof(float)*(kernel.size() - kernelSize));
}

void Compute1DBlurLinearKernel(float sigma,
                               int radius,
                               std::array<float, kMaxBlurSamples>& kernel,
                               std::array<float, kMaxBlurSamples>& offsets) {
    SkASSERT(sigma <= kMaxLinearBlurSigma);
    SkASSERT(radius == BlurSigmaRadius(sigma));
    SkASSERT(BlurLinearKernelWidth(radius) <= kMaxBlurSamples);

    // Given 2 adjacent gaussian points, they are blended as: Wi * Ci + Wj * Cj.
    // The GPU will mix Ci and Cj as Ci * (1 - x) + Cj * x during sampling.
    // Compute W', x such that W' * (Ci * (1 - x) + Cj * x) = Wi * Ci + Wj * Cj.
    // Solving W' * x = Wj, W' * (1 - x) = Wi:
    // W' = Wi + Wj
    // x = Wj / (Wi + Wj)
    auto get_new_weight = [](float* new_w, float* offset, float wi, float wj) {
        *new_w = wi + wj;
        *offset = wj / (wi + wj);
    };

    // Create a temporary standard kernel. The maximum blur radius that can be passed to this
    // function is (kMaxBlurSamples-1), so make an array large enough to hold the full kernel width.
    static constexpr int kMaxKernelWidth = BlurKernelWidth(kMaxBlurSamples - 1);
    SkASSERT(BlurKernelWidth(radius) <= kMaxKernelWidth);
    std::array<float, kMaxKernelWidth> fullKernel;
    Compute1DBlurKernel(sigma, radius, SkSpan<float>{fullKernel.data(), BlurKernelWidth(radius)});

    // Note that halfsize isn't just size / 2, but radius + 1. This is the size of the output array.
    int halfSize = skgpu::BlurLinearKernelWidth(radius);
    int halfRadius = halfSize / 2;
    int lowIndex = halfRadius - 1;

    // Compute1DGaussianKernel produces a full 2N + 1 kernel. Since the kernel can be mirrored,
    // compute only the upper half and mirror to the lower half.

    int index = radius;
    if (radius & 1) {
        // If N is odd, then use two samples.
        // The centre texel gets sampled twice, so halve its influence for each sample.
        // We essentially sample like this:
        // Texel edges
        // v    v    v    v
        // |    |    |    |
        // \-----^---/ Lower sample
        //      \---^-----/ Upper sample
        get_new_weight(&kernel[halfRadius],
                       &offsets[halfRadius],
                       fullKernel[index] * 0.5f,
                       fullKernel[index + 1]);
        kernel[lowIndex] = kernel[halfRadius];
        offsets[lowIndex] = -offsets[halfRadius];
        index++;
        lowIndex--;
    } else {
        // If N is even, then there are an even number of texels on either side of the centre texel.
        // Sample the centre texel directly.
        kernel[halfRadius] = fullKernel[index];
        offsets[halfRadius] = 0.0f;
    }
    index++;

    // Every other pair gets one sample.
    for (int i = halfRadius + 1; i < halfSize; index += 2, i++, lowIndex--) {
        get_new_weight(&kernel[i], &offsets[i], fullKernel[index], fullKernel[index + 1]);
        offsets[i] += static_cast<float>(index - radius);

        // Mirror to lower half.
        kernel[lowIndex] = kernel[i];
        offsets[lowIndex] = -offsets[i];
    }

    // Zero out remaining values in the arrays
    memset(kernel.data() + halfSize, 0, sizeof(float)*(kMaxBlurSamples - halfSize));
    memset(offsets.data() + halfSize, 0, sizeof(float)*(kMaxBlurSamples - halfSize));
}

} // namespace skgpu