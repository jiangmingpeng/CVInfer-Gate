#include "inference/ModelPoolManager.h"

#include "inference/CascadeEngine.h"
#include "inference/ModelFactory.h"
#include "utils/Logger.h"

bool ModelPoolManager::init(const std::vector<ModelConfig>& models,
                            int default_pool_size) {
    close(); // 重置旧资源

    if (models.empty()) {
        CVLOG_ERROR << "[ModelPoolManager] 未提供任何模型配置。";
        return false;
    }
    if (default_pool_size < 1) default_pool_size = 1;

    for (const auto& raw : models) {
        ModelConfig cfg = raw;
        // pool_size 兜底: 保持旧版 "引擎数 = worker 线程数" 的行为
        if (cfg.pool_size < 1) cfg.pool_size = default_pool_size;

        std::shared_ptr<IModel> model = ModelFactory::create(cfg);
        if (!model) {
            CVLOG_ERROR << "[ModelPoolManager] 模型创建失败: " << cfg.name
                        << " (role=" << cfg.role << "), 已回滚。";
            close();
            return false;
        }

        if (by_name_.find(cfg.name) != by_name_.end()) {
            CVLOG_WARN << "[ModelPoolManager] 模型名重复, 后者覆盖前者: " << cfg.name;
        }
        by_name_[cfg.name] = model;
        models_.push_back(model);
    }

    CVLOG_INFO << "[ModelPoolManager] 初始化完成, 模型数: " << models_.size();
    return true;
}

std::shared_ptr<IModel> ModelPoolManager::get(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : it->second;
}

std::shared_ptr<IDetector> ModelPoolManager::getDetector(const std::string& name) const {
    return std::dynamic_pointer_cast<IDetector>(get(name));
}

std::shared_ptr<IClassifier> ModelPoolManager::getClassifier(const std::string& name) const {
    return std::dynamic_pointer_cast<IClassifier>(get(name));
}

std::shared_ptr<IDetector> ModelPoolManager::primaryDetector() const {
    for (const auto& m : models_) {
        if (m && m->role() == ModelRole::Detector) {
            return std::dynamic_pointer_cast<IDetector>(m);
        }
    }
    return nullptr;
}

std::shared_ptr<IDetector> ModelPoolManager::buildCascade(const CascadeConfig& cfg) const {
    // 1. 主筛: 指定名(若可用) 否则首个 detector
    std::shared_ptr<IDetector> primary;
    if (!cfg.primary.empty()) {
        primary = getDetector(cfg.primary);
        if (!primary) {
            CVLOG_WARN << "[ModelPoolManager] cascade.primary 未找到或非检测器: "
                       << cfg.primary << ", 回退到首个 detector。";
        }
    }
    if (!primary) primary = primaryDetector();
    if (!primary) {
        CVLOG_ERROR << "[ModelPoolManager] 级联构建失败: 无可用主检测器。";
        return nullptr;
    }

    // 2. 二级分类器(可选): 为空/未找到 => 级联退化为单模型
    std::shared_ptr<IClassifier> secondary;
    if (!cfg.secondary.empty()) {
        secondary = getClassifier(cfg.secondary);
        if (!secondary) {
            CVLOG_WARN << "[ModelPoolManager] cascade.secondary 未找到或非分类器: "
                       << cfg.secondary << ", 级联退化为单模型。";
        }
    }

    // 3. 组装(两者均以 shared_ptr 注入, 生命周期安全)
    auto cascade = std::make_shared<CascadeEngine>(primary, secondary, cfg);
    ModelConfig mc; // 仅用于登记级联名字
    mc.name = "cascade";
    cascade->init(mc);

    CVLOG_INFO << "[ModelPoolManager] 级联构建完成: primary=" << primary->name()
               << ", secondary="
               << (secondary ? secondary->name() : std::string("(无/单模型)"))
               << ", trigger_labels=" << cfg.trigger_labels.size()
               << ", gray_zone=[" << cfg.min_conf << "," << cfg.max_conf << ")";
    return cascade;
}

std::vector<std::string> ModelPoolManager::names() const {
    std::vector<std::string> out;
    out.reserve(models_.size());
    for (const auto& m : models_) {
        if (m) out.push_back(m->name());
    }
    return out;
}

void ModelPoolManager::close() {
    models_.clear();
    by_name_.clear();
}
