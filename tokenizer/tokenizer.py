# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      Inazuma AI Microservices
# File:         main.py
# Description:  AI 辅助微服务模块。
#               提供大模型 Token 预估、文本向量化 (Embedding)、
#               以及结合多维特征 (CrossEncoder, TF-IDF, Jaccard) 的
#               语义重排 (Rerank) 接口。
# ==============================================================================

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import List, Optional
import tiktoken
import uvicorn
import jieba.analyse
from sentence_transformers import SentenceTransformer, CrossEncoder

app = FastAPI(title="Inazuma AI Microservices (Enterprise Edition)")

# =========================================================================
# 全局缓存与模型加载
# =========================================================================
# Tokenizer 编码器实例缓存，避免频繁的实例初始化开销
encoding_cache = {}

print("[Init] 正在加载 Embedding 模型 (BAAI/bge-small-zh-v1.5)...")
embed_model = SentenceTransformer('BAAI/bge-small-zh-v1.5')

print("[Init] 正在加载 Reranker 模型 (BAAI/bge-reranker-v2-m3)...")
rerank_model = CrossEncoder('BAAI/bge-reranker-v2-m3')

# =========================================================================
# 基础算法与工具函数
# =========================================================================
# 计算两个字符串字面层面的 Jaccard 相似度 (交集元素数量与并集元素数量的比值)
def calculate_jaccard_sim(str1: str, str2: str) -> float:
    set1, set2 = set(str1), set(str2)
    intersection = len(set1.intersection(set2))
    union = len(set1.union(set2))
    return intersection / union if union > 0 else 0.0

# =========================================================================
# API 数据模型定义
# =========================================================================
class TokenizeRequest(BaseModel):
    messages: List[dict]

class EmbedRequest(BaseModel):
    text: str

class RerankRequest(BaseModel):
    query: str
    cached_query: str

# =========================================================================
# API 路由控制
# =========================================================================
# 服务健康状态探针接口
@app.get("/health")
async def health(): return {"status": "ok"}

# 计算给定消息数组在指定模型下的 Token 总量
@app.post("/tokenizer/{model_name}")
async def get_num_tokens(model_name: str, req: TokenizeRequest):
    if model_name not in encoding_cache:
        try: 
            encoding = tiktoken.encoding_for_model(model_name)
        except: 
            encoding = tiktoken.get_encoding("cl100k_base")
        encoding_cache[model_name] = encoding
    
    enc = encoding_cache[model_name]
    # 基于 OpenAI 标准的 Chat 格式 Token 计算公式进行累加
    num = sum(len(enc.encode(m['content'])) + 4 for m in req.messages) + 3
    return {"code": 200, "num_tokens": num}

# 执行文本的向量化处理，并执行维度补齐
@app.post("/embed")
async def get_embedding(req: EmbedRequest):
    vec = embed_model.encode(req.text, normalize_embeddings=True).tolist()
    # 统一输出向量维度至 1536 维，以兼容现有下游架构规范
    if len(vec) < 1536: vec.extend([0.0] * (1536 - len(vec)))
    return {"embedding": vec}

# 执行多维度语义重排评估
@app.post("/rerank")
async def get_rerank_score(req: RerankRequest):
    # 1. 神经网络语义基础打分
    raw_score = float(rerank_model.predict([[req.query, req.cached_query]]))
    final_score = raw_score
    
    # 2. 结合 TF-IDF 进行核心实体提取与对比
    # 配置提取策略：提取前 4 个关键词，仅保留具有实际语义的词性（n:名词, vn:名动词, v:动词, eng:英文）
    # 用于排除代词、副词、语气词等低信息熵词汇的干扰
    pos_filter = ('n', 'vn', 'v', 'eng')
    kw1 = set(jieba.analyse.extract_tags(req.query, topK=4, allowPOS=pos_filter))
    kw2 = set(jieba.analyse.extract_tags(req.cached_query, topK=4, allowPOS=pos_filter))
    
    # 计算核心实体的重合比例
    entity_sim = len(kw1 & kw2) / len(kw1 | kw2) if (kw1 | kw2) else 1.0

    # 实体偏离惩罚机制：当有效提取到实体，且重合比例跌破阈值 (0.25) 时，实施得分减半惩罚
    if (kw1 | kw2) and entity_sim <= 0.25:
        final_score *= 0.5
        print(f"[Rerank] 实体偏离惩罚触发: {kw1} vs {kw2} | 降分至 {final_score}")

    # 3. 基于 Jaccard 相似度的语序倒置惩罚
    # 用于过滤字面高度重合，但因语序变化导致语义发生反转的边界情况
    j_sim = calculate_jaccard_sim(req.query, req.cached_query)
    if j_sim > 0.9 and raw_score < 0.985:
        final_score = pow(final_score, 8)

    return {"score": max(0.0, final_score)}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8001, log_level="warning")