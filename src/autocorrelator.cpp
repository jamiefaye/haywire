#include "autocorrelator.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iostream>

// FFT functions from old Haywire
extern "C" {
    void init_DFT_16384();
    void DFT_16384(double* Y, double* X);
}

namespace Haywire {

Autocorrelator::Autocorrelator() 
    : enabled(false), fftWorkspace(nullptr), fftOutput(nullptr) {
    InitFFT();
}

Autocorrelator::~Autocorrelator() {
    if (fftWorkspace) delete[] fftWorkspace;
    if (fftOutput) delete[] fftOutput;
}

void Autocorrelator::InitFFT() {
    fftWorkspace = new double[FFT_SIZE * 2];  // Complex pairs
    fftOutput = new double[FFT_SIZE * 2];
    
    // Initialize FFT tables
    init_DFT_16384();
}

std::vector<double> Autocorrelator::Correlate(const uint8_t* data, size_t size, int pixelFormat) {
    correlationData.clear();

    if (!data || size < 64) {
        return correlationData;
    }

    // Clear workspace
    std::memset(fftWorkspace, 0, sizeof(double) * FFT_SIZE * 2);
    std::memset(fftOutput, 0, sizeof(double) * FFT_SIZE * 2);

    // Extract samples based on pixel format
    // This matches the old Haywire implementation for better display quality
    double* tP = fftWorkspace;
    int64_t avga = 0;
    size_t sampleCount = SAMPLE_SIZE;

    switch (pixelFormat) {
        case 0:  // ARGB32/RGBA8888 - RGB888
        case 4:  // BGRA8888
        case 5:  // ARGB8888
        case 6:  // ABGR8888
        case 9:  // HEX_PIXEL (32-bit)
        {
            // 32-bit pixel formats
            const uint32_t* mP = reinterpret_cast<const uint32_t*>(data);
            sampleCount = std::min(size / 4, (size_t)SAMPLE_SIZE);

            // Calculate mean
            for (size_t i = 0; i < sampleCount; i++) {
                avga += mP[i];
            }
            double meanie = double(avga) / sampleCount;

            // Fill with mean-removed data
            for (size_t i = 0; i < sampleCount; i++) {
                *tP++ = mP[i] - meanie;
                *tP++ = 0;  // Imaginary part
            }
            break;
        }

        case 1:  // RGB888 (24-bit, no alpha)
        case 2:  // BGR888
        {
            const uint8_t* iB = data;
            sampleCount = std::min(size / 3, (size_t)SAMPLE_SIZE);

            // Calculate mean
            for (size_t i = 0; i < sampleCount; i++) {
                uint32_t Dw = (iB[0] << 16) | (iB[1] << 8) | iB[2];
                avga += Dw;
                iB += 3;
            }

            double mean2 = double(avga) / sampleCount;
            iB = data;

            // Fill with mean-removed data
            for (size_t i = 0; i < sampleCount; i++) {
                uint32_t Dw = (iB[0] << 16) | (iB[1] << 8) | iB[2];
                *tP++ = Dw - mean2;
                *tP++ = 0;
                iB += 3;
            }
            break;
        }

        case 7:  // RGB565 (16-bit)
        case 8:  // GRAYSCALE (16-bit in old code)
        {
            const uint16_t* sp = reinterpret_cast<const uint16_t*>(data);
            sampleCount = std::min(size / 2, (size_t)SAMPLE_SIZE);

            // Calculate mean
            for (size_t i = 0; i < sampleCount; i++) {
                avga += sp[i];
            }
            double mean3 = double(avga) / sampleCount;

            // Fill with mean-removed data
            for (size_t i = 0; i < sampleCount; i++) {
                *tP++ = sp[i] - mean3;
                *tP++ = 0;
            }
            break;
        }

        case 10:  // CHAR_8BIT (8-bit characters)
        {
            const uint8_t* sp = data;
            sampleCount = std::min(size, (size_t)SAMPLE_SIZE);

            // Calculate mean
            for (size_t i = 0; i < sampleCount; i++) {
                avga += sp[i];
            }
            double mean4 = double(avga) / sampleCount;

            // Fill with mean-removed data
            for (size_t i = 0; i < sampleCount; i++) {
                *tP++ = sp[i] - mean4;
                *tP++ = 0;
            }
            break;
        }

        default:
            // Fallback: treat as 32-bit
            const uint32_t* mP = reinterpret_cast<const uint32_t*>(data);
            sampleCount = std::min(size / 4, (size_t)SAMPLE_SIZE);

            for (size_t i = 0; i < sampleCount; i++) {
                avga += mP[i];
            }
            double meanie = double(avga) / sampleCount;

            for (size_t i = 0; i < sampleCount; i++) {
                *tP++ = mP[i] - meanie;
                *tP++ = 0;
            }
            break;
    }

    // Zero pad the rest (tP already points to the right location)
    for (size_t i = sampleCount; i < FFT_SIZE; i++) {
        *tP++ = 0;
        *tP++ = 0;
    }

    // Forward FFT
    DFT_16384(fftOutput, fftWorkspace);

    // Compute power spectrum
    double* t2P = fftWorkspace;
    double* Tp = fftOutput;
    for (int i = 0; i < SAMPLE_SIZE; i++) {
        double rV = *Tp++;
        double iV = *Tp++;
        *t2P++ = std::sqrt(rV * rV + iV * iV) / 32768.0;  // Match old normalization
        *t2P++ = 0;
    }

    // Forward FFT of power spectrum
    DFT_16384(fftOutput, fftWorkspace);

    // Conjugate to make it an inverse FFT (tear through conjugating the result)
    double* iP = fftOutput;
    for (int i = 0; i < FFT_SIZE; i++) {
        *iP++ /= 16384.0;           // Real part
        *iP = *iP / -16384.0;       // Imaginary part (conjugate)
        iP++;
    }

    // Extract correlation values (real part only)
    correlationData.resize(2048);  // Show first 2048 offsets
    for (size_t i = 0; i < correlationData.size(); i++) {
        correlationData[i] = fftOutput[i * 2];  // Already normalized above
    }

    return correlationData;
}

std::vector<int> Autocorrelator::FindPeaks(const std::vector<double>& correlation, double threshold) {
    std::vector<int> peaks;
    
    if (correlation.size() < 3) return peaks;
    
    // Skip offset 0 (always maximum)
    for (size_t i = 16; i < correlation.size() - 1; i++) {
        // Check if local maximum and above threshold
        if (correlation[i] > threshold &&
            correlation[i] > correlation[i-1] &&
            correlation[i] > correlation[i+1]) {
            peaks.push_back(i);
            
            // Skip nearby points to avoid duplicate peaks
            i += 8;
        }
    }
    
    return peaks;
}

std::vector<float> Autocorrelator::GetNormalizedCorrelation() const {
    std::vector<float> normalized;
    normalized.reserve(correlationData.size());
    
    for (const auto& val : correlationData) {
        normalized.push_back(static_cast<float>(val));
    }
    
    return normalized;
}

}