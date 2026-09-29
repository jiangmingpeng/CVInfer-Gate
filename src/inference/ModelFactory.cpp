#include "inference/ModelFactory.h"

#include "inference/YoloDetector.h"
#include "inference/BehaviorClassifier.h"
#include "utils/Logger.h"

std::shared_ptr<IModel> ModelFactory::create(const ModelConfig& cfg) {
    const ModelRole role = modelRoleFromString(cfg.role);

    switch (role) {
        case ModelRole::Detector: {
            auto det = std::make_shared<YoloDetector>();
            if (!det->init(cfg)) {
                CVLOG_ERROR << "[ModelFactory] YoloDetector 初始化失败: " << cfg.name;
                return nullptr;
            }
            return det;
        }
        case ModelRole::Classifier: {
            // T17 落地: 行为/安全帽分类器(单 ROI)
            auto cls = std::make_shared<BehaviorClassifier>();
            if (!cls->init(cfg)) {
                CVLOG_ERROR << "[ModelFactory] BehaviorClassifier 初始化失败: " << cfg.name;
                return nullptr;
            }
            return cls;
        }
        case ModelRole::Reviewer:
            // 复核模型由 review 层(复用 gRPC)负责, 不在此处构建
            CVLOG_WARN << "[ModelFactory] reviewer 角色不走模型工厂: " << cfg.name;
            return nullptr;
    }
    return nullptr;
}
