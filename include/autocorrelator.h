#pragma once

#include <vector>
#include <cstdint>

namespace Haywire {

class Autocorrelator {
public:
    static const int FFT_SIZE = 16384;
    static const int SAMPLE_SIZE = 8192;
    
    Autocorrelator();
    ~Autocorrelator();
    
    // Compute autocorrelation for memory buffer
    // Returns correlation values for different offsets
    std::vector<double> Correlate(const uint8_t* data, size_t size, int pixelFormat = 0);
    
    // Find peaks in correlation data
    std::vector<int> FindPeaks(const std::vector<double>& correlation, double threshold = 0.5);
    
    // Get normalized correlation for display (0-1 range)
    std::vector<float> GetNormalizedCorrelation() const;
    
    // Enable/disable correlation display
    void SetEnabled(bool enable) { enabled = enable; }
    bool IsEnabled() const { return enabled; }
    
private:
    bool enabled;
    std::vector<double> correlationData;
    double* fftWorkspace;
    double* fftOutput;
    
    void InitFFT();
    void ComputeFFT(const uint8_t* data, size_t size, double* output);
};

}