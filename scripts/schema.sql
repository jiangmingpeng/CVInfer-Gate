-- ============================================================
-- CVInfer-Gate 数据库表结构 (schema.sql)
-- ------------------------------------------------------------
-- 与 src/database/DBWriter.cpp 的批量插入语句严格对应:
--   detections: INSERT INTO detections (class_id, label, confidence, x1, y1, x2, y2)
--   alerts:     INSERT INTO alerts (alert_type, description)
--
-- 与 docker/init_db.sql 等价(此处额外指定 utf8mb4 与索引), 二者请保持同步。
-- 优点: 全部使用 IF NOT EXISTS, 可重复执行。
--
-- 用法(手动导入):
--   mysql -h <host> -u root -p < scripts/schema.sql
-- docker 环境: docker/init_db.sql 会在首次初始化数据卷时自动执行本结构。
-- ============================================================

CREATE DATABASE IF NOT EXISTS cv_infer
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE cv_infer;

-- ---- 检测记录表 (DBWriter 批量写入 Row::Type::Detection) ----
CREATE TABLE IF NOT EXISTS detections (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    class_id    INT         NOT NULL,                    -- DetectionResult::class_id
    label       VARCHAR(50) NOT NULL,                    -- DetectionResult::label
    confidence  FLOAT       NOT NULL,                    -- DetectionResult::confidence
    x1          INT         NOT NULL,                    -- box.x
    y1          INT         NOT NULL,                    -- box.y
    x2          INT         NOT NULL,                    -- box.x + box.width
    y2          INT         NOT NULL,                    -- box.y + box.height
    detect_time DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_detections_time  (detect_time),
    KEY idx_detections_label (label)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---- 告警记录表 (DBWriter::writeAlert -> Row::Type::Alert) ----
CREATE TABLE IF NOT EXISTS alerts (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    alert_type  VARCHAR(50)  NOT NULL,                   -- 如 "图书馆疑似占座违规"
    description VARCHAR(255) DEFAULT NULL,               -- 详细描述(可含中文)
    alert_time  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_alerts_time (alert_time),
    KEY idx_alerts_type (alert_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
