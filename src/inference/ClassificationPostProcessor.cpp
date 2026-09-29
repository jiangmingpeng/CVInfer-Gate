#include "inference/ClassificationPostProcessor.h"

#include <algorithm>
#include <cmath>

ClassificationPostProcessor::ClassificationPostProcessor(std::vector<std::string> labels)
    : labels_(std::move(labels)) {}

Classification ClassificationPostProcessor::process(const ov::Tensor& output) const {
    Classification c;   // class_id = -1 (无效)

    try {
        if (output.get_element_type() != ov::element::f32) return c;
        const float* data = output.data<float>();
        const std::size_t n = output.get_size();
        if (n == 0 || data == nullptr) return c;

        std::vector<float> probs(data, data + n);

        // 自动判定是否需要 softmax (见头文件说明)
        if (n > 1) {
            float sum = 0.0f;
            bool all_prob_like = true;
            for (float v : probs) {
                sum += v;
                if (v < 0.0f || v > 1.0f) { all_prob_like = false; break; }
            }
            const bool looks_like_probs = all_prob_like && std::fabs(sum - 1.0f) <= 0.01f;
            if (!looks_like_probs) {
                const float mx = *std::max_element(probs.begin(), probs.end());
                float denom = 0.0f;
                for (float& v : probs) { v = std::exp(v - mx); denom += v; }
                if (denom > 0.0f) {
                    for (float& v : probs) v /= denom;
                }
            }
        }

        // argmax
        std::size_t best = 0;
        float bestv = probs[0];
        for (std::size_t i = 1; i < probs.size(); ++i) {
            if (probs[i] > bestv) { bestv = probs[i]; best = i; }
        }

        c.class_id = static_cast<int>(best);
        c.confidence = bestv;
        c.label = (best < labels_.size()) ? labels_[best] : std::to_string(best);
    } catch (...) {
        // 保持无效( class_id = -1 )
    }
    return c;
}
