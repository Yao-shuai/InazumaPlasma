-- ==============================================================================
-- Copyright (c) 2026 Nexus_Yao. All rights reserved.
--
-- Project:      Inazuma AI Ecosystem
-- File:         init.sql
-- Description:  计费与鉴权数据库初始化脚本。
--               配置数据库字符集，构建用户配额表结构，并初始化默认系统管理员数据。
-- ==============================================================================

-- =========================================================================
-- 1. 数据库环境配置
-- =========================================================================
-- 配置数据库默认字符集为 utf8mb4，以完整支持多语言及 4 字节特殊字符 (如 Emoji)
ALTER DATABASE inazuma_chat CHARACTER SET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

USE inazuma_chat;

-- =========================================================================
-- 2. 核心表结构定义
-- =========================================================================
-- 构建用户账户与调用配额 (Quota) 关联表
CREATE TABLE IF NOT EXISTS users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(100) NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    quota INT NOT NULL DEFAULT 0,
    api_key VARCHAR(100) UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- =========================================================================
-- 3. 基础数据预置 (Seed Data)
-- =========================================================================
-- 初始化默认系统管理员账号，分配测试阶段所需的最高权限配额与预设 API Key
INSERT IGNORE INTO users (username, password_hash, quota, api_key) 
VALUES (
    'admin', 
    'admin123', 
    999999999, 
    'sk-inazuma-admin-super-key-001'
);