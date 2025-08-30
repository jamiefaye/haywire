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
    
    // Calculate how many samples to use (limit to SAMPLE_SIZE)
    size_t sampleCount = std::min(size / 4, (size_t)SAMPLE_SIZE);  // Assume 32-bit pixels
    
    // Convert input data to doubles and remove DC bias
    const uint32_t* pixels = reinterpret_cast<const uint32_t*>(data);
    double sum = 0;
    
    // Calculate mean
    for (size_t i = 0; i < sampleCount; i++) {
        sum += pixels[i];
    }
    double mean = sum / sampleCount;
    
    // Fill FFT input with mean-removed data
    for (size_t i = 0; i < sampleCount; i++) {
        fftWorkspace[i * 2] = pixels[i] - mean;  // Real part
        fftWorkspace[i * 2 + 1] = 0;  // Imaginary part
    }
    
    // Zero pad the rest
    for (size_t i = sampleCount; i < FFT_SIZE; i++) {
        fftWorkspace[i * 2] = 0;
        fftWorkspace[i * 2 + 1] = 0;
    }
    
    // Forward FFT
    DFT_16384(fftOutput, fftWorkspace);
    
    // Compute power spectrum
    for (int i = 0; i < FFT_SIZE; i++) {
        double real = fftOutput[i * 2];
        double imag = fftOutput[i * 2 + 1];
        double magnitude = std::sqrt(real * real + imag * imag);
        fftWorkspace[i * 2] = magnitude / FFT_SIZE;
        fftWorkspace[i * 2 + 1] = 0;
    }
    
    // Inverse FFT to get autocorrelation
    DFT_16384(fftOutput, fftWorkspace);
    
    // Extract correlation values (real part only, normalized)
    correlationData.resize(2048);  // Show first 2048 offsets
    
    // Get the autocorrelation peak value for normalization
    double maxVal = fftOutput[0];
    if (maxVal == 0) maxVal = 1.0;
    
    // Extract normalized correlation values
    for (size_t i = 0; i < correlationData.size() && i < FFT_SIZE; i++) {
        correlationData[i] = fftOutput[i * 2] / maxVal;
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