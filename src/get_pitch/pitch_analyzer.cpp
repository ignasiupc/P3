/// @file

#include <iostream>
#include <math.h>
#include "pitch_analyzer.h"

using namespace std;

/// Name space of UPC
namespace upc {
  static void makeTukeyWindow(std::vector<float>& w, float alpha) {
    int N = w.size();
    int edge = int(alpha * (N - 1) / 2);
    for (int n = 0; n < N; ++n) {
        if (n < edge)
            w[n] = 0.5f * (1 + cosf((float)3.1415926 * (2.0f * n / alpha / (N - 1) - 1)));
        else if (n <= N - 1 - edge)
            w[n] = 1.0f;
        else
            w[n] = 0.5f * (1 + cosf((float)3.1415926 * (2.0f * (n - (N - 1 - edge)) / alpha / (N - 1) + 1)));
    }
  }

  void PitchAnalyzer::autocorrelation(const std::vector<float>& x, std::vector<float>& r) const {
    unsigned int N = x.size();
    for (unsigned int l = 0; l < r.size(); ++l) {
        float sum = 0.0f;
        for (unsigned int n = 0; n + l < N; ++n)
            sum += x[n] * x[n + l];
        r[l] = sum / static_cast<float>(N);
    }
    if (r[0] == 0.0f)
        r[0] = 1e-10f;
  }

  void PitchAnalyzer::set_window(Window win_type) {
    window.resize(frameLen);
    switch (win_type) {
    case HAMMING:
        for (unsigned int i = 0; i < frameLen; ++i)
            window[i] = 0.54f - 0.46f * cosf(2.0f * (float)3.1415926 * i / (frameLen - 1));
        break;
    case TUKEY:
        makeTukeyWindow(window, 0.01f);
        break;
    case RECT:
    default:
        std::fill(window.begin(), window.end(), 1.0f);
    }
  }

  void PitchAnalyzer::set_f0_range(float min_F0, float max_F0) {
    npitch_min = samplingFreq / max_F0;
    npitch_min = std::max(npitch_min, 2u);
    npitch_max = 1 + static_cast<unsigned int>(samplingFreq / min_F0);
    npitch_max = std::min(npitch_max, frameLen / 2u);
  }
  bool PitchAnalyzer::analyze_frame(const std::vector<float>& in, float& out_pot, float& out_norm_r1, float& out_norm_rpeak, unsigned int& out_best_lag, bool& out_reliable_peak, float& r_val_at_lag_minus_1, float& r_val_at_lag, float& r_val_at_lag_plus_1 ) const {
     // copy and apply window
    std::vector<float> x = in;
    for (unsigned int i = 0; i < frameLen; ++i)
        x[i] *= window[i];

    // compute autocorrelation
    std::vector<float> r(npitch_max);
    autocorrelation(x, r);

    // define search range
    unsigned int lag_min = samplingFreq / 400;
    unsigned int lag_max = samplingFreq / 80;
    lag_max = std::min(lag_max, static_cast<unsigned int>(r.size() - 1));

    // find best lag
    out_best_lag = lag_min;
    float best_corr = r[lag_min];
    for (unsigned int l = lag_min + 1; l <= lag_max; ++l) {
        if (r[l] > best_corr) {
            best_corr = r[l];
            out_best_lag = l;
        }
    }
    r_val_at_lag = r[out_best_lag];
    // Handle boundary conditions for neighbors carefully
    r_val_at_lag_minus_1 = (out_best_lag > 0 && out_best_lag < r.size()) ? r[out_best_lag - 1] : r_val_at_lag;
    r_val_at_lag_plus_1 = (out_best_lag + 1 < r.size()) ? r[out_best_lag + 1] : r_val_at_lag;
    
    // compute power & normalizations
    out_pot = 10.0f * log10f(r[0]);
    out_norm_r1 = r[1] / r[0];
    out_norm_rpeak = best_corr / r[0];

    // peak reliability
    out_reliable_peak = (out_best_lag > lag_min && out_best_lag < lag_max)
        && (best_corr > r[out_best_lag - 1] && best_corr > r[out_best_lag + 1]);

    return true;
  }

  float PitchAnalyzer::lag_to_f0(unsigned int best_lag, bool reliable_peak, float pot, float r1norm, float rmaxnorm, float r_val_at_lag_minus_1, float r_val_at_lag, float r_val_at_lag_plus_1 ) const {
      float delta = 0.0f;
      if (reliable_peak && best_lag > 0) { 
          // Check if r_val_at_lag is a true peak and neighbors are distinct enough
          // to avoid issues with flat peaks or invalid denominator.
          if (r_val_at_lag > r_val_at_lag_minus_1 && r_val_at_lag > r_val_at_lag_plus_1) {
              float denominator = r_val_at_lag_minus_1 - 2.0f * r_val_at_lag + r_val_at_lag_plus_1;
              if (fabs(denominator) > 1e-6f) { // Avoid division by zero or very small numbers
                  delta = 0.5f * (r_val_at_lag_minus_1 - r_val_at_lag_plus_1) / denominator;
                  // Clamp delta to a reasonable range, e.g., between -0.5 and +0.5
                  delta = std::max(-0.5f, std::min(delta, 0.5f));
              }
          }
      }

      // only zero‐out on “unvoiced” if skipUnvoicedTest_==false
      if (!reliable_peak ||
          (!skipUnvoicedTest_ && unvoiced(pot,r1norm,rmaxnorm)))
          return 0.0f;

      return static_cast<float>(samplingFreq) / (best_lag + delta);
  }

  bool PitchAnalyzer::unvoiced(float pot, float r1norm, float rmaxnorm) const {
    /// \TODO Implement a rule to decide whether the sound is voiced or not.
    /// * You can use the standard features (pot, r1norm, rmaxnorm),
    ///   or compute and use other ones. 
    const float threshold_pot = -48.0f;    
    const float threshold_r1 = 0.46f;        
    const float threshold_rmax = 0.48f;     
    
    if (rmaxnorm > 0.6f && pot > -40.0f) // Was rmaxnorm > 0.6f && pot > -48.0f
        return false;
    
    // This condition remains, it's another path to being voiced.
    if (rmaxnorm > 0.58f && r1norm > 0.52f && pot > -40.0f)
        return false;
    
    // Main unvoiced condition check
    if ((pot < threshold_pot) || (r1norm < threshold_r1) || (rmaxnorm < threshold_rmax)) {
        // This is an "escape" from being unvoiced if these specific conditions are met.
        // We can make this slightly easier to meet too.
        if ((pot > -42.0f && rmaxnorm > 0.35f && r1norm > 0.42f) || // Was pot > -42.0f, rmaxnorm > 0.36f, norm_r1 > 0.42f
            (pot > -44.0f && rmaxnorm > 0.48f))                      // Was pot > -46.0f, rmaxnorm > 0.45f
            return false;
        return true; // If none of the above voiced conditions are met, it's unvoiced.
    }
    
    return false; 
  }

  float PitchAnalyzer::compute_pitch(vector<float> & x) const {
    float pot, nr1, nrp;
    unsigned int lag;
    bool rp;
    // Declare variables for the new correlation values
    float r_m1, r_0, r_p1; 
    // Call analyze_frame with the new arguments
    analyze_frame(x, pot, nr1, nrp, lag, rp, r_m1, r_0, r_p1);
    // Call lag_to_f0 with the new arguments
    return lag_to_f0(lag, rp, pot, nr1, nrp, r_m1, r_0, r_p1);
  }
}
